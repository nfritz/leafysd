/* Copyright (c) 2013 LeafLabs, LLC.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "control.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>             /* for sig_atomic_t */

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "logging.h"
#include "type_attrs.h"
#include "sockutil.h"
#include "control-private.h"
#include "control-client.h"
#include "control-dnode.h"

/* TODO use cached dnode_addr and dnode_c_port to establish periodic
 * reconnect handler to cover data node crashes. */

static void
control_fatal_err(const char *message, int code) /* code==-1 for "no code" */
{
    const char *msg = message ? message : "";
    const char *msgsep = message ? ": " : "";
    if (code > 0) {
        log_CRIT("fatal error (%d) in control session%s%s", code, msgsep, msg);
    } else {
        log_CRIT("fatal error in control session%s%s", msgsep, msg);
    }
    exit(EXIT_FAILURE);
}

/*
 * Client/dnode ops helpers
 */

static int control_client_start(struct control_session *cs)
{
    if (!control_client_ops->cs_start) {
        return 0;
    }
    return control_client_ops->cs_start(cs);
}

static void control_client_stop(struct control_session *cs)
{
    if (!control_client_ops->cs_stop) {
        return;
    }
    control_client_ops->cs_stop(cs);
}

static int control_client_open(struct control_session *cs,
                               evutil_socket_t sockfd)
{
    if (!control_client_ops->cs_open) {
        return 0;
    }
    return control_client_ops->cs_open(cs, sockfd);
}

static void control_client_close(struct control_session *cs)
{
    control_must_lock(cs);
    assert(cs->cbev);           /* or we never opened */
    bufferevent_free(cs->cbev);
    cs->cbev = NULL;
    if (cs->ctl_txns) {
        /* Hope you weren't in the middle of anything important... */
        log_INFO("halting data node I/O due to closed client connection");
        control_clear_transactions(cs, 1);
    }
    control_must_unlock(cs);
    if (control_client_ops->cs_close) {
        control_client_ops->cs_close(cs);
    }
}

static enum control_worker_why control_client_read(struct control_session *cs)
{
    if (!control_client_ops->cs_read) {
        return CONTROL_WHY_NONE;
    }
    return control_client_ops->cs_read(cs);
}

static void control_client_thread(struct control_session *cs)
{
    if (!control_client_ops->cs_thread) {
        return;
    }
    control_client_ops->cs_thread(cs);
}

static int control_dnode_start(struct control_session *cs)
{
    if (!control_dnode_ops->cs_start) {
        return 0;
    }
    return control_dnode_ops->cs_start(cs);
}

static void control_dnode_stop(struct control_session *cs)
{
    if (!control_dnode_ops->cs_stop) {
        return;
    }
    control_dnode_ops->cs_stop(cs);
}

static int control_dnode_open(struct control_session *cs,
                              evutil_socket_t sockfd)
{
    if (!control_dnode_ops->cs_open) {
        return 0;
    }
    return control_dnode_ops->cs_open(cs, sockfd);
}

static void control_dnode_close(struct control_session *cs)
{
    control_must_lock(cs);
    assert(cs->dbev);           /* or we never opened */
    bufferevent_free(cs->dbev);
    cs->dbev = NULL;
    if (cs->ctl_txns) {
        /* FIXME if there are ongoing transactions, then the client
         * connection should also be open; we should get the
         * client-side code to send an error response (how?), or naive
         * client will block forever. */
        log_INFO("halting data node I/O due to closed dnode connection");
        control_clear_transactions(cs, 1);
    }
    control_must_unlock(cs);
    if (control_dnode_ops->cs_close) {
        control_dnode_ops->cs_close(cs);
    }
}

static enum control_worker_why control_dnode_read(struct control_session *cs)
{
    if (!control_dnode_ops->cs_read) {
        return CONTROL_WHY_NONE;
    }
    return control_dnode_ops->cs_read(cs);
}

static void control_dnode_thread(struct control_session *cs)
{
    if (!control_dnode_ops->cs_thread) {
        return;
    }
    control_dnode_ops->cs_thread(cs);
}

/*
 * Other helpers
 */

static struct evconnlistener*
control_new_listener(struct control_session *cs,
                     struct event_base *base, uint16_t port,
                     evconnlistener_cb cb, evconnlistener_errorcb err_cb)
{
    int sockfd = sockutil_get_tcp_passive(port, 1);
    if (sockfd == -1) {
        log_ERR("can't make socket: %m");
        return NULL;
    }
    evutil_make_socket_nonblocking(sockfd);
    unsigned flags = (LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE |
                      LEV_OPT_THREADSAFE);
    struct evconnlistener *ecl = evconnlistener_new(base, cb, cs,
                                                    flags, 0, sockfd);
    if (!ecl) {
        log_ERR("can't allocate evconnlistener");
        close(sockfd);
        return NULL;
    }
    evconnlistener_set_error_cb(ecl, err_cb);
    return ecl;
}

/* Returned bufferevent comes back disabled */
static struct bufferevent*
control_new_bev(struct control_session *cs, evutil_socket_t fd,
                bufferevent_data_cb readcb, bufferevent_data_cb writecb,
                bufferevent_event_cb eventcb)
{
    int bev_opts = BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE;
    struct bufferevent *ret = bufferevent_socket_new(control_get_base(cs),
                                                     fd, bev_opts);
    if (!ret) {
        return NULL;
    }
    bufferevent_disable(ret, bufferevent_get_enabled(ret));
    bufferevent_setcb(ret, readcb, writecb, eventcb, cs);
    return ret;
}

/* You don't have to use this if you've already got the lock */
static void control_set_wake(struct control_session *cs,
                             enum control_worker_why why)
{
    control_must_lock(cs);
    cs->wake_why |= why;
    control_must_unlock(cs);
}

/* You don't have to use this if you've got the lock */
static void control_must_wake(struct control_session *cs,
                              enum control_worker_why why)
{
    control_set_wake(cs, why);
    control_must_signal(cs);
}

/*
 * Worker thread
 */

static void* control_worker_main(void *csessvp)
{
    struct control_session *cs = csessvp;
    while (1) {
        control_must_lock(cs);
        while (cs->wake_why == CONTROL_WHY_NONE) {
            control_must_cond_wait(cs);
        }
        assert(cs->wake_why != CONTROL_WHY_NONE);
        if (cs->wake_why & CONTROL_WHY_EXIT) {
            control_must_unlock(cs);
            pthread_exit(NULL);
        }
        if (cs->wake_why & (CONTROL_WHY_CLIENT_CMD | CONTROL_WHY_CLIENT_RES)) {
            control_client_thread(cs);
        }
        if (cs->wake_why & CONTROL_WHY_DNODE_TXN) {
            control_dnode_thread(cs);
        }
        control_must_unlock(cs);
    }
    control_fatal_err("control exiting unexpectedly", -1);
    return NULL; /* appease GCC */
}

/*
 * libevent plumbing
 */

static void control_bevt_handler(struct control_session *cs, short events,
                                 void (*on_close)(struct control_session*),
                                 const char *log_who)
{
    if (events & BEV_EVENT_EOF || events & BEV_EVENT_ERROR) {
        on_close(cs);
        log_INFO("%s connection closed", log_who);
    } else {
        log_WARNING("unhandled %s event; flags %d", log_who, events);
    }
}

static void control_client_event(__unused struct bufferevent *bev,
                                 short events, void *csessvp)
{
    struct control_session *cs = csessvp;
    assert(bev == cs->cbev);
    control_bevt_handler(cs, events, control_client_close, "client");
}

static void control_dnode_event(__unused struct bufferevent *bev,
                                short events, void *csessvp)
{
    /* FIXME install periodic event that tries to re-establish a
     * closed dnode connection */
    struct control_session *cs = csessvp;
    assert(bev == cs->dbev);
    control_bevt_handler(cs, events, control_dnode_close, "data node");
}

static void refuse_connection(evutil_socket_t fd,
                              const char *source, const char *cause)
{
    const char *c = cause ? cause : "unknown error";
    log_INFO("refusing new %s connection: %s", source, c);
    if (evutil_closesocket(fd)) {
        log_ERR("couldn't close new %s", source);
    }
}

static void
control_conn_open(struct control_session *cs,
                  struct bufferevent **bevp,
                  evutil_socket_t fd,
                  bufferevent_data_cb read,
                  bufferevent_data_cb write,
                  bufferevent_event_cb event,
                  int (*on_open)(struct control_session*,
                                 evutil_socket_t),
                  const char *log_who)
{
    if (*bevp) {
        refuse_connection(fd, log_who, "another is ongoing");
        return;
    }

    struct bufferevent *bev = control_new_bev(cs, fd, read, write, event);
    bufferevent_disable(bev, bufferevent_get_enabled(bev));
    if (!bev) {
        log_ERR("can't allocate resources for %s connection", log_who);
        return;
    }
    control_must_lock(cs);
    *bevp = bev;
    control_must_unlock(cs);
    if (on_open(cs, fd) == -1) {
        refuse_connection(fd, log_who, NULL);
        bufferevent_free(bev);
        *bevp = NULL;
    } else {
        bufferevent_enable(bev, EV_READ | EV_WRITE);
    }
    log_INFO("%s connection established", log_who);
}

static void
control_bev_reader(struct control_session *cs,
                   enum control_worker_why (*reader)(struct control_session*),
                   __unused const char *log_who)
{
    enum control_worker_why read_why_wake = reader(cs);
    switch (read_why_wake) {
    case CONTROL_WHY_NONE:
        break;
    case CONTROL_WHY_EXIT:
        log_CRIT("%s socket reader wants to shut down the worker", log_who);
        control_fatal_err("error reading from bufferevent", -1);
        break;
    default:
        control_must_wake(cs, read_why_wake);
        break;
    }
}

static void control_client_bev_read(__unused struct bufferevent *bev,
                                    void *csessvp)
{
    struct control_session *cs = csessvp;
    control_bev_reader(cs, control_client_read, "client");
}

static void control_dnode_bev_read(__unused struct bufferevent *bev,
                                   void *csessvp)
{
    struct control_session *cs = csessvp;
    control_bev_reader(cs, control_dnode_read, "data node");
}

static void client_ecl(__unused struct evconnlistener *ecl, evutil_socket_t fd,
                       __unused struct sockaddr *saddr, __unused int socklen,
                       void *csessvp)
{
    struct control_session *cs = csessvp;
    control_conn_open(cs, &cs->cbev, fd, control_client_bev_read, NULL,
                      control_client_event, control_client_open, "client");
}

static void client_ecl_err(__unused struct evconnlistener *ecl,
                           __unused void *csessvp)
{
    log_ERR("client accept() failed: %m");
}

static void control_sample(evutil_socket_t sockfd,
                           short events,
                           void *csessvp)
{
    struct control_session *cs = csessvp;
    if (sockfd != cs->ddatafd) {
        log_ERR("got data from socket %d, expecting %d",
                sockfd, cs->cdatafd);
        return;
    }
    if ((events & EV_READ) && (cs->cdatafd == -1)) {
        log_WARNING("received data from daemon, but no one wants it; "
                    "dropping the packet");
        struct sockaddr_storage dn_addr;
        socklen_t len = sizeof(struct sockaddr_storage);
        recvfrom(cs->ddatafd, NULL, 0, 0, (struct sockaddr*)&dn_addr, &len);
        return;
    }
}

/*
 * Public API
 */

struct control_session* control_new(struct event_base *base,
                                    uint16_t client_port,
                                    const char* dnode_addr,
                                    uint16_t dnode_port,
                                    uint16_t sample_port)
{
    int en;
    struct control_session *cs = malloc(sizeof(struct control_session));
    if (!cs) {
        log_ERR("out of memory");
        goto nocs;
    }
    cs->base = base;
    struct evconnlistener *cecl =
        control_new_listener(cs, base, client_port, client_ecl,
                             client_ecl_err);
    if (!cecl) {
        log_ERR("can't listen for client connections");
        goto nocecl;
    }
    cs->daddr = dnode_addr;
    cs->dport = dnode_port;
    int ddatafd = sockutil_get_tcp_connected_p(cs->daddr, cs->dport);
    if (ddatafd == -1) {
        log_ERR("can't connect to data node at %s, port %u",
                dnode_addr, dnode_port);
        goto nodsock;
    }
    if (evutil_make_socket_nonblocking(ddatafd)) {
        log_ERR("data node control socket doesn't support nonblocking I/O");
        goto nodnonblock;
    }
    en = pthread_mutex_init(&cs->mtx, NULL);
    if (en) {
        log_ERR("threading error while initializing control session");
        goto nomtx;
    }
    en = pthread_cond_init(&cs->cv, NULL);
    if (en) {
        log_ERR("threading error while initializing control session");
        goto nocv;
    }
    cs->wake_why = CONTROL_WHY_NONE;
    cs->cecl = cecl;
    cs->cbev = NULL;
    if (control_client_start(cs)) {
        log_ERR("can't start client side of control session");
        goto noclient;
    }
    cs->dbev = NULL;
    if (control_dnode_start(cs)) {
        log_ERR("can't start data node side of control session");
        goto nodnode;
    }
    control_conn_open(cs, &cs->dbev, ddatafd, control_dnode_bev_read,
                      NULL, control_dnode_event, control_dnode_open,
                      "data node");
    cs->ddatafd = sockutil_get_udp_socket(sample_port);
    if (cs->ddatafd == -1) {
        log_ERR("can't create daemon data socket");
        goto nobsock;
    }
    if (evutil_make_socket_nonblocking(cs->ddatafd) == -1) {
        log_ERR("nobnonblock");
        goto nobnonblock;
    }
    cs->cdatafd = -1;  /* TODO support for forwarding subsamples */
    cs->ddataevt = event_new(base, cs->ddatafd, EV_READ | EV_PERSIST,
                         control_sample, cs);
    if (!cs->ddataevt) {
        log_ERR("noddataevt");
        goto noddataevt;
    }
    event_add(cs->ddataevt, NULL);
    cs->ctl_txns = NULL;
    cs->ctl_n_txns = 0;
    cs->ctl_cur_txn = -1;
    cs->ctl_cur_rid = 0;
    control_must_lock(cs);
    en = pthread_create(&cs->thread, NULL, control_worker_main, cs);
    control_must_unlock(cs);
    if (en) {
        log_ERR("noworker");
        goto noworker;
    }
    return cs;

 noworker:
    event_free(cs->ddataevt);
 noddataevt:
 nobnonblock:
    if (evutil_closesocket(cs->ddatafd) == -1) {
        log_ERR("can't close sample socket: %m");
    }
 nobsock:
    if (cs->dbev) {
        bufferevent_free(cs->dbev);
    }
    control_dnode_stop(cs);
 nodnode:
    control_client_stop(cs);
 noclient:
    en = pthread_cond_destroy(&cs->cv);
    if (en) {
        control_fatal_err("can't destroy cvar", en);
    }
 nocv:
    en = pthread_mutex_destroy(&cs->mtx);
    if (en) {
        control_fatal_err("can't destroy cs mutex", en);
    }
 nomtx:
 nodnonblock:
    if (evutil_closesocket(ddatafd) == -1) {
        log_ERR("can't close data node socket: %m");
    }
 nodsock:
    evconnlistener_free(cecl);
 nocecl:
    free(cs);
 nocs:
    return NULL;
}

void control_free(struct control_session *cs)
{
    /* Acquired in control_new() */
    control_must_wake(cs, CONTROL_WHY_EXIT);
    control_must_join(cs, NULL);
    if (cs->ddataevt) {
        event_free(cs->ddataevt);
    }
    if (cs->cdatafd != -1 && evutil_closesocket(cs->cdatafd) == -1) {
        log_ERR("can't close client data socket: %m");
    }
    if (cs->ddatafd != -1 && evutil_closesocket(cs->ddatafd) == -1) {
        log_ERR("can't close sample socket: %m");
    }
    control_dnode_stop(cs);
    control_client_stop(cs);
    pthread_cond_destroy(&cs->cv);
    pthread_mutex_destroy(&cs->mtx);
    if (cs->dbev) {
        bufferevent_free(cs->dbev);
    }
    evconnlistener_free(cs->cecl);

    /* Possibly acquired elsewhere */
    if (cs->cbev) {
        bufferevent_free(cs->cbev);
    }
    if (cs->ctl_txns) {
        free(cs->ctl_txns);
    }

    free(cs);
}

struct event_base* control_get_base(struct control_session *cs)
{
    return cs->base;
}

/*
 * Private API
 */

void control_set_transactions(struct control_session *cs,
                              struct control_txn *txns, size_t n_txns,
                              int have_lock)
{
    if (!have_lock) {
        control_must_lock(cs);
    }
    /* You're not allowed to set up new transactions while existing
     * ones are ongoing, only to clear them. */
    assert((cs->ctl_txns == NULL &&
            cs->ctl_cur_txn == -1 &&
            cs->ctl_n_txns == 0) ||
           (txns == NULL && n_txns == 0));
    if (cs->ctl_txns) {
        free(cs->ctl_txns);
    }
    cs->ctl_txns = txns;
    cs->ctl_n_txns = n_txns;
    if (n_txns == 0) {
        cs->ctl_cur_txn = -1;
        goto done;
    }
    cs->ctl_cur_txn = 0;
    for (size_t i = 0; i < n_txns; i++) {
        ctxn_req(&txns[i])->r_id = cs->ctl_cur_rid++;
    }
 done:
    if (!have_lock) {
        control_must_unlock(cs);
    }
}

void __control_must_cond_wait(struct control_session *cs)
{
    int en = pthread_cond_wait(&cs->cv, &cs->mtx);
    if (en) {
        control_fatal_err("can't wait on next message", en);
    }
}

void __control_must_signal(struct control_session *cs)
{
    int en = pthread_cond_signal(&cs->cv);
    if (en) {
        control_fatal_err("can't signal cvar", en);
    }
}

void __control_must_lock(struct control_session *cs)
{
    int en = pthread_mutex_lock(&cs->mtx);
    if (en) {
        control_fatal_err("can't lock control thread", en);
    }
}

void __control_must_unlock(struct control_session *cs)
{
    int en = pthread_mutex_unlock(&cs->mtx);
    if (en) {
        control_fatal_err("can't unlock control thread", en);
    }
}

void __control_must_join(struct control_session *cs, void **retval)
{
    void *rv;
    if (!retval) {
        retval = &rv;
    }
    int en = pthread_join(cs->thread, retval);
    if (en) {
        control_fatal_err("can't join with control thread", en);
    }
}
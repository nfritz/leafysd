/* Copyright (c) 2013 LeafLabs, LLC. All rights reserved. */

#include "raw_packets.h"

#include <errno.h>

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>

#define PACKET_HEADER_MAGIC      0x5A
#define PACKET_HEADER_PROTO_VERS 0x00

void raw_packet_init(struct raw_packet *packet,
                     uint8_t type, uint8_t flags)
{
    packet->_p_magic = PACKET_HEADER_MAGIC;
    packet->_p_proto_vers = PACKET_HEADER_PROTO_VERS;
    packet->p_type = type;
    packet->p_flags = flags;
}

struct raw_packet* raw_packet_create_bsamp(uint16_t nchips, uint16_t nlines)
{
    /* This is pretty chummy with the packet type implementation,
     * which is a bit brittle. */
    size_t bsamp_size = (offsetof(struct raw_packet, p) +
                         offsetof(struct raw_msg_bsamp, bs_samples) +
                         (size_t)nchips * (size_t)nlines * sizeof(raw_samp_t));
    struct raw_packet *ret = malloc(bsamp_size);
    if (ret != 0) {
        raw_packet_init(ret, RAW_PKT_TYPE_BSAMP, 0);
        ret->p.bsamp.bs_nchips = nchips;
        ret->p.bsamp.bs_nlines = nlines;
    }
    return ret;
}

void raw_packet_copy(struct raw_packet *dst, struct raw_packet *src)
{
    memcpy(dst, src, offsetof(struct raw_packet, p));
    switch (src->p_type) {
    case RAW_PKT_TYPE_BSAMP:
        memcpy((uint8_t*)dst + offsetof(struct raw_packet, p),
               (uint8_t*)src + offsetof(struct raw_packet, p),
               sizeof(struct raw_msg_bsamp) + raw_packet_sampsize(src));
        break;
    case RAW_PKT_TYPE_REQ:      /* fall through */
    case RAW_PKT_TYPE_RES:
        memcpy((uint8_t*)dst + offsetof(struct raw_packet, p),
               (uint8_t*)src + offsetof(struct raw_packet, p),
               sizeof(struct raw_msg_req));
        break;
    case RAW_PKT_TYPE_ERR:
        break;
    default:
        assert(0 && "invalid packet type");
    }
}

static void raw_msg_bsamp_hton(struct raw_msg_bsamp *msg)
{
    for (size_t i = 0; i < raw_bsamp_nsamps(msg); i++) {
        msg->bs_samples[i] = htons(msg->bs_samples[i]);
    }
    msg->bs_idx = htonl(msg->bs_idx);
    msg->bs_nchips = htons(msg->bs_nchips);
    msg->bs_nlines = htons(msg->bs_nlines);
}

static void raw_msg_req_hton(struct raw_msg_req *msg)
{
    msg->r_id = htons(msg->r_id);
    msg->r_val = htonl(msg->r_val);
}

static void raw_msg_res_hton(struct raw_msg_res *msg)
{
    raw_msg_req_hton((struct raw_msg_req*)msg);
}

ssize_t raw_packet_send(int sockfd, struct raw_packet *packet, int flags)
{
    size_t packet_size = sizeof(struct raw_packet);
    switch (packet->p_type) {
    case RAW_PKT_TYPE_BSAMP:
        packet_size += raw_packet_sampsize(packet);
        raw_msg_bsamp_hton(&packet->p.bsamp);
        break;
    case RAW_PKT_TYPE_REQ:
        raw_msg_req_hton(&packet->p.req);
        break;
    case RAW_PKT_TYPE_RES:
        raw_msg_res_hton(&packet->p.res);
        break;
    case RAW_PKT_TYPE_ERR:
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    return send(sockfd, packet, packet_size, flags);
}

static void raw_msg_bsamp_ntoh(struct raw_msg_bsamp *msg)
{
    msg->bs_idx = ntohl(msg->bs_idx);
    msg->bs_nchips = ntohs(msg->bs_nchips);
    msg->bs_nlines = ntohs(msg->bs_nlines);
    for (size_t i = 0; i < raw_bsamp_nsamps(msg); i++) {
        msg->bs_samples[i] = ntohs(msg->bs_samples[i]);
    }
}

static void raw_msg_req_ntoh(struct raw_msg_req *msg)
{
    msg->r_id = ntohs(msg->r_id);
    msg->r_val = ntohl(msg->r_val);
}

static void raw_msg_res_ntoh(struct raw_msg_res *msg)
{
    raw_msg_req_ntoh((struct raw_msg_req*)msg);
}

ssize_t raw_packet_recv(int sockfd, struct raw_packet *packet,
                        uint8_t *packtype, int flags)
{
    size_t packsize = sizeof(struct raw_packet);
    uint8_t pt = 0;
    if (packtype == NULL) {
        packtype = &pt;
    }
    if (*packtype == RAW_PKT_TYPE_BSAMP) {
        packsize += raw_packet_sampsize(packet);
    }
    int ret = recv(sockfd, packet, packsize, flags);
    if (packet->_p_magic != PACKET_HEADER_MAGIC) {
        errno = EPROTO;
        return -1;
    }
    if (ret == -1) {
        return -1;
    }
    if (*packtype == 0) {
        *packtype = packet->p_type;
    } else if (*packtype != packet->p_type) {
        errno = EIO;
        return -1;
    }
    switch (*packtype) {
    case RAW_PKT_TYPE_BSAMP:
        raw_msg_bsamp_ntoh(&packet->p.bsamp);
        break;
    case RAW_PKT_TYPE_REQ:
        raw_msg_req_ntoh(&packet->p.req);
        break;
    case RAW_PKT_TYPE_RES:
        raw_msg_res_ntoh(&packet->p.res);
        break;
    case RAW_PKT_TYPE_ERR:
        break;
    default:
        errno = EPROTO;
        return -1;
    }

    return ret;
}
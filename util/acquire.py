#!/usr/bin/env python

from __future__ import print_function
import argparse
import collections
import os.path
import sys
from time import time

from daemon_control import *

##
## Command handling
##

BACKENDS = { 'STORE_HDF5': STORE_HDF5,
             'STORE_RAW': STORE_RAW }

BSI_INTERVAL = 1920

# parameters used in subsample determination
CHANNELS_PER_CHIP = 32
CHIPS_PER_DATANODE = 32


def acquire(enable, args):
    cmd = ControlCommand(type=ControlCommand.ACQUIRE)
    if enable:
        cmd.acquire.exp_cookie = long(time())
        start_sample = args.start_sample
        r = start_sample % BSI_INTERVAL
        if r != 0:
            start_sample += (BSI_INTERVAL-r)
            print('Rounding start_sample up to next multiple of %d:\n'
                   '\tstart_sample = %d'%(BSI_INTERVAL, start_sample))
        cmd.acquire.start_sample = start_sample
    cmd.acquire.enable = enable
    return [cmd]

def dump_err_regs(args):
    return read_err_regs()

def start(args):
    return acquire(True, args)

def stop(args):
    return acquire(False, args)

def save_stored(args):
    fpath = os.path.abspath(args.file)
    cmd = ControlCommand(type=ControlCommand.STORE)
    cmd.store.start_sample = args.start_sample
    cmd.store.nsamples = args.nsamples
    cmd.store.path = fpath
    if args.backend is not None:
        cmd.store.backend = BACKENDS[args.backend]
    return [cmd]

def save_stream(args):
    fpath = os.path.abspath(args.file)
    nsamples = args.nsamples
    cmd = ControlCommand(type=ControlCommand.STORE)
    cmd.store.path = os.path.abspath(fpath)
    cmd.store.nsamples = nsamples
    if args.backend is not None:
        cmd.store.backend = BACKENDS[args.backend]
    return [cmd]

def forward(args):
    cmd = ControlCommand(type=ControlCommand.FORWARD)
    if args.type == 'sample':
        cmd.forward.sample_type = BOARD_SAMPLE
    elif args.type == 'subsample':
        cmd.forward.sample_type = BOARD_SUBSAMPLE
    elif args.type == 'sample_raw':
        cmd.forward.sample_type = BOARD_SAMPLE_RAW
    elif args.type == 'subsample_raw':
        cmd.forward.sample_type = BOARD_SUBSAMPLE_RAW
    else:
        print('Invalid sample type:', args.type, file=sys.stderr)
        sys.exit(1)
    if args.force_daq_reset:
        cmd.forward.force_daq_reset = True
    try:
        aton = socket.inet_aton(args.address)
    except socket.error:
        print('Invalid address', args.address, file=sys.stderr)
        sys.exit(1)
    cmd.forward.dest_udp_addr4 = struct.unpack('!l', aton)[0]
    if args.port <= 0 or args.port >= 0xFFFF:
        print('Invalid port', args.port, file=sys.stderr)
        sys.exit(1)
    cmd.forward.dest_udp_port = args.port
    cmd.forward.enable = (args.enable == 'start')
    return [cmd]

def subsamples(args):
    constant = args.constant
    number = args.number
    chipchanList = []
    if constant == 'chip':
        chip = number
        chipchanList = [(chip, chan) for chan in range(CHANNELS_PER_CHIP)]
    elif  constant == 'channel':
        chan = number
        chipchanList = [(chip, chan) for chip in range(CHIPS_PER_DATANODE)]
    else:
        raise ValueError("Don't know how to hold constant '%s'" % constant)
    print("Setting sub-sample channels as:")
    print()
    print("\tindex\tchip\tchannel")
    for i,chipchan in enumerate(chipchanList):
        print("\t%3d\t%3d\t%3d" % (i, chipchan[0], chipchan[1]))
    cmdlist = []
    for i,chipchan in enumerate(chipchanList):
        chip = chipchan[0] & 0b00011111
        chan = chipchan[1] & 0b00011111
        cmdlist.append(reg_write(MOD_DAQ, DAQ_SUBSAMP_CHIP0+i,
                       (chip << 8) | chan))
    return cmdlist


##
## Argument parsing
##

DEFAULT_START_SAMPLE = 0
DEFAULT_NSAMPLES = 0 # default behavior: read until experiment cookie changes


start_parser = argparse.ArgumentParser(
    prog='start',
    description='Start acquiring to node disk and streaming live data to '
                'daemon')

start_parser.add_argument(
    '-s', '--start_sample',
    type=int,
    default=DEFAULT_START_SAMPLE,
    help='Board sample index (BSI) at which to start acquiring. Must be a '
         'multiple of %d, default is %d.'%(BSI_INTERVAL, DEFAULT_START_SAMPLE))

BACKEND_CHOICES = ['STORE_HDF5', 'STORE_RAW']

def no_arg_parser(cmd, description):
    return argparse.ArgumentParser(prog=cmd, description=description)

save_stored_parser = argparse.ArgumentParser(
    prog='save_stored',
    description='Copy experiment data from node disk to file on daemon '
                'computer (after stopping acquisition)',
    epilog="""DO NOT USE THIS WHILE ACQUISITION IS ONGOING.""")

save_stored_parser.add_argument('file',
                                help='File to store samples in.')

save_stored_parser.add_argument(
    '-s', '--start_sample',
    type=int,
    default=DEFAULT_START_SAMPLE,
    help='Start sample (default %d)' % DEFAULT_START_SAMPLE)

save_stored_parser.add_argument(
    '-n', '--nsamples',
    type=int,
    default=DEFAULT_NSAMPLES,
    help='Number of samples (default %d)' % DEFAULT_NSAMPLES)

save_stored_parser.add_argument(
    '-b', '--backend',
    default=None,
    choices=BACKEND_CHOICES,
    help='Storage backend')


save_stream_parser = argparse.ArgumentParser(
    prog='save_stream',
    description='Save live streaming data to disk on daemon computer')
save_stream_parser.add_argument('file',
                                help='File to store samples in')
save_stream_parser.add_argument('nsamples', type=int,
                                help='Number of samples to try storing')
save_stream_parser.add_argument(
    '-b', '--backend',
    default=None,
    choices=BACKEND_CHOICES,
    help='Storage backend')

subsamples_parser = argparse.ArgumentParser(
    prog='subsamples',
    description='Configure which channels make up the subsample')
subsamples_parser.add_argument("--constant",
    choices=['chip','channel'],
    required=True,
    help="what to hold constant")
subsamples_parser.add_argument("number",
    type=int,
    help="the value for the index being held constant")

DEFAULT_FORWARD_ADDR = '127.0.0.1'
DEFAULT_FORWARD_PORT = 7654      # for proto2bytes
DEFAULT_FORWARD_TYPE = 'sample'
forward_parser = argparse.ArgumentParser(
    prog='forward',
    description='Control live stream data forwarding',
    epilog='Enable/disable forwarding real-time data to another program.')
forward_parser.add_argument(
    '-f', '--force-daq-reset',
    default=False,
    action='store_true',
    help='[DANGEROUS] force DAQ module reset')
forward_parser.add_argument(
    '-t', '--type',
    choices=['sample', 'subsample', 'sample_raw', 'subsample_raw'],
    default=DEFAULT_FORWARD_TYPE,
    help='Type of packets to forward (default %s)' % DEFAULT_FORWARD_TYPE)
forward_parser.add_argument(
    '-a', '--address',
    default=DEFAULT_FORWARD_ADDR,
    help=('Address to forward packets to, default %s' %
          DEFAULT_FORWARD_ADDR))
forward_parser.add_argument(
    '-p', '--port',
    default=DEFAULT_FORWARD_PORT,
    help=('Port to forward packets to, default %s' %
          DEFAULT_FORWARD_PORT))
forward_parser.add_argument(
    'enable',
    choices=['start', 'stop'],
    help='Start or stop live sample forwarding')

def nop_resp_map(resps):
    return resps

COMMAND_HANDLING = collections.OrderedDict()
COMMAND_HANDLING['start'] = (
    start,
    start_parser,
    nop_resp_map)
COMMAND_HANDLING['save_stream'] = (
    save_stream,
    save_stream_parser,
    nop_resp_map)
COMMAND_HANDLING['stop'] = (
    stop,
    no_arg_parser('stop', 'Stop acquiring to node disk, and stop streaming'),
    nop_resp_map)
COMMAND_HANDLING['save_stored'] = (
    save_stored,
    save_stored_parser,
    nop_resp_map)
COMMAND_HANDLING['forward'] = (
    forward,
    forward_parser,
    nop_resp_map)
COMMAND_HANDLING['dump_err_regs'] = (
    dump_err_regs,
    no_arg_parser('dump_err_regs', 'Print nonzero error registers'),
    lambda resps: [r for r in resps if r.reg_io.val != 0])
COMMAND_HANDLING['subsamples'] = (
    subsamples,
    subsamples_parser,
    nop_resp_map)

##
## main()
##

def usage():
    print('usage: acquire.py command [[-h] command_args ...]\n\n'
          'Commands:\n\n'
          '%s' % ''.join(['   %s: %s\n' % (s, h_p[1].description)
                          for s, h_p in COMMAND_HANDLING.iteritems()]),
          end='', file=sys.stderr)
    sys.exit(1)

def main():
    if len(sys.argv) < 2:
        usage()
    cmd, cmd_args = sys.argv[1], sys.argv[2:]
    if cmd not in COMMAND_HANDLING:
        usage()
    handler, parser, rmap = COMMAND_HANDLING[cmd]
    cmds = handler(parser.parse_args(cmd_args))
    resps = do_control_cmds(cmds)
    if resps is None:
        print("Didn't get a response", file=sys.stderr)
        sys.exit(1)
    for resp in rmap(resps):
        print(resp, end='')
        if resp.type == ControlResponse.ERR:
            sys.exit(1)
    sys.exit(0)

if __name__ == '__main__':
    main()

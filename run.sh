#!/bin/sh
set -e

usage() {
    cat <<EOF
Usage: $(basename "$0") -c-udp | -c-tcp | -py-udp | -py-tcp

  -c-udp    compile and run C version, UDP mode
  -c-tcp    compile and run C version, TCP mode
  -py-udp   run Python version, UDP mode
  -py-tcp   run Python version, TCP mode
EOF
    exit 1
}

[ $# -eq 1 ] || usage

case "$1" in
    -c-udp)
        cc -Wall -Wextra -O2 -o ephemeral_ports ephemeral_ports.c
        ulimit -n 65536
        ./ephemeral_ports --udp
        ;;
    -c-tcp)
        cc -Wall -Wextra -O2 -o ephemeral_ports ephemeral_ports.c
        ulimit -n 65536
        ./ephemeral_ports --tcp
        ;;
    -py-udp)
        ulimit -n 65536
        python3 ephemeral_ports.py --udp
        ;;
    -py-tcp)
        ulimit -n 65536
        python3 ephemeral_ports.py --tcp
        ;;
    *)
        usage
        ;;
esac

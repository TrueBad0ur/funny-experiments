#!/bin/sh
set -e

usage() {
    cat <<EOF
Usage: $(basename "$0") -c | -py

  -c    compile and run C version (ephemeral_ports.c)
  -py   run Python version (ephemeral_ports.py)
EOF
    exit 1
}

[ $# -eq 1 ] || usage

case "$1" in
    -c)
        cc -Wall -Wextra -O2 -o ephemeral_ports ephemeral_ports.c
        ulimit -n 65536
        ./ephemeral_ports
        ;;
    -py)
        ulimit -n 65536
        python3 ephemeral_ports.py
        ;;
    *)
        usage
        ;;
esac

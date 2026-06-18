#!/usr/bin/env python3
"""
ephemeral_ports.py — ephemeral port exhaustion demo

Run:  ulimit -n 65536 && python3 ephemeral_ports.py

Uses UDP connect() — kernel assigns src_port without sending any packets.
When all ports to DST1 are exhausted → EADDRNOTAVAIL.
Then shows that connecting to DST2 (different dst) still works.

Compatible: Linux, OpenBSD.
"""

import errno
import os
import resource
import socket
import subprocess
import sys

DST1 = ("203.0.113.1", 80)   # RFC 5737 TEST-NET-3, not routable
DST2 = ("198.51.100.1", 80)  # RFC 5737 TEST-NET-2, different dst


def raise_fd_limit():
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    print(f"fd limit: cur={soft} max={hard}", end="")
    if soft < hard:
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
            soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
            print(f" → raised to {soft}", end="")
        except ValueError:
            pass
    print()
    if soft < 32768:
        print("WARN: fd limit too low, run: ulimit -n 65536", file=sys.stderr)


def show_port_range():
    try:
        with open("/proc/sys/net/ipv4/ip_local_port_range") as f:
            lo, hi = f.read().split()
        print(f"ephemeral range: {lo}–{hi}  (~{int(hi) - int(lo)} ports per dst)")
        return
    except FileNotFoundError:
        pass
    # OpenBSD
    try:
        lo = subprocess.check_output(
            ["sysctl", "-n", "net.inet.ip.portrange.first"], text=True
        ).strip()
        hi = subprocess.check_output(
            ["sysctl", "-n", "net.inet.ip.portrange.last"], text=True
        ).strip()
        print(f"ephemeral range: {lo}–{hi}  (~{int(hi) - int(lo)} ports per dst)")
    except Exception:
        print("ephemeral range: could not determine (check sysctl)")


def exhaust_to(dst):
    sockets = []
    print(f"\n=== Phase 1: exhaust ports to {dst[0]}:{dst[1]} ===")

    while True:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(dst)
            sockets.append(s)
            n = len(sockets)
            if n % 2000 == 0:
                print(f"  {n:5d} sockets  last src_port={s.getsockname()[1]}")
                sys.stdout.flush()
        except OSError as e:
            s.close()
            print(f"\nconnect() #{len(sockets)}: {e.strerror} (errno {e.errno})")
            if e.errno == errno.EADDRNOTAVAIL:
                print(f"→ EADDRNOTAVAIL: all ephemeral ports to {dst[0]}:{dst[1]} exhausted")
            elif e.errno == errno.EMFILE:
                print("→ EMFILE: hit fd limit, run: ulimit -n 65536")
            break

    print(f"\nAllocated {len(sockets)} ports to {dst[0]}:{dst[1]}")
    return sockets


def try_other_dst(dst):
    print(f"\n=== Phase 2: try DIFFERENT dst {dst[0]}:{dst[1]} ===")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(dst)
        src_port = s.getsockname()[1]
        print(f"SUCCESS: src_port={src_port}")
        print("→ port exhaustion is per (dst_ip, dst_port), not global")
    except OSError as e:
        print(f"FAILED: {e.strerror} (errno {e.errno})")
    finally:
        s.close()


def main():
    raise_fd_limit()
    show_port_range()

    sockets = exhaust_to(DST1)
    try_other_dst(DST2)

    print(f"\ncleanup: closing {len(sockets)} sockets...")
    for s in sockets:
        s.close()


if __name__ == "__main__":
    main()

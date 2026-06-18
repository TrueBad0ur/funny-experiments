#!/usr/bin/env python3
"""
ephemeral_ports.py — ephemeral port exhaustion demo

Run:  ulimit -n 65536 && python3 ephemeral_ports.py --udp|--tcp

--udp  UDP connect() binds port by (src_ip, src_port) only — no dst.
       Once a port is taken it is unavailable for ANY destination.
       Phase 2 shows that a new UDP connect() to a DIFFERENT dst also fails.

--tcp  Non-blocking TCP connect() — kernel allocates src_port and sends SYN.
       Port selection checks the full 4-tuple (src_ip, src_port, dst_ip, dst_port).
       Phase 2 shows that TCP to a DIFFERENT dst still succeeds.

Tip: shrink the range first for a faster demo:
  sudo sysctl -w net.ipv4.ip_local_port_range="50000 50099"
  (restore: sudo sysctl -w net.ipv4.ip_local_port_range="32768 60999")

Compatible: Linux, OpenBSD.
"""

import errno
import resource
import socket
import struct
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
            print(f" -> raised to {soft}", end="")
        except ValueError:
            pass
    print()
    if soft < 32768:
        print("WARN: fd limit too low, run: ulimit -n 65536", file=sys.stderr)


def show_port_range():
    try:
        with open("/proc/sys/net/ipv4/ip_local_port_range") as f:
            lo, hi = f.read().split()
        print(f"ephemeral range: {lo}-{hi}  (~{int(hi) - int(lo)} ports)")
        return
    except FileNotFoundError:
        pass
    try:
        lo = subprocess.check_output(
            ["sysctl", "-n", "net.inet.ip.portrange.first"], text=True
        ).strip()
        hi = subprocess.check_output(
            ["sysctl", "-n", "net.inet.ip.portrange.last"], text=True
        ).strip()
        print(f"ephemeral range: {lo}-{hi}  (~{int(hi) - int(lo)} ports)")
    except Exception:
        print("ephemeral range: could not determine (check sysctl)")


def _decode_addr(hex_addr):
    """'CB710300:0050' -> '203.0.113.1:80'  (little-endian x86)"""
    ip_hex, port_hex = hex_addr.split(":")
    ip = socket.inet_ntoa(struct.pack("<I", int(ip_hex, 16)))
    port = int(port_hex, 16)
    return f"{ip}:{port}"


def show_kernel_table(label, proto="udp"):
    print(f"\n[kernel {proto.upper()} table {label}]")
    try:
        with open(f"/proc/net/{proto}") as f:
            lines = f.readlines()[1:]  # skip header
        print(f"  entries: {len(lines)}")
        for line in lines[:5]:
            parts = line.split()
            src = _decode_addr(parts[1])
            dst = _decode_addr(parts[2])
            print(f"  {src} -> {dst}")
        if len(lines) > 5:
            print(f"  ... ({len(lines) - 5} more)")
    except FileNotFoundError:
        # OpenBSD
        flag = "-u" if proto == "udp" else "-t"
        out = subprocess.run(["netstat", "-an", flag],
                             capture_output=True, text=True).stdout
        rows = [l for l in out.splitlines() if proto in l.lower()]
        print(f"  entries: {len(rows)}")
        for row in rows[:5]:
            print(f"  {row}")
        if len(rows) > 5:
            print(f"  ... ({len(rows) - 5} more)")
    print()


def make_nb_tcp():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setblocking(False)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    return s


# ------------------------------------------------------------------- UDP ---

def run_udp():
    print(f"=== Phase 0: baseline — dig google.com @8.8.8.8 (ports available) ===")
    sys.stdout.flush()
    subprocess.run(["dig", "+time=2", "+tries=1", "google.com", "@8.8.8.8"])

    show_kernel_table("before", "udp")
    print(f"\n=== Phase 1: exhaust UDP ports to {DST1[0]}:{DST1[1]} ===")
    print("(UDP binds port by src_ip:src_port only — dst doesn't matter)\n")

    sockets = []
    while True:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(DST1)
            sockets.append(s)
            n = len(sockets)
            if n % 2000 == 0:
                print(f"  {n:5d} sockets  last src_port={s.getsockname()[1]}")
                sys.stdout.flush()
        except OSError as e:
            s.close()
            print(f"\nconnect() #{len(sockets)}: {e.strerror} (errno {e.errno})")
            if e.errno in (errno.EAGAIN, errno.EADDRNOTAVAIL):
                print("-> all ephemeral ports exhausted")
            elif e.errno == errno.EMFILE:
                print("-> EMFILE: hit fd limit, run: ulimit -n 65536")
            break

    print(f"\nAllocated {len(sockets)} UDP ports")
    show_kernel_table("after", "udp")

    print(f"\n=== Phase 2: try DIFFERENT dst {DST2[0]}:{DST2[1]} ===")
    s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s2.connect(DST2)
        print(f"SUCCESS: src_port={s2.getsockname()[1]}")
        s2.close()
    except OSError as e:
        print(f"FAILED: {e.strerror} (errno {e.errno})")
        print("-> UDP port exhaustion is GLOBAL: no src_port left for any dst")
        s2.close()

    print(f"\n=== Phase 3: real-world impact — dig google.com @8.8.8.8 ===")
    sys.stdout.flush()
    subprocess.run(["dig", "+time=2", "+tries=1", "google.com", "@8.8.8.8"])

    print(f"\ncleanup: closing {len(sockets)} sockets...")
    for s in sockets:
        s.close()


# ------------------------------------------------------------------- TCP ---

def run_tcp():
    print(f"\n=== Phase 1: exhaust TCP ports to {DST1[0]}:{DST1[1]} ===")
    print("(TCP checks full 4-tuple — src_port reusable for different dst)\n")

    sockets = []
    while True:
        s = make_nb_tcp()
        try:
            s.connect(DST1)
        except BlockingIOError:
            sockets.append(s)
            n = len(sockets)
            if n % 2000 == 0:
                print(f"  {n:5d} sockets  last src_port={s.getsockname()[1]}")
                sys.stdout.flush()
            continue
        except OSError as e:
            s.close()
            print(f"\nconnect() #{len(sockets)}: {e.strerror} (errno {e.errno})")
            if e.errno == errno.EADDRNOTAVAIL:
                print(f"-> EADDRNOTAVAIL: all ports to {DST1[0]}:{DST1[1]} exhausted")
            elif e.errno == errno.EMFILE:
                print("-> EMFILE: hit fd limit, run: ulimit -n 65536")
            break
        sockets.append(s)

    print(f"\nAllocated {len(sockets)} TCP ports to {DST1[0]}:{DST1[1]}")

    print(f"\n=== Phase 2: try DIFFERENT dst {DST2[0]}:{DST2[1]} ===")
    s2 = make_nb_tcp()
    try:
        s2.connect(DST2)
    except BlockingIOError:
        pass
    except OSError as e:
        print(f"FAILED: {e.strerror} (errno {e.errno})")
        s2.close()
        return
    print(f"SUCCESS: src_port={s2.getsockname()[1]}")
    print("-> TCP port exhaustion is per (dst_ip, dst_port), not global")
    s2.close()

    print(f"\ncleanup: closing {len(sockets)} sockets...")
    for s in sockets:
        s.close()


# ------------------------------------------------------------------ main ---

def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("--udp", "--tcp"):
        print(f"Usage: {sys.argv[0]} --udp | --tcp", file=sys.stderr)
        sys.exit(1)

    raise_fd_limit()
    show_port_range()

    if sys.argv[1] == "--udp":
        run_udp()
    else:
        run_tcp()


if __name__ == "__main__":
    main()

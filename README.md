# Ephemeral Port Exhaustion Lab

Demonstrates how the Linux/OpenBSD kernel selects source ports for outgoing
connections and what happens when they run out — for both UDP and TCP.

## Background

### How the kernel picks a source port

When you call `connect()` without calling `bind()` first, the kernel
automatically assigns a source port from the **ephemeral port range**
(`net.ipv4.ip_local_port_range`, default 32768–60999 on Linux, ~28231 ports).

The selection algorithm in `inet_hash_connect()` (Linux: `net/ipv4/inet_hashtables.c`):

1. Pick a random offset inside the range (intentionally non-monotonic — prevents port prediction).
2. Walk through all candidates from that offset, wrapping around.
3. For each candidate `src_port`, check uniqueness — the check differs by protocol (see below).
4. If free — reserve it, return success.
5. If the entire range is exhausted — return `EADDRNOTAVAIL` (TCP) or `EAGAIN` (UDP on Linux).

There is no "pool" data structure. It is a linear scan with a hash table lookup
on each step. At full exhaustion the kernel walks all ~28k candidates before
returning the error, which is why the last few `connect()` calls before hitting
the limit are visibly slower.

### UDP vs TCP: fundamentally different exhaustion semantics

This is the core of the lab. The two protocols use different uniqueness checks
when allocating a source port:

**UDP** — port is bound by `(src_ip, src_port)` only. The destination is not
part of the check. Once port 35000 is taken by any UDP socket, it is
unavailable for a new UDP socket regardless of where that socket is going.
Exhaustion is **global per source address**.

**TCP** — port is checked against the full 4-tuple `(src_ip, src_port, dst_ip, dst_port)`.
Port 35000 occupied by a connection to `203.0.113.1:80` does not prevent
a new connection to `198.51.100.1:80` from using the same port 35000 — it is
a different 4-tuple. Exhaustion is **per (src_ip, dst_ip, dst_port)**.

| src_ip   | src_port | dst_ip        | dst_port | TCP    | UDP    |
|----------|----------|---------------|----------|--------|--------|
| 10.0.0.1 | 32768    | 203.0.113.1   | 80       | taken  | taken  |
| 10.0.0.1 | 32768    | 198.51.100.1  | 80       | **free** | blocked |
| 10.0.0.1 | 32768    | 203.0.113.1   | 443      | **free** | blocked |
| 10.0.0.2 | 32768    | 203.0.113.1   | 80       | **free** | **free** |

In practice: after UDP port exhaustion, `dig google.com @8.8.8.8` fails with
`"UDP setup failed: address in use"` — even though it is connecting to a
completely different address. After TCP port exhaustion to one endpoint,
`connect()` to any other endpoint still succeeds.

### TIME_WAIT: the hidden TCP throughput cap

After a TCP connection closes, the 4-tuple is held in `TIME_WAIT` for 60 seconds
(2×MSL) to absorb late-arriving packets. The port is unavailable for a new
connection to the same destination during that window.

```
28231 ports / 60 seconds ≈ 470 new connections/second max to one dst_ip:dst_port
```

At 1000 RPS to a single database from a single IP you will hit this limit before
you ever see `EADDRNOTAVAIL` — new connections will queue waiting for `TIME_WAIT`
slots to expire.

### The kernel table

The kernel's current UDP socket state is visible at `/proc/net/udp` (Linux).
Each row is a connected or bound socket in hex little-endian encoding:

```
local_addr:port  rem_addr:port  state  ...
CB710300:B994    030071CB:0050  01     ...   →  10.8.0.7:47508 -> 203.0.113.1:80
```

The lab reads and decodes this table before and after exhaustion so you can
see the entries filling up.

## What this lab does

### UDP mode (`--udp`)

**Phase 1** — opens UDP sockets in a loop, calling `connect()` to `203.0.113.1:80`
(RFC 5737 documentation address, not routable — no packets sent). The kernel
allocates `src_port` and binds it globally to `(src_ip, src_port)`.
Continues until `EAGAIN` (all ports taken).

Prints the kernel UDP table (`/proc/net/udp`) before and after with decoded
`src_ip:src_port -> dst_ip:dst_port` entries.

**Phase 2** — attempts UDP `connect()` to a different destination (`198.51.100.1:80`).
Fails with the same error — exhaustion is global, dst doesn't matter.

**Phase 3** — runs `dig google.com @8.8.8.8` while ports are still exhausted.
Real output from the real tool:
```
;; UDP setup with 8.8.8.8#53(8.8.8.8) for google.com failed: address in use.
;; no servers could be reached
```

### TCP mode (`--tcp`)

**Phase 1** — opens non-blocking TCP sockets in a loop, calling `connect()` to
`203.0.113.1:80`. The kernel allocates `src_port`, transitions socket to
`SYN_SENT`, SYN goes nowhere (address not routable). Continues until
`EADDRNOTAVAIL`.

**Phase 2** — attempts TCP `connect()` to a different destination (`198.51.100.1:80`).
Succeeds, reusing a port number already in use for Phase 1 — different 4-tuple,
different connection. Exhaustion is scoped to the `(src_ip, dst_ip, dst_port)` triple.

## Limits involved

| Limit | Scope | Default | Tunable |
|-------|-------|---------|---------|
| `RLIMIT_NOFILE` | per process | 1024 | `ulimit -n` |
| ephemeral port range (UDP) | per src_ip, system-wide | 32768–60999 | `sysctl net.ipv4.ip_local_port_range` |
| ephemeral port range (TCP) | per (src_ip, dst_ip, dst_port), system-wide | 32768–60999 | `sysctl net.ipv4.ip_local_port_range` |

The fd limit is raised automatically at startup (to the process hard limit).
If that is still too low, run `ulimit -n 65536` before the script.

## Mitigations in production

- **Widen the range**: `sysctl -w net.ipv4.ip_local_port_range="1024 65535"` (~64k ports).
- **`tcp_tw_reuse`**: `sysctl -w net.ipv4.tcp_tw_reuse=1` — kernel reuses `TIME_WAIT`
  sockets for new outgoing connections when safe (sequence numbers don't conflict).
- **Multiple source IPs**: each IP gives an independent port budget. A NAT gateway
  with N public IPs sustains N×28k concurrent TCP connections to the same endpoint.
- **Connection pooling**: keep connections alive and reuse them — avoids `TIME_WAIT`
  accumulation entirely.

## Usage

```bash
# via run.sh (handles ulimit and compilation)
./run.sh -py-udp   # Python, UDP mode
./run.sh -py-tcp   # Python, TCP mode
./run.sh -c-udp    # build + run C, UDP mode
./run.sh -c-tcp    # build + run C, TCP mode

# directly
ulimit -n 65536
python3 ephemeral_ports.py --udp
python3 ephemeral_ports.py --tcp
./ephemeral_ports --udp
./ephemeral_ports --tcp
```

For a faster demo with a smaller port range:

```bash
sudo sysctl -w net.ipv4.ip_local_port_range="50000 50099"
./run.sh -py-udp
# restore
sudo sysctl -w net.ipv4.ip_local_port_range="32768 60999"
```

## Files

| File | Description |
|------|-------------|
| `ephemeral_ports.c` | C implementation (`--udp` / `--tcp`), Linux and OpenBSD |
| `ephemeral_ports.py` | Python implementation (`--udp` / `--tcp`), Linux and OpenBSD |
| `Makefile` | builds the C binary with `cc` |
| `run.sh` | compiles (C), sets `ulimit`, dispatches by flag |

/*
 * ephemeral_ports.c — ephemeral port exhaustion demo
 *
 * Build:  cc -Wall -O2 -o ephemeral_ports ephemeral_ports.c
 * Run:    ulimit -n 65536 && ./ephemeral_ports --udp|--tcp
 *
 * --udp  Phase 0: dig google.com @8.8.8.8 — baseline, works fine.
 *        Phase 1: exhaust all ephemeral ports via UDP connect() to one dst.
 *                 UDP binds port by (src_ip, src_port) only — dst irrelevant.
 *                 Prints /proc/net/udp entry count before and after via ss.
 *        Phase 2: UDP connect() to a DIFFERENT dst — also fails (global exhaustion).
 *        Phase 3: dig google.com @8.8.8.8 — fails: "address in use".
 *
 * --tcp  Phase 1: exhaust ports via non-blocking TCP connect() to one dst.
 *                 Kernel checks full 4-tuple (src_ip, src_port, dst_ip, dst_port).
 *        Phase 2: TCP connect() to a DIFFERENT dst — succeeds (per-dst exhaustion).
 *
 * Tip: shrink the range first for a faster demo:
 *   sudo sysctl -w net.ipv4.ip_local_port_range="50000 50099"
 *   (restore: sudo sysctl -w net.ipv4.ip_local_port_range="32768 60999")
 *
 * Compatible: Linux, OpenBSD.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* RFC 5737 documentation addresses — not routable */
#define DST1_IP  "203.0.113.1"
#define DST2_IP  "198.51.100.1"
#define DST_PORT 80

#define MAX_FDS 70000

static int fds[MAX_FDS];

static void raise_fd_limit(void)
{
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    printf("fd limit: cur=%lu max=%lu", (unsigned long)rl.rlim_cur,
           (unsigned long)rl.rlim_max);
    if (rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rl) == 0) {
            getrlimit(RLIMIT_NOFILE, &rl);
            printf(" -> raised to %lu", (unsigned long)rl.rlim_cur);
        }
    }
    printf("\n");
    if (rl.rlim_cur < 32768)
        fprintf(stderr, "WARN: fd limit too low, run: ulimit -n 65536\n");
}

static void show_port_range(void)
{
#ifdef __linux__
    FILE *f = fopen("/proc/sys/net/ipv4/ip_local_port_range", "r");
    if (f) {
        int lo, hi;
        fscanf(f, "%d %d", &lo, &hi);
        fclose(f);
        printf("ephemeral range: %d-%d  (~%d ports)\n", lo, hi, hi - lo);
        return;
    }
#endif
    printf("ephemeral range: check sysctl net.inet.ip.portrange.first/last (OpenBSD)\n"
           "                 or /proc/sys/net/ipv4/ip_local_port_range (Linux)\n");
}

static struct sockaddr_in make_addr(const char *ip, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((unsigned short)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

static void show_kernel_table(const char *label, const char *proto)
{
    char cmd[128];
#ifdef __linux__
    /* ss -uanH: UDP all numeric no-header; -tanH for TCP */
    snprintf(cmd, sizeof(cmd),
             "echo \"[kernel %s table %s: $(ss -%sanH | wc -l) entries]\""
             " && ss -%sanH | head -5",
             proto, label,
             strcmp(proto, "UDP") == 0 ? "u" : "t",
             strcmp(proto, "UDP") == 0 ? "u" : "t");
#else
    snprintf(cmd, sizeof(cmd),
             "echo \"[kernel %s table %s]\" && netstat -an%s | head -8",
             proto, label,
             strcmp(proto, "UDP") == 0 ? "u" : "t");
#endif
    system(cmd);
    printf("\n");
}

static int make_nb_tcp(void)
{
    struct linger lg = {1, 0};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    return fd;
}

/* ------------------------------------------------------------------ UDP --- */

static void run_udp(void)
{
    struct sockaddr_in dst1 = make_addr(DST1_IP, DST_PORT);
    struct sockaddr_in dst2 = make_addr(DST2_IP, DST_PORT);
    struct sockaddr_in local;
    socklen_t len;
    int n, err;

    printf("=== Phase 0: baseline — dig google.com @8.8.8.8 (ports available) ===\n");
    fflush(stdout);
    system("dig +time=2 +tries=1 google.com @8.8.8.8");

    show_kernel_table("before", "UDP");
    printf("\n=== Phase 1: exhaust UDP ports to %s:%d ===\n", DST1_IP, DST_PORT);
    printf("(UDP binds port by src_ip:src_port only — dst doesn't matter)\n\n");

    for (n = 0; n < MAX_FDS; n++) {
        fds[n] = socket(AF_INET, SOCK_DGRAM, 0);
        if (fds[n] < 0) {
            err = errno;
            fprintf(stderr, "socket() at n=%d: %s (errno %d)\n", n, strerror(err), err);
            if (err == EMFILE)
                fprintf(stderr, "  -> raise fd limit: ulimit -n 65536\n");
            break;
        }
        if (connect(fds[n], (struct sockaddr *)&dst1, sizeof(dst1)) < 0) {
            err = errno;
            close(fds[n]);
            fds[n] = -1;
            printf("\nconnect() #%d: %s (errno %d)\n", n, strerror(err), err);
            if (err == EAGAIN || err == EADDRNOTAVAIL)
                printf("-> all ephemeral ports exhausted\n");
            break;
        }
        if (n > 0 && n % 2000 == 0) {
            len = sizeof(local);
            getsockname(fds[n], (struct sockaddr *)&local, &len);
            printf("  %5d sockets  last src_port=%d\n", n, ntohs(local.sin_port));
            fflush(stdout);
        }
    }

    printf("\nAllocated %d UDP ports\n", n);
    show_kernel_table("after", "UDP");

    printf("\n=== Phase 2: try DIFFERENT dst %s:%d ===\n", DST2_IP, DST_PORT);
    {
        int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd2 < 0) {
            printf("socket(): %s\n", strerror(errno));
        } else if (connect(fd2, (struct sockaddr *)&dst2, sizeof(dst2)) < 0) {
            err = errno;
            printf("FAILED: %s (errno %d)\n", strerror(err), err);
            printf("-> UDP port exhaustion is GLOBAL: no src_port left for any dst\n");
            close(fd2);
        } else {
            len = sizeof(local);
            getsockname(fd2, (struct sockaddr *)&local, &len);
            printf("SUCCESS: src_port=%d\n", ntohs(local.sin_port));
            close(fd2);
        }
    }

    printf("\n=== Phase 3: real-world impact — dig google.com @8.8.8.8 ===\n");
    system("dig +time=2 +tries=1 google.com @8.8.8.8");

    printf("\ncleanup: closing %d sockets...\n", n);
    for (int i = 0; i < n; i++)
        if (fds[i] >= 0)
            close(fds[i]);
}

/* ------------------------------------------------------------------ TCP --- */

static void run_tcp(void)
{
    struct sockaddr_in dst1 = make_addr(DST1_IP, DST_PORT);
    struct sockaddr_in dst2 = make_addr(DST2_IP, DST_PORT);
    struct sockaddr_in local;
    socklen_t len;
    int n, err;

    printf("\n=== Phase 1: exhaust TCP ports to %s:%d ===\n", DST1_IP, DST_PORT);
    printf("(TCP checks full 4-tuple — src_port reusable for different dst)\n\n");

    for (n = 0; n < MAX_FDS; n++) {
        fds[n] = make_nb_tcp();
        if (fds[n] < 0) {
            err = errno;
            fprintf(stderr, "socket() at n=%d: %s (errno %d)\n", n, strerror(err), err);
            if (err == EMFILE)
                fprintf(stderr, "  -> raise fd limit: ulimit -n 65536\n");
            break;
        }
        if (connect(fds[n], (struct sockaddr *)&dst1, sizeof(dst1)) < 0) {
            err = errno;
            if (err == EINPROGRESS) {
                if (n > 0 && n % 2000 == 0) {
                    len = sizeof(local);
                    getsockname(fds[n], (struct sockaddr *)&local, &len);
                    printf("  %5d sockets  last src_port=%d\n", n, ntohs(local.sin_port));
                    fflush(stdout);
                }
                continue;
            }
            close(fds[n]);
            fds[n] = -1;
            printf("\nconnect() #%d: %s (errno %d)\n", n, strerror(err), err);
            if (err == EADDRNOTAVAIL)
                printf("-> EADDRNOTAVAIL: all ports to %s:%d exhausted\n",
                       DST1_IP, DST_PORT);
            break;
        }
    }

    printf("\nAllocated %d TCP ports to %s:%d\n", n, DST1_IP, DST_PORT);

    printf("\n=== Phase 2: try DIFFERENT dst %s:%d ===\n", DST2_IP, DST_PORT);
    {
        int fd2 = make_nb_tcp();
        if (fd2 < 0) {
            printf("socket(): %s\n", strerror(errno));
        } else {
            int rc = connect(fd2, (struct sockaddr *)&dst2, sizeof(dst2));
            err = errno;
            if (rc == 0 || err == EINPROGRESS) {
                len = sizeof(local);
                getsockname(fd2, (struct sockaddr *)&local, &len);
                printf("SUCCESS: src_port=%d\n", ntohs(local.sin_port));
                printf("-> TCP port exhaustion is per (dst_ip, dst_port), not global\n");
            } else {
                printf("FAILED: %s (errno %d)\n", strerror(err), err);
            }
            close(fd2);
        }
    }

    printf("\ncleanup: closing %d sockets...\n", n);
    for (int i = 0; i < n; i++)
        if (fds[i] >= 0)
            close(fds[i]);
}

/* --------------------------------------------------------------- main --- */

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--udp") != 0 && strcmp(argv[1], "--tcp") != 0)) {
        fprintf(stderr, "Usage: %s --udp | --tcp\n", argv[0]);
        return 1;
    }

    raise_fd_limit();
    show_port_range();

    if (strcmp(argv[1], "--udp") == 0)
        run_udp();
    else
        run_tcp();

    return 0;
}

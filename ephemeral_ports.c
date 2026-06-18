/*
 * ephemeral_ports.c — ephemeral port exhaustion demo
 *
 * Build:  cc -Wall -O2 -o ephemeral_ports ephemeral_ports.c
 * Run:    ulimit -n 65536 && ./ephemeral_ports
 *
 * Uses UDP connect() — kernel assigns src_port without sending any packets.
 * When all ~28000 ports to DST1 are exhausted → EADDRNOTAVAIL.
 * Then shows that connecting to DST2 (different dst) still works.
 *
 * Compatible: Linux, OpenBSD.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
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
            printf(" → raised to %lu", (unsigned long)rl.rlim_cur);
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
        printf("ephemeral range: %d–%d  (~%d ports per dst)\n", lo, hi, hi - lo);
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

int main(void)
{
    int n, err;
    struct sockaddr_in dst1, dst2, local;
    socklen_t len;

    raise_fd_limit();
    show_port_range();

    dst1 = make_addr(DST1_IP, DST_PORT);
    dst2 = make_addr(DST2_IP, DST_PORT);

    for (n = 0; n < MAX_FDS; n++)
        fds[n] = -1;

    printf("\n=== Phase 1: exhaust ports to %s:%d ===\n", DST1_IP, DST_PORT);

    for (n = 0; n < MAX_FDS; n++) {
        fds[n] = socket(AF_INET, SOCK_DGRAM, 0);
        if (fds[n] < 0) {
            err = errno;
            fprintf(stderr, "socket() at n=%d: %s (errno %d)\n", n, strerror(err), err);
            if (err == EMFILE)
                fprintf(stderr, "  → raise fd limit: ulimit -n 65536\n");
            break;
        }
        if (connect(fds[n], (struct sockaddr *)&dst1, sizeof(dst1)) < 0) {
            err = errno;
            close(fds[n]);
            fds[n] = -1;
            printf("\nconnect() #%d: %s (errno %d)\n", n, strerror(err), err);
            if (err == EADDRNOTAVAIL)
                printf("→ EADDRNOTAVAIL: all ephemeral ports to %s:%d exhausted\n",
                       DST1_IP, DST_PORT);
            break;
        }
        if (n > 0 && n % 2000 == 0) {
            len = sizeof(local);
            getsockname(fds[n], (struct sockaddr *)&local, &len);
            printf("  %5d sockets  last src_port=%d\n", n, ntohs(local.sin_port));
            fflush(stdout);
        }
    }

    printf("\nAllocated %d ports to %s:%d\n", n, DST1_IP, DST_PORT);

    printf("\n=== Phase 2: try DIFFERENT dst %s:%d ===\n", DST2_IP, DST_PORT);
    {
        int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd2 < 0) {
            printf("socket(): %s\n", strerror(errno));
        } else if (connect(fd2, (struct sockaddr *)&dst2, sizeof(dst2)) < 0) {
            printf("connect(): %s\n", strerror(errno));
            close(fd2);
        } else {
            len = sizeof(local);
            getsockname(fd2, (struct sockaddr *)&local, &len);
            printf("SUCCESS: src_port=%d\n", ntohs(local.sin_port));
            printf("→ port exhaustion is per (dst_ip, dst_port), not global\n");
            close(fd2);
        }
    }

    printf("\ncleanup: closing %d sockets...\n", n);
    for (int i = 0; i < n; i++)
        if (fds[i] >= 0)
            close(fds[i]);

    return 0;
}

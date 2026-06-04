#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ice_utils.h"

#ifndef HUGETLBFS_MAGIC
#define HUGETLBFS_MAGIC 0x958458f6
#endif

void die_errno(const char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

void die_msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

int parse_int_range(const char *s, int min_v, int max_v, int *out)
{
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (v < min_v || v > max_v)
        return -1;

    *out = (int)v;
    return 0;
}

int parse_mac_addr(const char *s, uint8_t *out)
{
    unsigned int b0, b1, b2, b3, b4, b5;
    char tail;
    int n;

    n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c",
               &b0, &b1, &b2, &b3, &b4, &b5, &tail);
    if (n != 6)
        return -1;

    out[0] = (uint8_t)b0;
    out[1] = (uint8_t)b1;
    out[2] = (uint8_t)b2;
    out[3] = (uint8_t)b3;
    out[4] = (uint8_t)b4;
    out[5] = (uint8_t)b5;
    return 0;
}

int parse_u32_hex(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (v > 0xFFFFFFFFUL)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        die_errno("clock_gettime");

    return (uint64_t)ts.tv_sec * NS_PER_S + (uint64_t)ts.tv_nsec;
}

double bytes_ns_to_gbps(uint64_t bytes, uint64_t duration_ns)
{
    if (duration_ns == 0)
        return 0.0;

    return ((double)bytes * 8.0 * (double)NS_PER_S) / ((double)duration_ns * 1e9);
}

double pkts_ns_to_mpps(uint64_t pkts, uint64_t duration_ns)
{
    if (duration_ns == 0)
        return 0.0;

    return ((double)pkts * (double)NS_PER_S) / ((double)duration_ns * 1e6);
}

int copy_path_option(char dst[PATH_MAX], const char *src, const char *opt_name)
{
    int n;

    if (!src || src[0] == '\0') {
        fprintf(stderr, "%s requires a non-empty path\n", opt_name);
        return -1;
    }

    n = snprintf(dst, PATH_MAX, "%s", src);
    if (n < 0 || n >= PATH_MAX) {
        fprintf(stderr, "%s path too long\n", opt_name);
        return -1;
    }

    return 0;
}

int write_rx_reflect_metrics_log(const struct ice_vfio_dev *d,
                                        const struct rx_reflect_metrics *metrics)
{
    char tmp_path[PATH_MAX];
    FILE *fp = NULL;
    int n;
    int saved_errno = 0;

    if (!d || !metrics || d->metrics_log_path[0] == '\0')
        return 0;

    /* Write to a temp file first so readers never observe a partially written reflect summary. */
    n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", d->metrics_log_path);
    if (n < 0 || n >= (int)sizeof(tmp_path)) {
        fprintf(stderr, "[my_ice] metrics log temp path too long for %s\n",
                d->metrics_log_path);
        return -1;
    }

    fp = fopen(tmp_path, "w");
    if (!fp) {
        fprintf(stderr, "[my_ice] failed to open metrics log %s: %s\n",
                tmp_path, strerror(errno));
        return -1;
    }

#define WRITE_METRIC(fmt, ...)                                                      \
    do {                                                                            \
        if (fprintf(fp, fmt "\n", ##__VA_ARGS__) < 0) {                             \
            saved_errno = errno;                                                    \
            goto fail;                                                              \
        }                                                                           \
    } while (0)

    WRITE_METRIC("schema=my_ice_rx_reflect_v1");
    WRITE_METRIC("mode=rx_reflect");
    WRITE_METRIC("seconds_total=%.6f", metrics->seconds_total);
    WRITE_METRIC("tx_mpps=%.6f", metrics->tx_mpps);
    WRITE_METRIC("rx_mpps=%.6f", metrics->rx_mpps);
    WRITE_METRIC("tx_l2_gbps=%.6f", metrics->tx_l2_gbps);
    WRITE_METRIC("rx_l2_gbps=%.6f", metrics->rx_l2_gbps);

#undef WRITE_METRIC

    if (fclose(fp) != 0) {
        saved_errno = errno;
        fp = NULL;
        goto fail;
    }
    fp = NULL;

    if (rename(tmp_path, d->metrics_log_path) != 0) {
        fprintf(stderr, "[my_ice] failed to rename metrics log %s -> %s: %s\n",
                tmp_path, d->metrics_log_path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    fprintf(stderr, "[my_ice] wrote metrics log %s\n", d->metrics_log_path);
    return 0;

fail:
    if (fp)
        fclose(fp);
    unlink(tmp_path);
    if (saved_errno == 0)
        saved_errno = errno;
    fprintf(stderr, "[my_ice] failed to write metrics log %s: %s\n",
            d->metrics_log_path, strerror(saved_errno));
    return -1;
}

int build_cpu_list(int *out, int max_out)
{
    cpu_set_t set;
    int count = 0;
    int cpu;

    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return -1;

    for (cpu = 0; cpu < CPU_SETSIZE && count < max_out; cpu++) {
        if (CPU_ISSET(cpu, &set))
            out[count++] = cpu;
    }

    return count;
}

void pin_thread_to_cpu(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}


void dump_hex(const uint8_t *buf, size_t len, size_t max_len)
{
    size_t n = len < max_len ? len : max_len;
    size_t i;

    for (i = 0; i < n; i += 16) {
        size_t j;
        fprintf(stderr, "  %04zx:", i);
        for (j = 0; j < 16 && i + j < n; j++)
            fprintf(stderr, " %02x", buf[i + j]);
        fprintf(stderr, "\n");
    }
}


size_t get_hugepage_size(void)
{
    FILE *fp;
    char line[256];
    long kb = 0;

    fp = fopen("/proc/meminfo", "r");
    if (!fp)
        return 2U * 1024U * 1024U;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "Hugepagesize: %ld kB", &kb) == 1)
            break;
    }
    fclose(fp);

    if (kb <= 0)
        return 2U * 1024U * 1024U;
    return (size_t)kb * 1024U;
}

size_t prepare_hugepage_file(struct ice_vfio_dev *d, size_t size, const char *dir)
{
    int fd;
    int n;
    size_t hp_size;
    size_t aligned;
    struct statfs sfs;

    n = snprintf(d->huge_path, sizeof(d->huge_path),
                 "%s/my_ice_hp_%d", dir, getpid());
    if (n < 0 || (size_t)n >= sizeof(d->huge_path))
        die_msg("hugepage dir path too long");

    if (statfs(dir, &sfs) != 0)
        die_errno("statfs hugepage dir");
    if ((unsigned long)sfs.f_type != HUGETLBFS_MAGIC)
        die_msg("hugepage dir is not hugetlbfs (mount -t hugetlbfs nodev /mnt/huge)");

    fd = open(d->huge_path, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        die_errno("open hugepage file");

    hp_size = get_hugepage_size();
    aligned = ALIGN_UP(size, hp_size);
    if (ftruncate(fd, (off_t)aligned) < 0) {
        if (errno == EINVAL) {
            fprintf(stderr,
                    "ftruncate hugepage file: Invalid argument (size=%zu, hugepage=%zu)\n",
                    aligned, hp_size);
        }
        die_errno("ftruncate hugepage file");
    }

    d->huge_fd = fd;
    d->huge_alloc_size = aligned;
    return aligned;
}

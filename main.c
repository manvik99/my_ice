#define _GNU_SOURCE
#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/vfio.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ice_min.h"

#ifndef VFIO_PCI_BAR0_REGION_INDEX
#define VFIO_PCI_BAR0_REGION_INDEX 0
#endif

#ifndef VFIO_PCI_CONFIG_REGION_INDEX
#define VFIO_PCI_CONFIG_REGION_INDEX 7
#endif

#ifndef HUGETLBFS_MAGIC
#define HUGETLBFS_MAGIC 0x958458f6
#endif

#define PCI_COMMAND_OFF 0x04
#define PCI_COMMAND_MEM 0x2
#define PCI_COMMAND_MASTER 0x4

#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((size_t)((a) - 1)))
#define NS_PER_S 1000000000ULL
#define TX_BURST_SIZE 64
#define RX_REFLECT_GLOBAL_BUDGET 512
#define RX_REFLECT_STATS_PAGE_SIZE 4096U
#define TX_RS_THRESH 32
#define ETH_WIRE_OVERHEAD_BYTES 24U
#define ICE_PKT_BUF_DATA_SIZE \
    ((ICE_RX_BUF_SIZE > ICE_TX_PKT_BUF_SIZE) ? ICE_RX_BUF_SIZE : ICE_TX_PKT_BUF_SIZE)
#define ICE_REFLECT_POOL_EXTRA TX_BURST_SIZE

static bool g_dump_topo = false;
static bool g_qparent_override_set = false;
static uint32_t g_qparent_override = 0;
struct pkt_mempool;

struct pkt_buf {
    uint64_t buf_addr_iova;
    struct pkt_mempool *mempool;
    uint32_t mempool_idx;
    uint32_t size;
    uint8_t head_room[40];
    uint8_t data[] __attribute__((aligned(64)));
};

struct pkt_mempool {
    uint8_t *base;
    uint64_t base_iova;
    uint32_t entry_size;
    uint32_t num_entries;
    uint32_t free_head;
    uint32_t free_tail;
    uint32_t free_count;
    uint32_t *free_ring;
};

#define ICE_PKT_BUF_ENTRY_SIZE \
    ALIGN_UP(offsetof(struct pkt_buf, data) + ICE_PKT_BUF_DATA_SIZE, 64)

struct dma_block {
    void *vaddr;
    uint64_t iova;
    size_t size;
};

struct aq_ring_ctx {
    struct ice_aq_desc *desc;
    uint64_t desc_iova;
    uint8_t *buf;
    uint64_t buf_iova;
    uint16_t count;
    uint16_t next_to_use;
};

struct rxq_ctx {
    uint16_t rxq_id;
    union ice_32b_rx_flex_desc *rx_desc;
    uint64_t rx_desc_iova;
    uint8_t *rx_bufs;
    uint64_t rx_bufs_iova;
    struct pkt_buf **rx_pkt_bufs;
    uint16_t rx_ntc;
    bool rss_sampled;
};

struct io_ring_ctx {
    uint8_t mac[ETHER_ADDR_LEN];
    uint8_t lport;
    uint16_t vsi_num;
    uint16_t rxq_count;
    uint16_t rxq_poll_next;
    uint16_t rss_target_vsig;
    uint16_t rss_current_vsig;
    uint32_t qparent_teid;
    struct rxq_ctx rxqs[ICE_MAX_RX_QUEUES];
};

struct txq_ctx {
    uint16_t txq_id;
    uint16_t desc_count;
    struct ice_tx_desc *tx_desc;
    uint64_t tx_desc_iova;
    uint8_t *tx_pkt_bufs;
    uint64_t tx_pkt_iova;
    uint16_t tx_next_to_use;
    uint16_t tx_next_to_clean;
    uint16_t tx_free;
    uint16_t tx_pkts_since_rs;
    struct pkt_buf **tx_pkt_buf_refs;
};

struct dev_ctx {
    int container_fd;
    int group_fd;
    int device_fd;
    int group_id;
    int huge_fd;
    char pci_bdf[32];
    char huge_path[PATH_MAX];
    uint8_t *bar0;
    size_t bar0_size;
    uint16_t txq_count;
    uint16_t txq_alloc_count;
    uint16_t tx_desc_count;
    struct txq_ctx *txqs;

    struct dma_block dma;
    struct aq_ring_ctx atq;
    struct aq_ring_ctx arq;
    struct io_ring_ctx io;
    struct pkt_mempool reflect_pool;
    struct pkt_mempool reflect_pools[ICE_MAX_RX_QUEUES];
};

static void rearm_rx_desc(struct dev_ctx *d, uint16_t idx);
static uint16_t tx_try_enqueue_pkt_buf_batch(struct dev_ctx *d, struct txq_ctx *q,
                                             struct pkt_buf *bufs[], uint16_t num_bufs);

static const struct ice_aqc_get_set_rss_keys g_default_rss_key = {
    .standard_rss_key = {
        0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
        0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
        0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
        0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
        0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
    },
    .extended_hash_key = {
        0xfe, 0xc4, 0x2b, 0x73, 0x01, 0x9d,
        0x1f, 0x7b, 0xa6, 0x4f, 0xd4, 0x91,
    },
};

static const struct ice_ctx_ele rlan_ctx_info[] = {
    ICE_CTX_STORE(ice_rlan_ctx, head,        13, 0),
    ICE_CTX_STORE(ice_rlan_ctx, cpuid,        8, 13),
    ICE_CTX_STORE(ice_rlan_ctx, base,        57, 32),
    ICE_CTX_STORE(ice_rlan_ctx, qlen,        13, 89),
    ICE_CTX_STORE(ice_rlan_ctx, dbuf,         7, 102),
    ICE_CTX_STORE(ice_rlan_ctx, hbuf,         5, 109),
    ICE_CTX_STORE(ice_rlan_ctx, dtype,        2, 114),
    ICE_CTX_STORE(ice_rlan_ctx, dsize,        1, 116),
    ICE_CTX_STORE(ice_rlan_ctx, crcstrip,     1, 117),
    ICE_CTX_STORE(ice_rlan_ctx, l2tsel,       1, 119),
    ICE_CTX_STORE(ice_rlan_ctx, hsplit_0,     4, 120),
    ICE_CTX_STORE(ice_rlan_ctx, hsplit_1,     2, 124),
    ICE_CTX_STORE(ice_rlan_ctx, showiv,       1, 127),
    ICE_CTX_STORE(ice_rlan_ctx, rxmax,       14, 174),
    ICE_CTX_STORE(ice_rlan_ctx, tphrdesc_ena, 1, 193),
    ICE_CTX_STORE(ice_rlan_ctx, tphwdesc_ena, 1, 194),
    ICE_CTX_STORE(ice_rlan_ctx, tphdata_ena,  1, 195),
    ICE_CTX_STORE(ice_rlan_ctx, tphhead_ena,  1, 196),
    ICE_CTX_STORE(ice_rlan_ctx, lrxqthresh,   3, 198),
    ICE_CTX_STORE(ice_rlan_ctx, prefena,      1, 201),
    { 0, 0, 0, 0 },
};

static const struct ice_ctx_ele tlan_ctx_info[] = {
    ICE_CTX_STORE(ice_tlan_ctx, base,                 57, 0),
    ICE_CTX_STORE(ice_tlan_ctx, port_num,              3, 57),
    ICE_CTX_STORE(ice_tlan_ctx, cgd_num,               5, 60),
    ICE_CTX_STORE(ice_tlan_ctx, pf_num,                3, 65),
    ICE_CTX_STORE(ice_tlan_ctx, vmvf_num,             10, 68),
    ICE_CTX_STORE(ice_tlan_ctx, vmvf_type,             2, 78),
    ICE_CTX_STORE(ice_tlan_ctx, src_vsi,              10, 80),
    ICE_CTX_STORE(ice_tlan_ctx, tsyn_ena,              1, 90),
    ICE_CTX_STORE(ice_tlan_ctx, internal_usage_flag,   1, 91),
    ICE_CTX_STORE(ice_tlan_ctx, alt_vlan,              1, 92),
    ICE_CTX_STORE(ice_tlan_ctx, cpuid,                 8, 93),
    ICE_CTX_STORE(ice_tlan_ctx, wb_mode,               1, 101),
    ICE_CTX_STORE(ice_tlan_ctx, tphrd_desc,            1, 102),
    ICE_CTX_STORE(ice_tlan_ctx, tphrd,                 1, 103),
    ICE_CTX_STORE(ice_tlan_ctx, tphwr_desc,            1, 104),
    ICE_CTX_STORE(ice_tlan_ctx, cmpq_id,               9, 105),
    ICE_CTX_STORE(ice_tlan_ctx, qnum_in_func,         14, 114),
    ICE_CTX_STORE(ice_tlan_ctx, itr_notification_mode, 1, 128),
    ICE_CTX_STORE(ice_tlan_ctx, adjust_prof_id,        6, 129),
    ICE_CTX_STORE(ice_tlan_ctx, qlen,                 13, 135),
    ICE_CTX_STORE(ice_tlan_ctx, quanta_prof_idx,       4, 148),
    ICE_CTX_STORE(ice_tlan_ctx, tso_ena,               1, 152),
    ICE_CTX_STORE(ice_tlan_ctx, tso_qnum,             11, 153),
    ICE_CTX_STORE(ice_tlan_ctx, legacy_int,            1, 164),
    ICE_CTX_STORE(ice_tlan_ctx, drop_ena,              1, 165),
    ICE_CTX_STORE(ice_tlan_ctx, cache_prof_idx,        2, 166),
    ICE_CTX_STORE(ice_tlan_ctx, pkt_shaper_prof_idx,   3, 168),
    ICE_CTX_STORE(ice_tlan_ctx, int_q_state,         122, 171),
    { 0, 0, 0, 0 },
};

static void die_errno(const char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

static void die_msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

static int parse_int_range(const char *s, int min_v, int max_v, int *out)
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

static int parse_mac_addr(const char *s, uint8_t *out)
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

static int parse_u32_hex(const char *s, uint32_t *out)
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

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        die_errno("clock_gettime");

    return (uint64_t)ts.tv_sec * NS_PER_S + (uint64_t)ts.tv_nsec;
}

static double bytes_ns_to_gbps(uint64_t bytes, uint64_t duration_ns)
{
    if (duration_ns == 0)
        return 0.0;

    return ((double)bytes * 8.0 * (double)NS_PER_S) / ((double)duration_ns * 1e9);
}

static double pkts_ns_to_mpps(uint64_t pkts, uint64_t duration_ns)
{
    if (duration_ns == 0)
        return 0.0;

    return ((double)pkts * (double)NS_PER_S) / ((double)duration_ns * 1e6);
}

static uint64_t l2_bytes_to_wire_bytes(uint64_t pkts, uint64_t l2_bytes)
{
    return l2_bytes + pkts * ETH_WIRE_OVERHEAD_BYTES;
}

static int build_cpu_list(int *out, int max_out)
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

static int pin_thread_to_cpu(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static int read_text_line(const char *path, char *buf, size_t buf_sz)
{
    FILE *fp;

    if (!path || !buf || buf_sz < 2)
        return -1;

    fp = fopen(path, "r");
    if (!fp)
        return -1;
    if (!fgets(buf, (int)buf_sz, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int read_int_file(const char *path, int *out)
{
    char buf[128];
    char *end = NULL;
    long v;

    if (!out)
        return -1;
    if (read_text_line(path, buf, sizeof(buf)) < 0)
        return -1;

    errno = 0;
    v = strtol(buf, &end, 10);
    if (errno != 0 || end == buf)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_cpu_list(const char *s, int out[], int max_out)
{
    const char *p = s;
    int count = 0;

    if (!s || !out || max_out <= 0)
        return -1;

    while (*p != '\0') {
        char *end = NULL;
        long start;
        long stop;
        long cpu;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
            p++;
        if (*p == '\0')
            break;

        errno = 0;
        start = strtol(p, &end, 10);
        if (errno != 0 || end == p)
            return -1;
        stop = start;
        p = end;
        if (*p == '-') {
            p++;
            errno = 0;
            stop = strtol(p, &end, 10);
            if (errno != 0 || end == p || stop < start)
                return -1;
            p = end;
        }

        for (cpu = start; cpu <= stop; cpu++) {
            if (cpu < 0 || cpu >= CPU_SETSIZE)
                continue;
            if (count >= max_out)
                return count;
            out[count++] = (int)cpu;
        }
    }

    return count;
}

static int get_device_numa_node(const struct dev_ctx *d, int *out_node)
{
    char path[PATH_MAX];

    if (!d || !out_node)
        return -1;

    if (snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/numa_node", d->pci_bdf) < 0)
        return -1;

    return read_int_file(path, out_node);
}

static int read_node_cpu_list(int node, int out[], int max_out)
{
    char path[PATH_MAX];
    char buf[4096];

    if (snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", node) < 0)
        return -1;
    if (read_text_line(path, buf, sizeof(buf)) < 0)
        return -1;
    return parse_cpu_list(buf, out, max_out);
}

static int get_cpu_topology_ids(int cpu, int *pkg_id, int *core_id)
{
    char path[PATH_MAX];

    if (!pkg_id || !core_id)
        return -1;

    if (snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu) < 0)
        return -1;
    if (read_int_file(path, pkg_id) < 0)
        return -1;

    if (snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu) < 0)
        return -1;
    if (read_int_file(path, core_id) < 0)
        return -1;

    return 0;
}

static int select_reflect_worker_cpus(const struct dev_ctx *d, int out[], int needed,
                                      int *out_node, bool *out_node_fallback)
{
    int allowed[CPU_SETSIZE];
    int node_cpus[CPU_SETSIZE];
    bool allowed_mask[CPU_SETSIZE] = {false};
    int candidates[CPU_SETSIZE];
    int packages[CPU_SETSIZE];
    int cores[CPU_SETSIZE];
    int allowed_count;
    int node_count = 0;
    int candidate_count = 0;
    int node;
    int out_count = 0;

    if (!d || !out || needed <= 0)
        return -1;

    allowed_count = build_cpu_list(allowed, (int)(sizeof(allowed) / sizeof(allowed[0])));
    if (allowed_count <= 0)
        return -1;
    for (int i = 0; i < allowed_count; i++)
        allowed_mask[allowed[i]] = true;

    if (get_device_numa_node(d, &node) < 0)
        return -1;

    if (node >= 0) {
        node_count = read_node_cpu_list(node, node_cpus,
                                        (int)(sizeof(node_cpus) / sizeof(node_cpus[0])));
        if (node_count < 0)
            return -1;
        for (int i = 0; i < node_count; i++) {
            int cpu = node_cpus[i];

            if (cpu < 0 || cpu >= CPU_SETSIZE || !allowed_mask[cpu])
                continue;
            candidates[candidate_count++] = cpu;
        }
        if (out_node_fallback)
            *out_node_fallback = false;
    } else {
        for (int i = 0; i < allowed_count; i++)
            candidates[candidate_count++] = allowed[i];
        if (out_node_fallback)
            *out_node_fallback = true;
    }

    for (int i = 0; i < candidate_count && out_count < needed; i++) {
        int cpu = candidates[i];
        int pkg_id;
        int core_id;
        bool seen = false;

        if (get_cpu_topology_ids(cpu, &pkg_id, &core_id) < 0)
            continue;
        for (int j = 0; j < out_count; j++) {
            if (packages[j] == pkg_id && cores[j] == core_id) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;

        packages[out_count] = pkg_id;
        cores[out_count] = core_id;
        out[out_count++] = cpu;
    }

    if (out_node)
        *out_node = node;
    return out_count;
}

static uint32_t reflect_pool_entry_count_per_queue(const struct dev_ctx *d)
{
    return (uint32_t)ICE_RX_DESC_COUNT +
           (uint32_t)d->tx_desc_count +
           ICE_REFLECT_POOL_EXTRA;
}

static uint32_t reflect_pool_entry_count_total(const struct dev_ctx *d)
{
    return (uint32_t)d->io.rxq_count * reflect_pool_entry_count_per_queue(d);
}

static size_t reflect_pool_dma_bytes_total(const struct dev_ctx *d)
{
    return (size_t)reflect_pool_entry_count_total(d) * ICE_PKT_BUF_ENTRY_SIZE;
}

static struct pkt_buf *pkt_pool_get_entry(struct pkt_mempool *pool, uint32_t idx)
{
    return (struct pkt_buf *)(pool->base + (size_t)idx * pool->entry_size);
}

static void alloc_queue_sw_state(struct dev_ctx *d)
{
    uint16_t i;

    for (i = 0; i < d->io.rxq_count; i++) {
        d->io.rxqs[i].rx_pkt_bufs = calloc(ICE_RX_DESC_COUNT, sizeof(*d->io.rxqs[i].rx_pkt_bufs));
        if (!d->io.rxqs[i].rx_pkt_bufs)
            die_errno("calloc rx pkt buf slots");

        d->reflect_pools[i].entry_size = ICE_PKT_BUF_ENTRY_SIZE;
        d->reflect_pools[i].num_entries = reflect_pool_entry_count_per_queue(d);
        d->reflect_pools[i].free_ring = calloc(d->reflect_pools[i].num_entries,
                                               sizeof(*d->reflect_pools[i].free_ring));
        if (!d->reflect_pools[i].free_ring)
            die_errno("calloc reflect pool free ring");
    }

    for (i = 0; i < d->txq_alloc_count; i++) {
        d->txqs[i].tx_pkt_buf_refs =
            calloc(d->tx_desc_count, sizeof(*d->txqs[i].tx_pkt_buf_refs));
        if (!d->txqs[i].tx_pkt_buf_refs)
            die_errno("calloc tx pkt buf refs");
    }

    d->reflect_pool.entry_size = ICE_PKT_BUF_ENTRY_SIZE;
    d->reflect_pool.num_entries = reflect_pool_entry_count_total(d);
}

static void pkt_pool_init(struct dev_ctx *d)
{
    size_t per_pool_bytes = (size_t)reflect_pool_entry_count_per_queue(d) * ICE_PKT_BUF_ENTRY_SIZE;

    memset(d->reflect_pool.base, 0, reflect_pool_dma_bytes_total(d));
    for (uint16_t q_idx = 0; q_idx < d->io.rxq_count; q_idx++) {
        struct pkt_mempool *pool = &d->reflect_pools[q_idx];

        pool->base = d->reflect_pool.base + (size_t)q_idx * per_pool_bytes;
        pool->base_iova = d->reflect_pool.base_iova + (uint64_t)q_idx * per_pool_bytes;
        for (uint32_t i = 0; i < pool->num_entries; i++) {
            struct pkt_buf *buf = pkt_pool_get_entry(pool, i);

            buf->buf_addr_iova =
                pool->base_iova + (uint64_t)i * pool->entry_size + offsetof(struct pkt_buf, data);
            buf->mempool = pool;
            buf->mempool_idx = i;
            buf->size = 0;
            pool->free_ring[i] = i;
        }
        pool->free_head = 0;
        pool->free_tail = 0;
        pool->free_count = pool->num_entries;
    }
}

static struct pkt_buf *pkt_buf_alloc(struct pkt_mempool *pool)
{
    struct pkt_buf *buf = NULL;

    if (!pool)
        return NULL;

    if (pool->free_count != 0) {
        uint32_t head = pool->free_head;
        uint32_t idx = pool->free_ring[head];

        head++;
        if (head == pool->num_entries)
            head = 0;
        pool->free_head = head;
        pool->free_count--;
        buf = pkt_pool_get_entry(pool, idx);
        buf->size = 0;
    }

    return buf;
}

static uint32_t pkt_buf_alloc_batch(struct pkt_mempool *pool, struct pkt_buf *bufs[],
                                    uint32_t num_bufs)
{
    uint32_t avail;
    uint32_t i;

    if (!pool || !bufs || num_bufs == 0)
        return 0;

    avail = pool->free_count < num_bufs ? pool->free_count : num_bufs;
    for (i = 0; i < avail; i++) {
        uint32_t head = pool->free_head;
        uint32_t idx = pool->free_ring[head];

        head++;
        if (head == pool->num_entries)
            head = 0;
        pool->free_head = head;
        pool->free_count--;
        bufs[i] = pkt_pool_get_entry(pool, idx);
        bufs[i]->size = 0;
    }

    return avail;
}

static void pkt_buf_free(struct pkt_buf *buf)
{
    struct pkt_mempool *pool;
    uint32_t tail;

    if (!buf || !buf->mempool)
        return;

    pool = buf->mempool;
    if (pool->free_count >= pool->num_entries)
        return;

    buf->size = 0;
    tail = pool->free_tail;
    pool->free_ring[tail] = buf->mempool_idx;
    tail++;
    if (tail == pool->num_entries)
        tail = 0;
    pool->free_tail = tail;
    pool->free_count++;
}

static uint32_t reg_read32(struct dev_ctx *d, uint32_t off)
{
    volatile uint32_t *p = (volatile uint32_t *)(d->bar0 + off);
    return *p;
}

static void reg_write32(struct dev_ctx *d, uint32_t off, uint32_t val)
{
    volatile uint32_t *p = (volatile uint32_t *)(d->bar0 + off);
    *p = val;
}

static void dump_hex(const uint8_t *buf, size_t len, size_t max_len)
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

static void dump_topo_response(const uint8_t *buf, size_t len, uint8_t num_branches)
{
    const struct ice_aqc_get_topo_elem *topo = (const struct ice_aqc_get_topo_elem *)buf;
    size_t max_branches = len / sizeof(*topo);
    uint16_t b;

    if (max_branches == 0) {
        fprintf(stderr, "[my_ice] topo dump: buffer too small\n");
        return;
    }

    if (num_branches == 0 || num_branches > max_branches)
        num_branches = (uint8_t)max_branches;

    fprintf(stderr, "[my_ice] topo dump: branches=%u (max=%zu)\n",
            num_branches, max_branches);

    for (b = 0; b < num_branches; b++) {
        uint16_t num_elems = le16toh(topo[b].hdr.num_elems);
        uint16_t i;

        fprintf(stderr, "  branch %u: parent_teid=0x%08x num_elems=%u\n",
                b, le32toh(topo[b].hdr.parent_teid), num_elems);

        if (num_elems > ICE_AQC_TOPO_MAX_LEVEL_NUM)
            num_elems = ICE_AQC_TOPO_MAX_LEVEL_NUM;

        for (i = 0; i < num_elems; i++) {
            const struct ice_aqc_txsched_elem_data *e = &topo[b].generic[i];
            fprintf(stderr,
                    "    [%u] type=%u node_teid=0x%08x parent_teid=0x%08x valid=0x%02x cir_prof=%u cir_alloc=%u eir_prof=%u eir_alloc=%u\n",
                    i, e->data.elem_type, le32toh(e->node_teid), le32toh(e->parent_teid),
                    e->data.valid_sections,
                    le16toh(e->data.cir_bw.bw_profile_idx),
                    le16toh(e->data.cir_bw.bw_alloc),
                    le16toh(e->data.eir_bw.bw_profile_idx),
                    le16toh(e->data.eir_bw.bw_alloc));
        }
    }

    fprintf(stderr, "[my_ice] topo raw (first 256 bytes):\n");
    dump_hex(buf, len, 256);
}

static int get_iommu_group_id(const char *bdf)
{
    char path[256];
    char target[256];
    ssize_t n;
    char *slash;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    n = readlink(path, target, sizeof(target) - 1);
    if (n < 0)
        die_errno("readlink iommu_group");
    target[n] = '\0';

    slash = strrchr(target, '/');
    if (!slash || !*(slash + 1))
        die_msg("failed to parse iommu group");

    return atoi(slash + 1);
}

static void vfio_init(struct dev_ctx *d, const char *bdf)
{
    struct vfio_group_status gstatus = { .argsz = sizeof(gstatus) };
    struct vfio_region_info bar0_rinfo = { .argsz = sizeof(bar0_rinfo), .index = VFIO_PCI_BAR0_REGION_INDEX };
    struct vfio_region_info cfg_rinfo = { .argsz = sizeof(cfg_rinfo), .index = VFIO_PCI_CONFIG_REGION_INDEX };
    char group_path[128];
    uint16_t pci_cmd;

    d->container_fd = open("/dev/vfio/vfio", O_RDWR);
    if (d->container_fd < 0)
        die_errno("open /dev/vfio/vfio");

    if (ioctl(d->container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION)
        die_msg("VFIO API version mismatch");

    if (!ioctl(d->container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU))
        die_msg("VFIO_TYPE1_IOMMU is not supported");

    d->group_id = get_iommu_group_id(bdf);
    snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", d->group_id);
    d->group_fd = open(group_path, O_RDWR);
    if (d->group_fd < 0)
        die_errno("open vfio group");

    if (ioctl(d->group_fd, VFIO_GROUP_GET_STATUS, &gstatus) < 0)
        die_errno("VFIO_GROUP_GET_STATUS");
    if (!(gstatus.flags & VFIO_GROUP_FLAGS_VIABLE))
        die_msg("vfio group is not viable");

    if (ioctl(d->group_fd, VFIO_GROUP_SET_CONTAINER, &d->container_fd) < 0)
        die_errno("VFIO_GROUP_SET_CONTAINER");

    if (ioctl(d->container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0)
        die_errno("VFIO_SET_IOMMU");

    d->device_fd = ioctl(d->group_fd, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (d->device_fd < 0)
        die_errno("VFIO_GROUP_GET_DEVICE_FD");

    if (ioctl(d->device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg_rinfo) < 0)
        die_errno("VFIO_DEVICE_GET_REGION_INFO CFG");

    if (pread(d->device_fd, &pci_cmd, sizeof(pci_cmd), cfg_rinfo.offset + PCI_COMMAND_OFF) != sizeof(pci_cmd))
        die_errno("pread PCI_COMMAND");
    pci_cmd |= (PCI_COMMAND_MEM | PCI_COMMAND_MASTER);
    if (pwrite(d->device_fd, &pci_cmd, sizeof(pci_cmd), cfg_rinfo.offset + PCI_COMMAND_OFF) != sizeof(pci_cmd))
        die_errno("pwrite PCI_COMMAND");

    if (ioctl(d->device_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_rinfo) < 0)
        die_errno("VFIO_DEVICE_GET_REGION_INFO BAR0");

    d->bar0_size = bar0_rinfo.size;
    d->bar0 = mmap(NULL, d->bar0_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   d->device_fd, bar0_rinfo.offset);
    if (d->bar0 == MAP_FAILED)
        die_errno("mmap BAR0");
}

static void dma_map(struct dev_ctx *d, size_t size)
{
    struct vfio_iommu_type1_dma_map map = {0};

    size = ALIGN_UP(size, 4096);
    if (d->huge_fd >= 0) {
        d->dma.vaddr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, d->huge_fd, 0);
    } else {
        d->dma.vaddr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (d->dma.vaddr == MAP_FAILED)
        die_errno("mmap dma");

    d->dma.size = size;
    d->dma.iova = (uint64_t)(uintptr_t)d->dma.vaddr;

    map.argsz = sizeof(map);
    map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
    map.vaddr = (uint64_t)(uintptr_t)d->dma.vaddr;
    map.iova = d->dma.iova;
    map.size = d->dma.size;

    if (ioctl(d->container_fd, VFIO_IOMMU_MAP_DMA, &map) < 0)
        die_errno("VFIO_IOMMU_MAP_DMA");
}

static void layout_dma(struct dev_ctx *d)
{
    uint8_t *base = (uint8_t *)d->dma.vaddr;
    size_t off = 0;
    struct ice_tx_desc *tx_desc_base = NULL;
    uint64_t tx_desc_iova = 0;
    uint8_t *tx_buf_base = NULL;
    uint64_t tx_buf_iova = 0;
    union ice_32b_rx_flex_desc *rx_desc_base = NULL;
    uint64_t rx_desc_iova = 0;
    uint8_t *rx_buf_base = NULL;
    uint64_t rx_buf_iova = 0;
    uint16_t i;

#define PLACE(ptr, iova_out, sz, align) \
    do { \
        off = ALIGN_UP(off, (align)); \
        (ptr) = (void *)(base + off); \
        (iova_out) = d->dma.iova + off; \
        off += (sz); \
    } while (0)

    d->atq.count = ICE_AQ_NUM_DESC;
    d->arq.count = ICE_AQ_NUM_DESC;

    PLACE(d->atq.desc, d->atq.desc_iova, ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc), 128);
    PLACE(d->arq.desc, d->arq.desc_iova, ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc), 128);
    PLACE(d->atq.buf, d->atq.buf_iova, ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN, 4096);
    PLACE(d->arq.buf, d->arq.buf_iova, ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN, 4096);

    PLACE(tx_desc_base, tx_desc_iova,
          (size_t)d->txq_alloc_count * d->tx_desc_count * sizeof(struct ice_tx_desc), 128);
    PLACE(tx_buf_base, tx_buf_iova,
          (size_t)d->txq_alloc_count * d->tx_desc_count * ICE_TX_PKT_BUF_SIZE, 128);
    PLACE(rx_desc_base, rx_desc_iova,
          (size_t)d->io.rxq_count * ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc), 128);
    PLACE(rx_buf_base, rx_buf_iova,
          (size_t)d->io.rxq_count * ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE, 128);
    PLACE(d->reflect_pool.base, d->reflect_pool.base_iova, reflect_pool_dma_bytes_total(d), 4096);

#undef PLACE

    for (i = 0; i < d->txq_alloc_count; i++) {
        struct txq_ctx *q = &d->txqs[i];
        size_t desc_off = (size_t)i * d->tx_desc_count * sizeof(struct ice_tx_desc);
        size_t buf_off = (size_t)i * d->tx_desc_count * ICE_TX_PKT_BUF_SIZE;

        q->desc_count = d->tx_desc_count;
        q->tx_desc = (struct ice_tx_desc *)((uint8_t *)tx_desc_base + desc_off);
        q->tx_desc_iova = tx_desc_iova + desc_off;
        q->tx_pkt_bufs = tx_buf_base + buf_off;
        q->tx_pkt_iova = tx_buf_iova + buf_off;
        q->tx_next_to_use = 0;
        q->tx_next_to_clean = 0;
        q->tx_pkts_since_rs = 0;
    }

    for (i = 0; i < d->io.rxq_count; i++) {
        struct rxq_ctx *q = &d->io.rxqs[i];
        size_t desc_off = (size_t)i * ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc);
        size_t buf_off = (size_t)i * ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE;

        q->rx_desc = (union ice_32b_rx_flex_desc *)((uint8_t *)rx_desc_base + desc_off);
        q->rx_desc_iova = rx_desc_iova + desc_off;
        q->rx_bufs = rx_buf_base + buf_off;
        q->rx_bufs_iova = rx_buf_iova + buf_off;
        q->rx_ntc = 0;
    }

    if (off > d->dma.size)
        die_msg("DMA layout overflow");
}

static void fill_dflt_direct_desc(struct ice_aq_desc *desc, uint16_t opcode)
{
    memset(desc, 0, sizeof(*desc));
    desc->opcode = htole16(opcode);
    desc->flags = htole16(ICE_AQ_FLAG_SI);
}

static void adminq_hw_init(struct dev_ctx *d)
{
    uint32_t i;

    memset(d->atq.desc, 0, ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc));
    memset(d->arq.desc, 0, ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc));
    memset(d->atq.buf, 0, ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN);
    memset(d->arq.buf, 0, ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN);

    for (i = 0; i < ICE_AQ_NUM_DESC; i++) {
        struct ice_aq_desc *desc = &d->arq.desc[i];
        uint64_t buf_iova = d->arq.buf_iova + (uint64_t)i * ICE_AQ_MAX_BUF_LEN;

        desc->flags = htole16(ICE_AQ_FLAG_BUF | ICE_AQ_FLAG_LB);
        desc->datalen = htole16(ICE_AQ_MAX_BUF_LEN);
        desc->params.generic.addr_high = htole32((uint32_t)(buf_iova >> 32));
        desc->params.generic.addr_low = htole32((uint32_t)(buf_iova & 0xffffffffU));
    }

    reg_write32(d, PF_FW_ATQH, 0);
    reg_write32(d, PF_FW_ATQT, 0);
    reg_write32(d, PF_FW_ATQLEN, ICE_AQ_NUM_DESC | PF_FW_ATQLEN_ATQENABLE_M);
    reg_write32(d, PF_FW_ATQBAL, (uint32_t)(d->atq.desc_iova & 0xffffffffU));
    reg_write32(d, PF_FW_ATQBAH, (uint32_t)(d->atq.desc_iova >> 32));

    reg_write32(d, PF_FW_ARQH, 0);
    reg_write32(d, PF_FW_ARQT, 0);
    reg_write32(d, PF_FW_ARQLEN, ICE_AQ_NUM_DESC | PF_FW_ARQLEN_ARQENABLE_M);
    reg_write32(d, PF_FW_ARQBAL, (uint32_t)(d->arq.desc_iova & 0xffffffffU));
    reg_write32(d, PF_FW_ARQBAH, (uint32_t)(d->arq.desc_iova >> 32));

    reg_write32(d, PF_FW_ARQT, ICE_AQ_NUM_DESC - 1);

    d->atq.next_to_use = 0;
    d->arq.next_to_use = 0;

    if (reg_read32(d, PF_FW_ATQBAL) != (uint32_t)(d->atq.desc_iova & 0xffffffffU))
        die_msg("ATQBAL verification failed");
}

static int aq_send_cmd(struct dev_ctx *d, struct ice_aq_desc *desc, void *buf, uint16_t buf_size)
{
    struct aq_ring_ctx *sq = &d->atq;
    struct ice_aq_desc *ring_desc;
    struct timespec start, now;
    const int timeout_ms = 2000;
    uint16_t ntu = sq->next_to_use;
    uint16_t next;
    uint32_t head;

    head = reg_read32(d, PF_FW_ATQH) & 0x3ff;
    next = (uint16_t)((ntu + 1) % sq->count);
    if (next == head) {
        fprintf(stderr, "ATQ full\n");
        return -1;
    }

    ring_desc = &sq->desc[ntu];
    memcpy(ring_desc, desc, sizeof(*ring_desc));

    if (buf && buf_size) {
        uint64_t biova = sq->buf_iova + (uint64_t)ntu * ICE_AQ_MAX_BUF_LEN;
        uint8_t *b = sq->buf + (size_t)ntu * ICE_AQ_MAX_BUF_LEN;

        memcpy(b, buf, buf_size);
        ring_desc->flags = htole16(le16toh(ring_desc->flags) | ICE_AQ_FLAG_BUF);
        if (buf_size > ICE_AQ_LG_BUF)
            ring_desc->flags = htole16(le16toh(ring_desc->flags) | ICE_AQ_FLAG_LB);
        ring_desc->datalen = htole16(buf_size);
        ring_desc->params.generic.addr_high = htole32((uint32_t)(biova >> 32));
        ring_desc->params.generic.addr_low = htole32((uint32_t)(biova & 0xffffffffU));
    }

    sq->next_to_use = next;
    reg_write32(d, PF_FW_ATQT, sq->next_to_use);
    (void)reg_read32(d, PF_FW_ATQT);

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        die_errno("clock_gettime start");

    for (;;) {
        long elapsed_ms;

        if ((reg_read32(d, PF_FW_ATQH) & 0x3ff) == sq->next_to_use)
            break;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            die_errno("clock_gettime now");

        elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
                     (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= timeout_ms)
            break;
        usleep(100);
    }

    if ((reg_read32(d, PF_FW_ATQH) & 0x3ff) != sq->next_to_use) {
        fprintf(stderr, "AQ timeout (ATQH=0x%x ATQLEN=0x%x ARQLEN=0x%x)\n",
                reg_read32(d, PF_FW_ATQH), reg_read32(d, PF_FW_ATQLEN), reg_read32(d, PF_FW_ARQLEN));
        return -1;
    }

    memcpy(desc, ring_desc, sizeof(*desc));

    if (buf && buf_size) {
        uint8_t *b = sq->buf + (size_t)ntu * ICE_AQ_MAX_BUF_LEN;
        uint16_t copy_sz = le16toh(desc->datalen);
        if (copy_sz > buf_size)
            copy_sz = buf_size;
        memcpy(buf, b, copy_sz);
    }

    if (le16toh(desc->retval)) {
        fprintf(stderr, "AQ command 0x%04x failed, retval=0x%04x\n",
                le16toh(desc->opcode), le16toh(desc->retval));
        return -1;
    }

    return 0;
}

static int aq_get_fw_ver(struct dev_ctx *d)
{
    struct ice_aq_desc desc;
    struct ice_aqc_get_ver *v;

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_GET_VER);
    if (aq_send_cmd(d, &desc, NULL, 0) < 0)
        return -1;

    v = &desc.params.get_ver;
    printf("Firmware: %u.%u.%u build %u branch %u\n",
           v->fw_major, v->fw_minor, v->fw_patch,
           le32toh(v->fw_build), v->fw_branch);
    printf("API: %u.%u.%u branch %u\n",
           v->api_major, v->api_minor, v->api_patch, v->api_branch);

    return 0;
}

static int aq_manage_mac_read(struct dev_ctx *d)
{
    struct ice_aq_desc desc;
    struct ice_aqc_manage_mac_read *cmd;
    struct ice_aqc_manage_mac_read_resp resp[8];
    uint16_t flags;
    uint8_t i;
    bool found = false;

    memset(resp, 0, sizeof(resp));
    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_MANAGE_MAC_READ);

    if (aq_send_cmd(d, &desc, resp, sizeof(resp)) < 0)
        return -1;

    cmd = &desc.params.mac_read;
    flags = le16toh(cmd->flags) & ICE_AQC_MAN_MAC_READ_M;

    if (!(flags & ICE_AQC_MAN_MAC_LAN_ADDR_VALID))
        fprintf(stderr, "LAN MAC not marked valid by firmware (flags=0x%04x)\n", flags);

    for (i = 0; i < cmd->num_addr && i < 8; i++) {
        if (resp[i].addr_type == ICE_AQC_MAN_MAC_ADDR_TYPE_LAN) {
            memcpy(d->io.mac, resp[i].mac_addr, ETHER_ADDR_LEN);
            d->io.lport = resp[i].lport_num;
            printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                   d->io.mac[0], d->io.mac[1], d->io.mac[2],
                   d->io.mac[3], d->io.mac[4], d->io.mac[5]);
            found = true;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "No LAN MAC in response (num_addr=%u)\n", cmd->num_addr);
        return -1;
    }

    return 0;
}

static int aq_get_default_vsi_and_lport(struct dev_ctx *d)
{
    struct ice_aqc_get_sw_cfg_resp_elem buf[256];
    struct ice_aq_desc desc;
    uint16_t req_desc = 0;
    bool got_vsi = false;
    bool got_lport = false;

    do {
        uint16_t num_elems;
        size_t i;

        memset(buf, 0, sizeof(buf));
        fill_dflt_direct_desc(&desc, ICE_AQC_OPC_GET_SW_CFG);
        desc.params.get_sw_conf.element = htole16(req_desc);

        if (aq_send_cmd(d, &desc, buf, sizeof(buf)) < 0)
            return -1;

        req_desc = le16toh(desc.params.get_sw_conf.element);
        num_elems = le16toh(desc.params.get_sw_conf.num_elems);
        if (num_elems > (uint16_t)(sizeof(buf) / sizeof(buf[0])))
            num_elems = (uint16_t)(sizeof(buf) / sizeof(buf[0]));

        for (i = 0; i < num_elems; i++) {
            uint16_t vsi_port_num = le16toh(buf[i].vsi_port_num);
            uint16_t pf_vf_num = le16toh(buf[i].pf_vf_num);
            uint16_t num = vsi_port_num & ICE_AQC_GET_SW_CONF_RESP_VSI_PORT_NUM_M;
            uint8_t type = (uint8_t)((vsi_port_num & ICE_AQC_GET_SW_CONF_RESP_TYPE_M) >>
                                     ICE_AQC_GET_SW_CONF_RESP_TYPE_S);
            bool is_vf = !!(pf_vf_num & ICE_AQC_GET_SW_CONF_RESP_IS_VF);

            if (is_vf)
                continue;

            if (type == ICE_AQC_GET_SW_CONF_RESP_PHYS_PORT && !got_lport) {
                d->io.lport = (uint8_t)(num & 0xff);
                got_lport = true;
            }

            if (type == ICE_AQC_GET_SW_CONF_RESP_VSI && !got_vsi) {
                d->io.vsi_num = num;
                got_vsi = true;
            }
        }
    } while (req_desc != 0);

    if (!got_vsi) {
        fprintf(stderr, "failed to discover default VSI number from GET_SW_CFG\n");
        return -1;
    }
    if (!got_lport) {
        fprintf(stderr, "failed to discover physical lport from GET_SW_CFG\n");
        return -1;
    }

    return 0;
}

static int aq_get_qparent_teid(struct dev_ctx *d)
{
    uint8_t topo_buf[ICE_AQ_MAX_BUF_LEN];
    struct ice_aqc_get_topo_elem *topo = (struct ice_aqc_get_topo_elem *)topo_buf;
    struct ice_aq_desc desc;
    uint8_t num_branches;
    uint16_t num_elems;

    if (g_qparent_override_set) {
        d->io.qparent_teid = g_qparent_override;
        fprintf(stderr, "[my_ice] using override qparent_teid=0x%08x\n",
                d->io.qparent_teid);
        return 0;
    }

    memset(topo_buf, 0, sizeof(topo_buf));
    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_GET_DFLT_TOPO);
    desc.params.get_topo.port_num = d->io.lport;

    if (aq_send_cmd(d, &desc, topo_buf, sizeof(topo_buf)) < 0) {
        fprintf(stderr, "[my_ice] GET_DFLT_TOPO failed for lport=%u\n", d->io.lport);
        return -1;
    }

    num_branches = desc.params.get_topo.num_branches;
    if (g_dump_topo)
        dump_topo_response(topo_buf, sizeof(topo_buf), num_branches);
    if (num_branches < 1 || num_branches > 8) {
        fprintf(stderr, "[my_ice] GET_DFLT_TOPO invalid num_branches=%u\n", num_branches);
        return -1;
    }

    num_elems = le16toh(topo[0].hdr.num_elems);
    if (num_elems < 2 || num_elems > ICE_AQC_TOPO_MAX_LEVEL_NUM) {
        fprintf(stderr, "[my_ice] GET_DFLT_TOPO invalid num_elems=%u\n", num_elems);
        return -1;
    }

    if (topo[0].generic[num_elems - 1].data.elem_type == ICE_AQC_ELEM_TYPE_LEAF)
        d->io.qparent_teid = le32toh(topo[0].generic[num_elems - 2].node_teid);
    else
        d->io.qparent_teid = le32toh(topo[0].generic[num_elems - 1].node_teid);
    fprintf(stderr, "[my_ice] selected qparent_teid=0x%08x\n", d->io.qparent_teid);
    return 0;
}

struct my_sw_rule_lkup_rx_tx {
    struct {
        uint16_t type;
        uint16_t status;
    } hdr;
    uint16_t recipe_id;
    uint16_t src;
    uint32_t act;
    uint16_t index;
    uint16_t hdr_len;
    uint8_t hdr_data[ICE_DUMMY_ETH_HDR_LEN];
} __attribute__((packed));

static int aq_add_rx_mac_rule(struct dev_ctx *d, uint16_t *rule_idx)
{
    static const uint8_t dummy_eth_header[ICE_DUMMY_ETH_HDR_LEN] = {
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x81, 0x00, 0x00, 0x00
    };
    struct my_sw_rule_lkup_rx_tx rule = {0};
    struct ice_aq_desc desc;
    uint32_t act = 0;

    rule.hdr.type = htole16(ICE_AQC_SW_RULES_T_LKUP_RX);
    rule.recipe_id = htole16(ICE_SW_LKUP_MAC);
    rule.src = htole16(d->io.lport);
    act |= ((uint32_t)d->io.vsi_num << ICE_SINGLE_ACT_VSI_ID_S) &
           ICE_SINGLE_ACT_VSI_ID_M;
    act |= ICE_SINGLE_ACT_VSI_FORWARDING | ICE_SINGLE_ACT_VALID_BIT;
    rule.act = htole32(act);
    rule.hdr_len = htole16(ICE_DUMMY_ETH_HDR_LEN);
    memcpy(rule.hdr_data, dummy_eth_header, sizeof(dummy_eth_header));
    memcpy(rule.hdr_data, d->io.mac, ETHER_ADDR_LEN);

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_ADD_SW_RULES);
    desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);
    desc.params.sw_rules.num_rules_fltr_entry_index = htole16(1);

    if (aq_send_cmd(d, &desc, &rule, sizeof(rule)) < 0)
        return -1;

    if (rule_idx)
        *rule_idx = le16toh(rule.index);
    fprintf(stderr, "[my_ice] ADD_SW_RULES RX MAC index=%u\n",
            le16toh(rule.index));
    return 0;
}

static void aq_remove_sw_rule_best_effort(struct dev_ctx *d, uint16_t rule_type,
                                          uint16_t rule_idx)
{
    struct my_sw_rule_lkup_rx_tx rule = {0};
    struct ice_aq_desc desc;

    if (rule_idx == UINT16_MAX)
        return;

    rule.hdr.type = htole16(rule_type);
    rule.index = htole16(rule_idx);

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_REMOVE_SW_RULES);
    desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);
    desc.params.sw_rules.num_rules_fltr_entry_index = htole16(1);

    if (aq_send_cmd(d, &desc, &rule, sizeof(rule)) < 0)
        fprintf(stderr, "[my_ice] warning: failed to remove switch rule %u\n",
                rule_idx);
    else
        fprintf(stderr, "[my_ice] removed switch rule %u\n", rule_idx);
}

static void set_ctx_bits(uint8_t *src_ctx, uint8_t *dest_ctx,
                         const struct ice_ctx_ele *ce_info)
{
    int f;

    for (f = 0; ce_info[f].width; f++) {
        uint64_t v = 0;
        uint16_t bit;

        if (ce_info[f].width > ce_info[f].size_of * 8)
            continue;

        memcpy(&v, src_ctx + ce_info[f].offset, ce_info[f].size_of);

        for (bit = 0; bit < ce_info[f].width; bit++) {
            uint32_t dst_bit = ce_info[f].lsb + bit;
            uint8_t *dst_byte = &dest_ctx[dst_bit / 8];
            uint8_t dst_mask = (uint8_t)(1U << (dst_bit % 8));
            if (v & (1ULL << bit))
                *dst_byte |= dst_mask;
            else
                *dst_byte &= (uint8_t)~dst_mask;
        }
    }
}

static uint16_t ceil_log2_u16(uint16_t v)
{
    uint16_t pow = 0;
    uint16_t x = 1;

    while (x < v && pow < 15) {
        x <<= 1;
        pow++;
    }

    return pow;
}

static int get_rx_queue_block(struct dev_ctx *d, uint16_t *first_q, uint16_t *last_q)
{
    uint32_t rx_alloc = reg_read32(d, PFLAN_RX_QALLOC);

    if (!(rx_alloc & PFLAN_RX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (RX=0x%08x)\n", rx_alloc);
        return -1;
    }

    *first_q = (uint16_t)(rx_alloc & PFLAN_RX_QALLOC_FIRSTQ_M);
    *last_q = (uint16_t)((rx_alloc & PFLAN_RX_QALLOC_LASTQ_M) >> PFLAN_RX_QALLOC_LASTQ_S);
    return 0;
}

static void assign_rx_queue_ids(struct dev_ctx *d, uint16_t first_q)
{
    uint16_t i;

    for (i = 0; i < d->io.rxq_count; i++) {
        d->io.rxqs[i].rxq_id = (uint16_t)(first_q + i);
        d->io.rxqs[i].rss_sampled = false;
    }
}

static int add_tx_queues(struct dev_ctx *d, uint16_t count)
{
    struct ice_aqc_add_tx_qgrp *qg;
    struct ice_aq_desc desc;
    size_t qg_size;
    uint16_t i;

    if (count == 0)
        return -1;
    if (count > 255)
        count = 255;

    qg_size = sizeof(*qg) + (size_t)(count - 1) * sizeof(struct ice_aqc_add_txqs_perq);
    qg = calloc(1, qg_size);
    if (!qg)
        return -1;

    qg->parent_teid = htole32(d->io.qparent_teid);
    qg->num_txqs = (uint8_t)count;

    for (i = 0; i < count; i++) {
        struct txq_ctx *q = &d->txqs[i];
        struct ice_tlan_ctx tlan = {0};

        tlan.port_num = d->io.lport;
        tlan.qlen = d->tx_desc_count;
        tlan.base = q->tx_desc_iova >> 7;
        tlan.pf_num = 0;
        tlan.vmvf_type = ICE_TLAN_CTX_VMVF_TYPE_PF;
        tlan.src_vsi = d->io.vsi_num;
        tlan.tso_ena = 1;
        tlan.internal_usage_flag = 1;
        tlan.tso_qnum = q->txq_id;
        tlan.legacy_int = 1;

        qg->txqs[i].txq_id = htole16(q->txq_id);
        set_ctx_bits((uint8_t *)&tlan, qg->txqs[i].txq_ctx, tlan_ctx_info);

        qg->txqs[i].info.valid_sections =
            ICE_AQC_ELEM_VALID_GENERIC | ICE_AQC_ELEM_VALID_CIR | ICE_AQC_ELEM_VALID_EIR;
        qg->txqs[i].info.generic = 0;
        qg->txqs[i].info.cir_bw.bw_profile_idx = htole16(ICE_SCHED_DFLT_RL_PROF_ID);
        qg->txqs[i].info.cir_bw.bw_alloc = htole16(ICE_SCHED_DFLT_BW_WT);
        qg->txqs[i].info.eir_bw.bw_profile_idx = htole16(ICE_SCHED_DFLT_RL_PROF_ID);
        qg->txqs[i].info.eir_bw.bw_alloc = htole16(ICE_SCHED_DFLT_BW_WT);
    }

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_ADD_TXQS);
    desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);
    desc.params.add_txqs.num_qgrps = 1;

    if (aq_send_cmd(d, &desc, qg, (uint16_t)qg_size) < 0) {
        free(qg);
        return -1;
    }

    fprintf(stderr, "[my_ice] ADD_TXQS resp:");
    for (i = 0; i < count; i++) {
        fprintf(stderr, " txq_id=%u q_teid=0x%x",
                le16toh(qg->txqs[i].txq_id), le32toh(qg->txqs[i].q_teid));
    }
    fprintf(stderr, "\n");

    free(qg);
    return 0;
}

static void rss_log_lut_summary(const char *tag, const uint8_t *lut, uint16_t lut_size,
                                uint16_t num_rxq)
{
    uint16_t counts[ICE_MAX_RX_QUEUES] = {0};
    uint16_t i;

    for (i = 0; i < lut_size; i++) {
        uint8_t q = lut[i];
        if (q < ICE_MAX_RX_QUEUES)
            counts[q]++;
    }

    fprintf(stderr, "[my_ice] %s size=%u queues=%u", tag, lut_size, num_rxq); // RSS:IMPL
    for (i = 0; i < num_rxq && i < ICE_MAX_RX_QUEUES; i++)
        fprintf(stderr, " q%u=%u", i, counts[i]); // RSS:IMPL
    fprintf(stderr, "\n"); // RSS:IMPL
}

static int aq_set_rss_key(struct dev_ctx *d)
{
    struct ice_aq_desc desc;
    struct ice_aqc_get_set_rss_key *rss;
    struct ice_aqc_get_set_rss_keys key;

    key = g_default_rss_key;
    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_SET_RSS_KEY);
    desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);
    rss = &desc.params.get_set_rss_key;
    rss->vsi_id = htole16((uint16_t)(d->io.vsi_num | ICE_AQC_RSS_VSI_VALID));

    fprintf(stderr, "[my_ice] RSS set key vsi=%u len=%u\n", d->io.vsi_num,
            ICE_AQC_RSS_KEY_SIZE); // RSS:IMPL
    return aq_send_cmd(d, &desc, &key, sizeof(key));
}

static int aq_get_set_rss_lut(struct dev_ctx *d, uint16_t flags, uint8_t *lut,
                              uint16_t lut_size, bool set)
{
    struct ice_aq_desc desc;
    struct ice_aqc_get_set_rss_lut *cmd;

    fill_dflt_direct_desc(&desc, set ? ICE_AQC_OPC_SET_RSS_LUT : ICE_AQC_OPC_GET_RSS_LUT);
    if (set)
        desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);

    cmd = &desc.params.get_set_rss_lut;
    cmd->vsi_id = htole16((uint16_t)(d->io.vsi_num | ICE_AQC_RSS_VSI_VALID));
    cmd->flags = htole16(flags);

    fprintf(stderr, "[my_ice] RSS %s LUT vsi=%u flags=0x%04x len=%u\n",
            set ? "set" : "get", d->io.vsi_num, flags, lut_size); // RSS:IMPL
    return aq_send_cmd(d, &desc, lut, lut_size);
}

static void rss_fill_round_robin_lut(uint8_t *lut, uint16_t lut_size, uint16_t num_rxq)
{
    uint16_t i;

    for (i = 0; i < lut_size; i++)
        lut[i] = (uint8_t)(i % num_rxq);
}

static int aq_get_vsi_params(struct dev_ctx *d, struct ice_aqc_vsi_props *props)
{
    struct ice_aq_desc desc;
    struct ice_aqc_add_get_update_free_vsi *cmd;

    memset(props, 0, sizeof(*props));
    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_GET_VSI_PARAMS);
    cmd = &desc.params.vsi_cmd;
    cmd->vsi_num = htole16((uint16_t)(d->io.vsi_num | ICE_AQ_VSI_IS_VALID));

    if (aq_send_cmd(d, &desc, props, sizeof(*props)) < 0)
        return -1;

    fprintf(stderr,
            "[my_ice] RSS get VSI vsi=%u valid=0x%04x map_flags=0x%04x q_map0=%u q_map1=%u tc0=0x%04x q_opt_rss=0x%02x q_opt_tc=0x%02x q_opt_flags=0x%02x\n",
            d->io.vsi_num, le16toh(props->valid_sections), le16toh(props->mapping_flags),
            le16toh(props->q_mapping[0]), le16toh(props->q_mapping[1]),
            le16toh(props->tc_mapping[0]), props->q_opt_rss, props->q_opt_tc,
            props->q_opt_flags); // RSS:IMPL
    return 0;
}

static int aq_update_vsi_rss(struct dev_ctx *d)
{
    struct ice_aq_desc desc;
    struct ice_aqc_add_get_update_free_vsi *cmd;
    struct ice_aqc_vsi_props props;
    uint16_t valid;
    uint16_t first_rxq = d->io.rxqs[0].rxq_id;
    uint16_t pow = ceil_log2_u16(d->io.rxq_count);
    uint16_t tc_map;

    memset(&props, 0, sizeof(props));
    valid = ICE_AQ_VSI_PROP_RXQ_MAP_VALID | ICE_AQ_VSI_PROP_Q_OPT_VALID;
    props.valid_sections = htole16(valid);
    props.mapping_flags = htole16(ICE_AQ_VSI_Q_MAP_CONTIG);
    props.q_mapping[0] = htole16(first_rxq);
    props.q_mapping[1] = htole16(d->io.rxq_count);
    tc_map = (uint16_t)((0U << ICE_AQ_VSI_TC_Q_OFFSET_S) |
                        (pow << ICE_AQ_VSI_TC_Q_NUM_S));
    props.tc_mapping[0] = htole16(tc_map);
    props.q_opt_rss = (uint8_t)(ICE_AQ_VSI_Q_OPT_RSS_LUT_PF |
                                (ICE_AQ_VSI_Q_OPT_RSS_HASH_TPLZ <<
                                 ICE_AQ_VSI_Q_OPT_RSS_HASH_S));

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_UPDATE_VSI);
    desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);
    cmd = &desc.params.vsi_cmd;
    cmd->vsi_num = htole16((uint16_t)(d->io.vsi_num | ICE_AQ_VSI_IS_VALID));

    fprintf(stderr,
            "[my_ice] RSS update VSI vsi=%u first_rxq=%u num_rxq=%u tc0=0x%04x q_opt_rss=0x%02x\n",
            d->io.vsi_num, first_rxq, d->io.rxq_count, tc_map,
            props.q_opt_rss); // RSS:IMPL
    return aq_send_cmd(d, &desc, &props, sizeof(props));
}

static int aq_get_pkg_info_list(struct dev_ctx *d)
{
    uint8_t buf[ICE_AQ_MAX_BUF_LEN];
    struct ice_aq_desc desc;
    uint32_t count;
    uint32_t i;

    memset(buf, 0, sizeof(buf));
    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_GET_PKG_INFO_LIST);
    if (aq_send_cmd(d, &desc, buf, sizeof(buf)) < 0)
        return -1;

    memcpy(&count, buf, sizeof(count));
    count = le32toh(count);
    fprintf(stderr, "[my_ice] RSS package info count=%u\n", count); // RSS:IMPL
    for (i = 0; i < count; i++) {
        const struct ice_aqc_get_pkg_info *info;
        size_t off = sizeof(uint32_t) + (size_t)i * sizeof(*info);

        if (off + sizeof(*info) > sizeof(buf))
            break;
        info = (const struct ice_aqc_get_pkg_info *)(buf + off);
        fprintf(stderr,
                "[my_ice] RSS package[%u] active=%u boot=%u modified=%u name='%.*s' ver=%u.%u.%u.%u\n",
                i, info->is_active, info->is_active_at_boot, info->is_modified,
                (int)sizeof(info->name), info->name, info->ver.major, info->ver.minor,
                info->ver.update, info->ver.draft); // RSS:IMPL
    }
    return 0;
}

struct rss_xlt2_entry {
    uint16_t vsi;
    uint16_t vsig;
};

static int aq_upload_section(struct dev_ctx *d, void *buf, uint16_t buf_size)
{
    struct ice_aq_desc desc;

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_UPLOAD_SECTION);
    desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);
    return aq_send_cmd(d, &desc, buf, buf_size);
}

static int rss_build_section_request(void *buf, uint16_t buf_size, uint32_t section_type,
                                     uint16_t section_size)
{
    struct ice_buf_hdr *hdr = buf;
    struct ice_section_entry *entry;

    if (!buf || buf_size < (uint16_t)(sizeof(*hdr) + sizeof(*entry) + section_size))
        return -1;

    memset(buf, 0, buf_size);
    hdr->section_count = htole16(1);
    hdr->data_end = htole16((uint16_t)(sizeof(*hdr) + sizeof(*entry) + section_size));
    entry = hdr->section_entry;
    entry->type = htole32(section_type);
    entry->offset = htole16((uint16_t)(sizeof(*hdr) + sizeof(*entry)));
    entry->size = htole16(section_size);
    return 0;
}

static int aq_read_xlt2_rss(struct dev_ctx *d, uint16_t vsigs[ICE_XLT2_CNT])
{
    uint8_t buf[1600];
    struct ice_xlt2_section *xlt2;
    uint16_t i;

    if (rss_build_section_request(buf, sizeof(buf), ICE_SID_XLT2_RSS,
                                  (uint16_t)(sizeof(*xlt2) + ICE_XLT2_CNT * sizeof(uint16_t))) < 0)
        return -1;

    xlt2 = (struct ice_xlt2_section *)(buf + sizeof(struct ice_buf_hdr) +
                                       sizeof(struct ice_section_entry));
    xlt2->count = htole16(ICE_XLT2_CNT);
    xlt2->offset = 0;

    if (aq_upload_section(d, buf, sizeof(buf)) < 0)
        return -1;

    for (i = 0; i < ICE_XLT2_CNT; i++) {
        uint16_t v;
        memcpy(&v, xlt2->value + i, sizeof(v));
        vsigs[i] = le16toh(v);
    }

    fprintf(stderr, "[my_ice] RSS read XLT2 entries=%u\n", ICE_XLT2_CNT); // RSS:IMPL
    return 0;
}

static int aq_read_xlt1_rss(struct dev_ctx *d, uint8_t ptgs[ICE_XLT1_CNT])
{
    uint8_t buf[1200];
    struct ice_xlt1_section *xlt1;

    if (rss_build_section_request(buf, sizeof(buf), ICE_SID_XLT1_RSS,
                                  (uint16_t)(sizeof(*xlt1) + ICE_XLT1_CNT)) < 0)
        return -1;

    xlt1 = (struct ice_xlt1_section *)(buf + sizeof(struct ice_buf_hdr) +
                                       sizeof(struct ice_section_entry));
    xlt1->count = htole16(ICE_XLT1_CNT);
    xlt1->offset = 0;

    if (aq_upload_section(d, buf, sizeof(buf)) < 0)
        return -1;

    memcpy(ptgs, xlt1->value, ICE_XLT1_CNT);
    fprintf(stderr, "[my_ice] RSS read XLT1 entries=%u\n", ICE_XLT1_CNT); // RSS:IMPL
    return 0;
}

static int aq_read_prof_redir_rss(struct dev_ctx *d, uint8_t prof_redir[ICE_RSS_PROF_ID_COUNT])
{
    uint8_t buf[160];
    struct ice_prof_redir_section *redir;

    if (rss_build_section_request(buf, sizeof(buf), ICE_SID_PROFID_REDIR_RSS,
                                  (uint16_t)(sizeof(*redir) + ICE_RSS_PROF_ID_COUNT)) < 0)
        return -1;

    redir = (struct ice_prof_redir_section *)(buf + sizeof(struct ice_buf_hdr) +
                                              sizeof(struct ice_section_entry));
    redir->count = htole16(ICE_RSS_PROF_ID_COUNT);
    redir->offset = 0;

    if (aq_upload_section(d, buf, sizeof(buf)) < 0)
        return -1;

    memcpy(prof_redir, redir->redir_value, ICE_RSS_PROF_ID_COUNT);
    fprintf(stderr, "[my_ice] RSS read PROFID_REDIR entries=%u\n", ICE_RSS_PROF_ID_COUNT); // RSS:IMPL
    return 0;
}

static int aq_read_prof_tcam_rss(struct dev_ctx *d,
                                 struct ice_prof_tcam_entry tcam[ICE_RSS_PROF_TCAM_COUNT])
{
    uint16_t base = 0;

    memset(tcam, 0, sizeof(struct ice_prof_tcam_entry) * ICE_RSS_PROF_TCAM_COUNT);
    while (base < ICE_RSS_PROF_TCAM_COUNT) {
        uint8_t buf[ICE_AQ_MAX_BUF_LEN];
        struct ice_prof_id_section *sec;
        uint16_t max_entries;
        uint16_t chunk;
        uint16_t section_size;
        uint16_t i;

        max_entries = (uint16_t)((ICE_AQ_MAX_BUF_LEN - sizeof(struct ice_buf_hdr) -
                                  sizeof(struct ice_section_entry) - sizeof(*sec)) /
                                 sizeof(struct ice_prof_tcam_entry));
        if (max_entries == 0)
            return -1;
        chunk = (uint16_t)(ICE_RSS_PROF_TCAM_COUNT - base);
        if (chunk > max_entries)
            chunk = max_entries;
        section_size = (uint16_t)(sizeof(*sec) + chunk * sizeof(struct ice_prof_tcam_entry));
        if (rss_build_section_request(buf, sizeof(buf), ICE_SID_PROFID_TCAM_RSS, section_size) < 0)
            return -1;

        sec = (struct ice_prof_id_section *)(buf + sizeof(struct ice_buf_hdr) +
                                             sizeof(struct ice_section_entry));
        sec->count = htole16(chunk);
        for (i = 0; i < chunk; i++)
            sec->entry[i].addr = htole16((uint16_t)(base + i));

        if (aq_upload_section(d, buf, sizeof(buf)) < 0)
            return -1;

        memcpy(tcam + base, sec->entry, chunk * sizeof(struct ice_prof_tcam_entry));
        base = (uint16_t)(base + chunk);
    }

    fprintf(stderr, "[my_ice] RSS read PROFID_TCAM entries=%u\n", ICE_RSS_PROF_TCAM_COUNT); // RSS:IMPL
    return 0;
}

static int aq_read_fld_vec_rss(struct dev_ctx *d,
                               struct ice_fv_word fv[ICE_RSS_PROF_ID_COUNT][ICE_RSS_FV_WORDS])
{
    uint16_t prof = 0;

    memset(fv, 0, sizeof(struct ice_fv_word) * ICE_RSS_PROF_ID_COUNT * ICE_RSS_FV_WORDS);
    while (prof < ICE_RSS_PROF_ID_COUNT) {
        uint8_t buf[ICE_AQ_MAX_BUF_LEN];
        struct ice_pkg_es *es;
        uint16_t max_prof;
        uint16_t prof_count;
        uint16_t vec_bytes = (uint16_t)(ICE_RSS_FV_WORDS * sizeof(struct ice_fv_word));
        uint16_t section_size;
        uint16_t i;

        max_prof = (uint16_t)((ICE_AQ_MAX_BUF_LEN - sizeof(struct ice_buf_hdr) -
                               sizeof(struct ice_section_entry) - sizeof(*es)) / vec_bytes);
        if (max_prof == 0)
            return -1;
        prof_count = (uint16_t)(ICE_RSS_PROF_ID_COUNT - prof);
        if (prof_count > max_prof)
            prof_count = max_prof;
        section_size = (uint16_t)(sizeof(*es) + prof_count * vec_bytes);
        if (rss_build_section_request(buf, sizeof(buf), ICE_SID_FLD_VEC_RSS, section_size) < 0)
            return -1;

        es = (struct ice_pkg_es *)(buf + sizeof(struct ice_buf_hdr) +
                                   sizeof(struct ice_section_entry));
        es->count = htole16(prof_count);
        es->offset = htole16(prof);

        if (aq_upload_section(d, buf, sizeof(buf)) < 0)
            return -1;

        for (i = 0; i < prof_count; i++) {
            memcpy(fv[prof + i], es->es + (size_t)i * ICE_RSS_FV_WORDS, vec_bytes);
        }
        prof = (uint16_t)(prof + prof_count);
    }

    fprintf(stderr, "[my_ice] RSS read FLD_VEC entries=%u fvw=%u\n",
            ICE_RSS_PROF_ID_COUNT, ICE_RSS_FV_WORDS); // RSS:IMPL
    return 0;
}

static void rss_log_profile_fv(uint8_t prof_id,
                               const struct ice_fv_word fv[ICE_RSS_FV_WORDS])
{
    uint16_t i;
    bool any = false;

    fprintf(stderr, "[my_ice] RSS profile prof_id=%u fv", prof_id); // RSS:IMPL
    for (i = 0; i < ICE_RSS_FV_WORDS; i++) {
        uint16_t off = le16toh(fv[i].off);

        if (off == ICE_FV_OFFSET_INVAL)
            continue;
        any = true;
        fprintf(stderr, " [%u:p=%u off=%u]", i, fv[i].prot_id, off); // RSS:IMPL
    }
    if (!any)
        fprintf(stderr, " <empty>"); // RSS:IMPL
    fprintf(stderr, "\n"); // RSS:IMPL
}

static bool rss_decode_key_byte(uint8_t key, uint8_t key_inv, uint8_t *val,
                                uint8_t *dc, uint8_t *nm)
{
    uint8_t out_val = 0;
    uint8_t out_dc = 0;
    uint8_t out_nm = 0;
    uint8_t bit;

    for (bit = 0; bit < 8; bit++) {
        uint8_t k = (uint8_t)((key >> bit) & 0x1U);
        uint8_t ki = (uint8_t)((key_inv >> bit) & 0x1U);

        if (k && ki) {
            out_dc |= (uint8_t)(1U << bit);
        } else if (!k && !ki) {
            out_nm |= (uint8_t)(1U << bit);
        } else if (!k && ki) {
            out_val |= (uint8_t)(1U << bit);
        } else if (!(k && !ki)) {
            return false;
        }
    }

    *val = out_val;
    *dc = out_dc;
    *nm = out_nm;
    return true;
}

static bool rss_decode_tcam_entry(const struct ice_prof_tcam_entry *entry,
                                  struct ice_prof_id_key *key_out,
                                  uint8_t dc_mask[sizeof(struct ice_prof_id_key)],
                                  uint8_t nm_mask[sizeof(struct ice_prof_id_key)])
{
    uint8_t raw[sizeof(struct ice_prof_id_key)] = {0};
    size_t i;

    for (i = 0; i < sizeof(raw); i++) {
        if (!rss_decode_key_byte(entry->key[i], entry->key[i + sizeof(raw)],
                                 &raw[i], &dc_mask[i], &nm_mask[i]))
            return false;
    }

    memcpy(key_out, raw, sizeof(*key_out));
    return true;
}

static bool rss_mask_any_set(const uint8_t *mask, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (mask[i] != 0)
            return true;
    }
    return false;
}

static void rss_inspect_active_vsig(struct dev_ctx *d, uint16_t active_vsig)
{
    uint8_t ptgs[ICE_XLT1_CNT];
    uint8_t prof_redir[ICE_RSS_PROF_ID_COUNT];
    struct ice_prof_tcam_entry tcam[ICE_RSS_PROF_TCAM_COUNT];
    struct ice_fv_word fv[ICE_RSS_PROF_ID_COUNT][ICE_RSS_FV_WORDS];
    uint16_t ptg_ptype_counts[256] = {0};
    bool matched_ptg_seen[256] = {false};
    bool hw_prof_seen[ICE_RSS_PROF_ID_COUNT] = {false};
    bool any_match = false;
    uint16_t ptype;
    uint16_t matched_ptg_count = 0;
    uint16_t matched_tcam_count = 0;
    uint16_t matched_hw_prof_count = 0;

    if (active_vsig == 0) {
        fprintf(stderr, "[my_ice] RSS inspect skipped: active VSIG is default\n"); // RSS:IMPL
        return;
    }
    if (aq_read_xlt1_rss(d, ptgs) < 0)
        return;
    if (aq_read_prof_redir_rss(d, prof_redir) < 0)
        return;
    if (aq_read_prof_tcam_rss(d, tcam) < 0)
        return;
    if (aq_read_fld_vec_rss(d, fv) < 0)
        return;

    for (ptype = 0; ptype < ICE_XLT1_CNT; ptype++)
        ptg_ptype_counts[ptgs[ptype]]++;

    fprintf(stderr, "[my_ice] RSS inspect active_vsig=0x%04x begin\n", active_vsig); // RSS:IMPL
    for (ptype = 0; ptype < ICE_RSS_PROF_TCAM_COUNT; ptype++) {
        struct ice_prof_id_key decoded;
        uint8_t dc_mask[sizeof(decoded)];
        uint8_t nm_mask[sizeof(decoded)];
        uint8_t ptg;
        uint8_t sw_prof;
        uint8_t hw_prof;
        uint16_t vsig;
        uint16_t flags;
        uint16_t flags_mask;

        if (!rss_decode_tcam_entry(&tcam[ptype], &decoded, dc_mask, nm_mask))
            continue;
        if (rss_mask_any_set(nm_mask, sizeof(nm_mask)))
            continue;

        vsig = le16toh(decoded.xlt2_cdid);
        if (vsig != active_vsig)
            continue;

        any_match = true;
        matched_tcam_count++;
        ptg = decoded.xlt1;
        sw_prof = tcam[ptype].prof_id;
        hw_prof = sw_prof < ICE_RSS_PROF_ID_COUNT ? prof_redir[sw_prof] : sw_prof;
        flags = le16toh(decoded.flags);
        memcpy(&flags_mask, dc_mask, sizeof(flags_mask));
        flags_mask = (uint16_t)~le16toh(flags_mask);

        if (!matched_ptg_seen[ptg]) {
            matched_ptg_seen[ptg] = true;
            matched_ptg_count++;
        }

        fprintf(stderr,
                "[my_ice] RSS active_tcam idx=%u ptg=%u ptypes=%u sw_prof=%u hw_prof=%u flags=0x%04x flags_mask=0x%04x\n",
                ptype, ptg, ptg_ptype_counts[ptg], sw_prof, hw_prof, flags, flags_mask); // RSS:IMPL
        if (hw_prof >= ICE_RSS_PROF_ID_COUNT || hw_prof_seen[hw_prof])
            continue;
        hw_prof_seen[hw_prof] = true;
        matched_hw_prof_count++;
        rss_log_profile_fv(hw_prof, fv[hw_prof]);
    }

    if (!any_match) {
        fprintf(stderr,
                "[my_ice] RSS inspect active_vsig=0x%04x no PROFID_TCAM matches found\n",
                active_vsig); // RSS:IMPL
        return;
    }

    fprintf(stderr,
            "[my_ice] RSS inspect active_vsig=0x%04x matched_tcam=%u matched_ptgs=%u matched_hw_profiles=%u\n",
            active_vsig, matched_tcam_count, matched_ptg_count, matched_hw_prof_count); // RSS:IMPL
}

static int aq_acquire_change_lock(struct dev_ctx *d)
{
    struct ice_aq_desc desc;
    struct ice_aqc_req_res *cmd;
    int i;

    for (i = 0; i < 100; i++) {
        fill_dflt_direct_desc(&desc, ICE_AQC_OPC_REQUEST_RES);
        cmd = &desc.params.req_res;
        cmd->res_id = htole16(ICE_AQC_RES_ID_CHNG_LOCK);
        cmd->access_type = htole16(ICE_AQC_RES_ACCESS_WRITE);
        cmd->timeout = htole32(ICE_CHANGE_LOCK_TIMEOUT_MS);
        cmd->res_number = 0;
        cmd->status = 0;

        if (aq_send_cmd(d, &desc, NULL, 0) == 0) {
            fprintf(stderr, "[my_ice] RSS acquired change lock after %d tries\n", i + 1); // RSS:IMPL
            return 0;
        }
        if (le16toh(desc.retval) != ICE_AQ_RC_EBUSY)
            break;
        fprintf(stderr, "[my_ice] RSS change lock busy try=%d retval=0x%04x\n",
                i + 1, le16toh(desc.retval)); // RSS:IMPL
        usleep(10000);
    }

    fprintf(stderr, "[my_ice] RSS failed to acquire change lock\n"); // RSS:IMPL
    return -1;
}

static void aq_release_change_lock(struct dev_ctx *d)
{
    struct ice_aq_desc desc;
    struct ice_aqc_req_res *cmd;

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_RELEASE_RES);
    cmd = &desc.params.req_res;
    cmd->res_id = htole16(ICE_AQC_RES_ID_CHNG_LOCK);
    cmd->res_number = 0;
    if (aq_send_cmd(d, &desc, NULL, 0) < 0)
        fprintf(stderr, "[my_ice] RSS failed to release change lock\n"); // RSS:IMPL
    else
        fprintf(stderr, "[my_ice] RSS released change lock\n"); // RSS:IMPL
}

static int aq_write_xlt2_rss(struct dev_ctx *d, uint16_t vsi, uint16_t target_vsig)
{
    uint8_t buf[18];
    struct ice_aq_desc desc;
    struct ice_aqc_download_pkg *cmd;
    uint16_t section_count = htole16(1);
    uint16_t data_end = htole16(18);
    uint32_t section_type = htole32(ICE_SID_XLT2_RSS);
    uint16_t section_off = htole16(12);
    uint16_t section_size = htole16(6);
    uint16_t xlt2_count = htole16(1);
    uint16_t xlt2_offset = htole16(vsi);
    uint16_t xlt2_value = htole16(target_vsig);

    memset(buf, 0, sizeof(buf));
    memcpy(buf + 0, &section_count, sizeof(section_count));
    memcpy(buf + 2, &data_end, sizeof(data_end));
    memcpy(buf + 4, &section_type, sizeof(section_type));
    memcpy(buf + 8, &section_off, sizeof(section_off));
    memcpy(buf + 10, &section_size, sizeof(section_size));
    memcpy(buf + 12, &xlt2_count, sizeof(xlt2_count));
    memcpy(buf + 14, &xlt2_offset, sizeof(xlt2_offset));
    memcpy(buf + 16, &xlt2_value, sizeof(xlt2_value));

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_UPDATE_PKG);
    desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);
    cmd = &desc.params.download_pkg;
    cmd->flags = ICE_AQC_DOWNLOAD_PKG_LAST_BUF;

    fprintf(stderr, "[my_ice] RSS write XLT2 vsi=%u target_vsig=0x%04x\n",
            vsi, target_vsig); // RSS:IMPL
    return aq_send_cmd(d, &desc, buf, sizeof(buf));
}

static int rss_select_target_vsig(struct dev_ctx *d, const uint16_t vsigs[ICE_XLT2_CNT],
                                  uint16_t *target_vsig)
{
    struct rss_vsig_candidate {
        uint16_t vsig;
        uint16_t total_count;
        uint16_t other_count;
        uint8_t pf_num;
        bool is_current;
    } candidates[ICE_XLT2_CNT];
    uint16_t unique_vsigs[ICE_XLT2_CNT];
    uint16_t counts[ICE_XLT2_CNT];
    uint16_t unique_count = 0;
    uint16_t cand_count = 0;
    uint16_t i;

    d->io.rss_current_vsig = vsigs[d->io.vsi_num];
    for (i = 0; i < ICE_XLT2_CNT; i++) {
        uint16_t vsig = vsigs[i];
        uint16_t j;

        if (vsig == 0)
            continue;
        for (j = 0; j < unique_count; j++) {
            if (unique_vsigs[j] == vsig) {
                counts[j]++;
                break;
            }
        }
        if (j == unique_count) {
            unique_vsigs[unique_count] = vsig;
            counts[unique_count] = 1;
            unique_count++;
        }
    }

    for (i = 0; i < unique_count; i++) {
        uint16_t vsig = unique_vsigs[i];
        uint16_t other = counts[i];
        uint8_t pf_num = (uint8_t)((vsig >> 13) & 0x7);
        bool is_current = (vsig == d->io.rss_current_vsig);
        if (is_current && other > 0)
            other--;
        if (other == 0)
            continue;

        candidates[cand_count].vsig = vsig;
        candidates[cand_count].total_count = counts[i];
        candidates[cand_count].other_count = other;
        candidates[cand_count].pf_num = pf_num;
        candidates[cand_count].is_current = is_current;
        cand_count++;

        fprintf(stderr,
                "[my_ice] RSS candidate vsig=0x%04x pf=%u total=%u other=%u current=%u\n",
                vsig, pf_num, counts[i], other, is_current ? 1 : 0); // RSS:IMPL
    }

    for (i = 0; i < cand_count; i++) {
        uint16_t j;
        for (j = (uint16_t)(i + 1); j < cand_count; j++) {
            bool swap = false;
            bool i_pf = candidates[i].pf_num != 0;
            bool j_pf = candidates[j].pf_num != 0;

            if (j_pf && !i_pf)
                swap = true;
            else if (j_pf == i_pf && candidates[j].other_count > candidates[i].other_count)
                swap = true;
            else if (j_pf == i_pf && candidates[j].other_count == candidates[i].other_count &&
                     candidates[i].is_current && !candidates[j].is_current)
                swap = true;
            else if (j_pf == i_pf && candidates[j].other_count == candidates[i].other_count &&
                     candidates[i].is_current == candidates[j].is_current &&
                     candidates[j].vsig < candidates[i].vsig)
                swap = true;

            if (swap) {
                struct rss_vsig_candidate tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }

    if (cand_count == 0)
        return -1;

    *target_vsig = candidates[0].vsig;
    d->io.rss_target_vsig = *target_vsig;
    fprintf(stderr,
            "[my_ice] RSS selected target vsig=0x%04x current=0x%04x pf=%u other=%u\n",
            candidates[0].vsig, d->io.rss_current_vsig, candidates[0].pf_num,
            candidates[0].other_count); // RSS:IMPL
    return 0;
}

static int aq_associate_vsi_with_rss_profiles(struct dev_ctx *d)
{
    uint16_t vsigs[ICE_XLT2_CNT];
    uint16_t target_vsig;
    int rc = -1;

    if (aq_read_xlt2_rss(d, vsigs) < 0)
        return -1;
    if (rss_select_target_vsig(d, vsigs, &target_vsig) < 0) {
        fprintf(stderr,
                "[my_ice] RSS no reusable VSIG found for VSI=%u; continuing without XLT2 reassociation\n",
                d->io.vsi_num); // RSS:IMPL
        return 0;
    }
    if (d->io.rss_current_vsig == target_vsig) {
        fprintf(stderr,
                "[my_ice] RSS VSI=%u already in target VSIG=0x%04x\n",
                d->io.vsi_num, target_vsig); // RSS:IMPL
        return 0;
    }

    if (aq_acquire_change_lock(d) < 0)
        return -1;

    rc = aq_write_xlt2_rss(d, d->io.vsi_num, target_vsig);
    aq_release_change_lock(d);
    if (rc < 0)
        return -1;

    if (aq_read_xlt2_rss(d, vsigs) == 0) {
        fprintf(stderr,
                "[my_ice] RSS verify XLT2 vsi=%u vsig=0x%04x\n",
                d->io.vsi_num, vsigs[d->io.vsi_num]); // RSS:IMPL
    }

    return 0;
}

static int setup_rss_scaling(struct dev_ctx *d)
{
    uint8_t vsi_lut[ICE_AQC_RSS_VSI_LUT_SIZE];
    uint8_t pf_lut[ICE_AQC_RSS_PF_LUT_SIZE];
    struct ice_aqc_vsi_props props;

    if (d->io.rxq_count <= 1)
        return 0;

    fprintf(stderr, "[my_ice] RSS setup start vsi=%u rxqs=%u first_rxq=%u\n",
            d->io.vsi_num, d->io.rxq_count, d->io.rxqs[0].rxq_id); // RSS:IMPL
    if (aq_set_rss_key(d) < 0)
        return -1;

    memset(vsi_lut, 0, sizeof(vsi_lut));
    memset(pf_lut, 0, sizeof(pf_lut));
    (void)aq_get_set_rss_lut(d, ICE_AQC_RSS_LUT_TYPE_VSI | ICE_AQC_RSS_LUT_SIZE_SMALL,
                             vsi_lut, sizeof(vsi_lut), false);
    (void)aq_get_set_rss_lut(d, ICE_AQC_RSS_LUT_TYPE_PF | ICE_AQC_RSS_LUT_SIZE_2K,
                             pf_lut, sizeof(pf_lut), false);

    rss_fill_round_robin_lut(vsi_lut, sizeof(vsi_lut), d->io.rxq_count);
    rss_fill_round_robin_lut(pf_lut, sizeof(pf_lut), d->io.rxq_count);
    rss_log_lut_summary("RSS VSI LUT", vsi_lut, sizeof(vsi_lut), d->io.rxq_count); // RSS:IMPL
    rss_log_lut_summary("RSS PF LUT", pf_lut, sizeof(pf_lut), d->io.rxq_count); // RSS:IMPL

    if (aq_get_set_rss_lut(d, ICE_AQC_RSS_LUT_TYPE_VSI | ICE_AQC_RSS_LUT_SIZE_SMALL,
                           vsi_lut, sizeof(vsi_lut), true) < 0)
        return -1;
    if (aq_get_set_rss_lut(d, ICE_AQC_RSS_LUT_TYPE_PF | ICE_AQC_RSS_LUT_SIZE_2K,
                           pf_lut, sizeof(pf_lut), true) < 0)
        return -1;
    if (aq_get_set_rss_lut(d, ICE_AQC_RSS_LUT_TYPE_PF | ICE_AQC_RSS_LUT_SIZE_2K,
                           pf_lut, sizeof(pf_lut), false) == 0)
        rss_log_lut_summary("RSS PF LUT readback", pf_lut, sizeof(pf_lut), d->io.rxq_count); // RSS:IMPL

    (void)aq_get_vsi_params(d, &props);
    if (aq_update_vsi_rss(d) < 0)
        return -1;
    (void)aq_get_vsi_params(d, &props);
    (void)aq_get_pkg_info_list(d);
    if (aq_associate_vsi_with_rss_profiles(d) < 0)
        return -1;
    rss_inspect_active_vsig(d, d->io.rss_target_vsig ? d->io.rss_target_vsig : d->io.rss_current_vsig);
    return 0;
}

static int wait_rxq_ready(struct dev_ctx *d, uint16_t rxq, uint32_t *reg)
{
    int i;

    for (i = 0; i < 2000; i++) {
        uint32_t qrx_ctrl = reg_read32(d, QRX_CTRL(rxq));
        uint32_t qena_req = qrx_ctrl & QRX_CTRL_QENA_REQ_M;
        uint32_t qena_stat = qrx_ctrl & QRX_CTRL_QENA_STAT_M;
        if (!!qena_req == !!qena_stat) {
            *reg = qrx_ctrl;
            return 0;
        }
        usleep(10);
    }

    return -1;
}

static int enable_rxq(struct dev_ctx *d, struct rxq_ctx *rxq)
{
    struct ice_rlan_ctx rlan = {0};
    uint8_t ctx_buf[ICE_RXQ_CTX_SZ] = {0};
    uint16_t q = rxq->rxq_id;
    uint32_t reg;
    int i;

    rlan.base = rxq->rx_desc_iova >> 7;
    rlan.qlen = ICE_RX_DESC_COUNT;
    rlan.dbuf = ICE_RX_BUF_SIZE >> ICE_RLAN_CTX_DBUF_S;
    rlan.dsize = 1;
    rlan.crcstrip = 1;
    rlan.l2tsel = 1;
    rlan.dtype = ICE_RX_DTYPE_NO_SPLIT;
    rlan.hsplit_0 = 0;
    rlan.hsplit_1 = 0;
    rlan.showiv = 1;
    rlan.rxmax = ICE_RX_BUF_SIZE;
    rlan.lrxqthresh = 1;
    rlan.prefena = 1;

    reg = reg_read32(d, QRXFLXP_CNTXT(q));
    reg &= ~QRXFLXP_CNTXT_RXDID_IDX_M;
    reg |= ((uint32_t)ICE_RXDID_FLEX_NIC << QRXFLXP_CNTXT_RXDID_IDX_S) & QRXFLXP_CNTXT_RXDID_IDX_M;
    reg &= ~QRXFLXP_CNTXT_RXDID_PRIO_M;
    reg |= (0x03U << QRXFLXP_CNTXT_RXDID_PRIO_S) & QRXFLXP_CNTXT_RXDID_PRIO_M;
    reg_write32(d, QRXFLXP_CNTXT(q), reg);

    set_ctx_bits((uint8_t *)&rlan, ctx_buf, rlan_ctx_info);
    for (i = 0; i < ICE_RXQ_CTX_SIZE_DWORDS; i++) {
        uint32_t v;
        memcpy(&v, ctx_buf + i * sizeof(uint32_t), sizeof(uint32_t));
        reg_write32(d, QRX_CONTEXT(i, q), v);
    }

    if (wait_rxq_ready(d, q, &reg) < 0)
        return -1;

    if (!(reg & QRX_CTRL_QENA_STAT_M)) {
        reg |= QRX_CTRL_QENA_REQ_M;
        reg_write32(d, QRX_CTRL(q), reg);
        if (wait_rxq_ready(d, q, &reg) < 0)
            return -1;
        if (!(reg & QRX_CTRL_QENA_STAT_M))
            return -1;
    }

    rxq->rx_ntc = 0;
    reg_write32(d, QRX_TAIL(q), ICE_RX_DESC_COUNT - 1);
    return 0;
}

static int setup_and_enable_rxq(struct dev_ctx *d, struct rxq_ctx *rxq)
{
    int i;

    memset(rxq->rx_desc, 0, ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc));
    memset(rxq->rx_bufs, 0, ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE);

    for (i = 0; i < ICE_RX_DESC_COUNT; i++) {
        uint64_t biova = rxq->rx_bufs_iova + (uint64_t)i * ICE_RX_BUF_SIZE;
        rxq->rx_desc[i].read.pkt_addr = htole64(biova);
        rxq->rx_desc[i].read.hdr_addr = 0;
    }

    return enable_rxq(d, rxq);
}

static int setup_and_enable_rxq_pool(struct dev_ctx *d, struct rxq_ctx *rxq,
                                     struct pkt_mempool *pool)
{
    int i;

    memset(rxq->rx_desc, 0, ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc));
    memset(rxq->rx_pkt_bufs, 0, ICE_RX_DESC_COUNT * sizeof(*rxq->rx_pkt_bufs));

    for (i = 0; i < ICE_RX_DESC_COUNT; i++) {
        struct pkt_buf *buf = pkt_buf_alloc(pool);

        if (!buf)
            return -1;
        rxq->rx_pkt_bufs[i] = buf;
        rxq->rx_desc[i].read.pkt_addr = htole64(buf->buf_addr_iova);
        rxq->rx_desc[i].read.hdr_addr = 0;
    }

    return enable_rxq(d, rxq);
}

static void tx_ring_init(struct dev_ctx *d)
{
    uint16_t i;

    for (i = 0; i < d->txq_count; i++) {
        struct txq_ctx *q = &d->txqs[i];
        memset(q->tx_desc, 0, (size_t)q->desc_count * sizeof(struct ice_tx_desc));
        memset(q->tx_pkt_buf_refs, 0, (size_t)q->desc_count * sizeof(*q->tx_pkt_buf_refs));
        q->tx_next_to_use = 0;
        q->tx_next_to_clean = 0;
        q->tx_free = (uint16_t)(q->desc_count - 1);
        q->tx_pkts_since_rs = 0;
    }
}

static void tx_update_free(struct dev_ctx *d, struct txq_ctx *q)
{
    uint16_t head = (uint16_t)(reg_read32(d, QTX_COMM_HEAD(q->txq_id)) & 0x1FFFU);
    uint16_t ntu = q->tx_next_to_use;
    uint16_t used;
    uint16_t idx;

    if (head >= q->desc_count)
        head = (uint16_t)(head % q->desc_count);

    idx = q->tx_next_to_clean;
    while (idx != head) {
        if (q->tx_pkt_buf_refs[idx]) {
            pkt_buf_free(q->tx_pkt_buf_refs[idx]);
            q->tx_pkt_buf_refs[idx] = NULL;
        }
        idx = (uint16_t)((idx + 1) % q->desc_count);
    }
    q->tx_next_to_clean = head;

    if (ntu >= head)
        used = (uint16_t)(ntu - head);
    else
        used = (uint16_t)(q->desc_count - (head - ntu));

    q->tx_free = (uint16_t)(q->desc_count - used - 1);
}

static int poll_rx_batch(struct dev_ctx *d, struct rxq_ctx *rxq,
                         uint16_t out_idxs[], uint16_t out_lens[],
                            uint16_t max_count)
{
    uint16_t idx;
    uint16_t count = 0;

    (void)d;

    if (max_count == 0)
        return 0;

    idx = rxq->rx_ntc;
    while (count < max_count) {
        union ice_32b_rx_flex_desc *rxd = &rxq->rx_desc[idx];
        uint16_t status0 = le16toh(rxd->wb.status_error0);
        uint16_t pkt_len;

        if (!(status0 & BIT(ICE_RX_FLEX_DESC_STATUS0_DD_S)))
            break;

        pkt_len = (uint16_t)(le16toh(rxd->wb.pkt_len) & ICE_RX_FLX_DESC_PKT_LEN_M);
        if (!(status0 & BIT(ICE_RX_FLEX_DESC_STATUS0_EOF_S)) ||
            (status0 & BIT(ICE_RX_FLEX_DESC_STATUS0_RXE_S)) ||
            pkt_len == 0) {
            fprintf(stderr,
                    "[my_ice] rx descriptor error idx=%u status0=0x%04x pkt_len=%u\n",
                    idx, status0, pkt_len);
            return -1;
        }

        if (!rxq->rss_sampled) {
            uint32_t rss_hash = le32toh(rxd->wb.rss_hash);
            uint16_t rss_valid = (uint16_t)((status0 >> ICE_RX_FLEX_DESC_STATUS0_RSS_VALID_S) & 0x1U);
            rxq->rss_sampled = true;
            fprintf(stderr,
                    "[my_ice] RSS rx sample q=%u desc=%u rss_hash=0x%08x rss_valid=%u status0=0x%04x len=%u\n",
                    rxq->rxq_id, idx, rss_hash, rss_valid, status0, pkt_len); // RSS:IMPL
        }

        out_idxs[count] = idx;
        out_lens[count] = pkt_len;
        count++;
        idx = (uint16_t)((idx + 1) % ICE_RX_DESC_COUNT);
    }

    return count;
}

static int poll_one_rx_desc(struct dev_ctx *d, uint16_t *out_idx, uint16_t *out_len)
{
    uint16_t idx;
    uint16_t len;
    int count;

    count = poll_rx_batch(d, &d->io.rxqs[0], &idx, &len, 1);
    if (count <= 0)
        return count;

    *out_idx = idx;
    *out_len = len;
    return 1;
}

static int tx_try_reserve_slot(struct dev_ctx *d, struct txq_ctx *q, uint16_t *out_idx)
{
    if (q->tx_free == 0) {
        tx_update_free(d, q);
        if (q->tx_free == 0)
            return 1;
    }

    *out_idx = q->tx_next_to_use;
    return 0;
}

static void tx_commit_slot(struct txq_ctx *q, uint16_t idx, struct pkt_buf *buf_ref)
{
    q->tx_pkt_buf_refs[idx] = buf_ref;
    __sync_synchronize();
    q->tx_next_to_use = (uint16_t)((idx + 1) % q->desc_count);
    q->tx_free--;
}

static void tx_prepare_desc(struct txq_ctx *q, uint16_t idx, uint64_t buf_iova, uint16_t len)
{
    struct ice_tx_desc *txd = &q->tx_desc[idx];
    uint16_t cmd = ICE_TX_DESC_CMD_EOP;
    uint64_t qw1;

    q->tx_pkts_since_rs++;
    if (q->tx_pkts_since_rs >= TX_RS_THRESH) {
        cmd |= ICE_TX_DESC_CMD_RS;
        q->tx_pkts_since_rs = 0;
    }

    txd->buf_addr = htole64(buf_iova);
    qw1 = ((uint64_t)ICE_TX_DESC_DTYPE_DATA << ICE_TXD_QW1_DTYPE_S) |
          ((uint64_t)cmd << ICE_TXD_QW1_CMD_S) |
          ((uint64_t)len << ICE_TXD_QW1_TX_BUF_SZ_S);
    txd->cmd_type_offset_bsz = htole64(qw1);
}

/* Returns: 0=enqueued, 1=ring full, -1=invalid packet */
static int tx_try_enqueue(struct dev_ctx *d, struct txq_ctx *q, const uint8_t *pkt,
                          uint16_t len, bool copy, const uint8_t *dst_mac,
                          const uint8_t *src_mac)
{
    uint16_t idx;
    uint8_t *buf;
    int reserve_rc;

    if (len > ICE_TX_PKT_BUF_SIZE)
        return -1;

    reserve_rc = tx_try_reserve_slot(d, q, &idx);
    if (reserve_rc != 0)
        return reserve_rc;

    buf = q->tx_pkt_bufs + ((size_t)idx * ICE_TX_PKT_BUF_SIZE);
    if (copy && pkt && len)
        memcpy(buf, pkt, len);
    if ((dst_mac || src_mac) && len < 2 * ETHER_ADDR_LEN)
        return -1;
    if (dst_mac)
        memcpy(buf, dst_mac, ETHER_ADDR_LEN);
    if (src_mac)
        memcpy(buf + ETHER_ADDR_LEN, src_mac, ETHER_ADDR_LEN);

    tx_prepare_desc(q, idx, q->tx_pkt_iova + ((uint64_t)idx * ICE_TX_PKT_BUF_SIZE), len);
    tx_commit_slot(q, idx, NULL);
    return 0;
}

static uint16_t tx_try_enqueue_pkt_buf_batch(struct dev_ctx *d, struct txq_ctx *q,
                                             struct pkt_buf *bufs[], uint16_t num_bufs)
{
    uint16_t sent = 0;

    if (!q || !bufs || num_bufs == 0)
        return 0;

    if (q->tx_free < num_bufs)
        tx_update_free(d, q);

    while (sent < num_bufs && q->tx_free != 0) {
        uint16_t idx = q->tx_next_to_use;
        struct pkt_buf *buf = bufs[sent];

        if (!buf || buf->size == 0 || buf->size > ICE_PKT_BUF_DATA_SIZE)
            break;

        tx_prepare_desc(q, idx, buf->buf_addr_iova, (uint16_t)buf->size);
        tx_commit_slot(q, idx, buf);
        sent++;
    }

    return sent;
}

static void rearm_rx_desc_batch(struct dev_ctx *d, struct rxq_ctx *rxq,
                                const uint16_t idxs[], uint16_t count)
{
    uint16_t i;

    if (!count)
        return;

    for (i = 0; i < count; i++) {
        uint16_t idx = idxs[i];
        uint64_t buf_iova;

        if (rxq->rx_pkt_bufs && rxq->rx_pkt_bufs[idx]) {
            rxq->rx_pkt_bufs[idx]->size = 0;
            buf_iova = rxq->rx_pkt_bufs[idx]->buf_addr_iova;
        } else {
            buf_iova = rxq->rx_bufs_iova + (uint64_t)idx * ICE_RX_BUF_SIZE;
        }

        rxq->rx_desc[idx].read.pkt_addr = htole64(buf_iova);
        rxq->rx_desc[idx].read.hdr_addr = 0;
        rxq->rx_desc[idx].read.rsvd1 = 0;
        rxq->rx_desc[idx].read.rsvd2 = 0;
    }

    reg_write32(d, QRX_TAIL(rxq->rxq_id), idxs[count - 1]);
    rxq->rx_ntc = (uint16_t)((idxs[count - 1] + 1) % ICE_RX_DESC_COUNT);
}

static void rearm_rx_desc(struct dev_ctx *d, uint16_t idx)
{
    uint16_t one = idx;

    rearm_rx_desc_batch(d, &d->io.rxqs[0], &one, 1);
}

static int poll_one_rx_packet(struct dev_ctx *d, uint8_t *out, uint16_t out_sz, uint16_t *out_len)
{
    uint16_t idx;
    int got = poll_one_rx_desc(d, &idx, out_len);

    if (got <= 0)
        return got;

    if (*out_len > out_sz)
        *out_len = out_sz;

    memcpy(out, d->io.rxqs[0].rx_bufs + ((size_t)idx * ICE_RX_BUF_SIZE), *out_len);
    rearm_rx_desc(d, idx);
    return 1;
}

static inline void tx_ring_doorbell(struct dev_ctx *d, struct txq_ctx *q)
{
    reg_write32(d, QTX_COMM_DBELL(q->txq_id), q->tx_next_to_use);
}

static int tx_wait_drain(struct dev_ctx *d, struct txq_ctx *q, int timeout_ms)
{
    uint64_t start_ns = monotonic_ns();
    uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ULL;

    while (monotonic_ns() - start_ns < timeout_ns) {
        tx_update_free(d, q);
        if (q->tx_next_to_clean == q->tx_next_to_use)
            return 0;
        usleep(50);
    }

    return -1;
}

static void dump_mdet_regs(struct dev_ctx *d)
{
    uint32_t gl_tx_tclan = reg_read32(d, GL_MDET_TX_TCLAN);
    uint32_t gl_tx_pqm = reg_read32(d, GL_MDET_TX_PQM);
    uint32_t gl_rx = reg_read32(d, GL_MDET_RX);
    uint32_t pf_tx_tclan = reg_read32(d, PF_MDET_TX_TCLAN);
    uint32_t pf_tx_pqm = reg_read32(d, PF_MDET_TX_PQM);
    uint32_t pf_rx = reg_read32(d, PF_MDET_RX);

    if (gl_tx_tclan & GL_MDET_TX_TCLAN_VALID_M)
        fprintf(stderr, "[my_ice] GL_MDET_TX_TCLAN=0x%08x\n", gl_tx_tclan);
    if (gl_tx_pqm & GL_MDET_TX_PQM_VALID_M)
        fprintf(stderr, "[my_ice] GL_MDET_TX_PQM=0x%08x\n", gl_tx_pqm);
    if (gl_rx & GL_MDET_RX_VALID_M)
        fprintf(stderr, "[my_ice] GL_MDET_RX=0x%08x\n", gl_rx);
    if (pf_tx_tclan & PF_MDET_TX_TCLAN_VALID_M)
        fprintf(stderr, "[my_ice] PF_MDET_TX_TCLAN=0x%08x\n", pf_tx_tclan);
    if (pf_tx_pqm & PF_MDET_TX_PQM_VALID_M)
        fprintf(stderr, "[my_ice] PF_MDET_TX_PQM=0x%08x\n", pf_tx_pqm);
    if (pf_rx & PF_MDET_RX_VALID_M)
        fprintf(stderr, "[my_ice] PF_MDET_RX=0x%08x\n", pf_rx);
}

static void dump_rx_desc_snapshot(struct dev_ctx *d)
{
    uint16_t i;
    struct rxq_ctx *rxq = &d->io.rxqs[0];

    for (i = 0; i < 8; i++) {
        uint16_t idx = (uint16_t)((rxq->rx_ntc + i) % ICE_RX_DESC_COUNT);
        union ice_32b_rx_flex_desc *rxd = &rxq->rx_desc[idx];
        uint16_t status0 = le16toh(rxd->wb.status_error0);
        uint16_t pkt_len = (uint16_t)(le16toh(rxd->wb.pkt_len) & ICE_RX_FLX_DESC_PKT_LEN_M);
        fprintf(stderr,
                "[my_ice] rxdesc[%u] rxdid=%u status0=0x%04x pkt_len=%u\n",
                idx, rxd->wb.rxdid, status0, pkt_len);
    }
}

static uint64_t read_glv_counter64(struct dev_ctx *d, uint32_t lo_off, uint32_t hi_off)
{
    uint32_t lo_old;
    uint32_t hi;
    uint32_t lo;

    do {
        lo_old = reg_read32(d, lo_off);
        hi = reg_read32(d, hi_off);
        lo = reg_read32(d, lo_off);
    } while (lo_old > lo);

    return ((uint64_t)lo) | (((uint64_t)hi & 0xFFU) << 32);
}

static uint64_t counter40_delta(uint64_t after, uint64_t before)
{
    const uint64_t mask = (1ULL << 40) - 1;

    after &= mask;
    before &= mask;
    if (after >= before)
        return after - before;
    return (after + (1ULL << 40)) - before;
}

static void print_payload_dump(const uint8_t *pkt, uint16_t len)
{
    const uint16_t l2_len = 14;
    const uint16_t dump_cap = 256;
    uint16_t payload_len;
    uint16_t n;
    uint16_t i;

    if (len <= l2_len) {
        printf("Payload: <none>\n");
        return;
    }

    payload_len = (uint16_t)(len - l2_len);
    n = payload_len > dump_cap ? dump_cap : payload_len;

    printf("Payload: %u bytes (showing %u)\n", payload_len, n);
    printf("Payload ASCII: ");
    for (i = 0; i < n; i++) {
        uint8_t c = pkt[l2_len + i];
        putchar((c >= 32 && c <= 126) ? (char)c : '.');
    }
    putchar('\n');

    printf("Payload HEX:\n");
    for (i = 0; i < n; i += 16) {
        uint16_t j;
        uint16_t line_end = (uint16_t)(i + 16);
        if (line_end > n)
            line_end = n;
        printf("  %04x: ", i);
        for (j = i; j < line_end; j++)
            printf("%02x ", pkt[l2_len + j]);
        putchar('\n');
    }
}

static void rewrite_reflect_l2(struct pkt_buf *buf, const uint8_t *local_mac)
{
    uint8_t original_src[ETHER_ADDR_LEN];

    memcpy(original_src, buf->data + ETHER_ADDR_LEN, ETHER_ADDR_LEN);
    memcpy(buf->data, original_src, ETHER_ADDR_LEN);
    memcpy(buf->data + ETHER_ADDR_LEN, local_mac, ETHER_ADDR_LEN);
}

static int run_rx_listen(struct dev_ctx *d, int timeout_ms)
{
    uint16_t first_q, last_q;
    uint8_t rxpkt[2048];
    uint16_t rxlen = 0;
    uint16_t rx_mac_rule_idx = UINT16_MAX;
    uint64_t gorc_before, gorc_after;
    int rc = -1;
    int i;

    if (get_rx_queue_block(d, &first_q, &last_q) < 0)
        goto out;
    assign_rx_queue_ids(d, first_q);

    if (aq_get_default_vsi_and_lport(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get default VSI/LPORT\n");
        goto out;
    }

    fprintf(stderr, "[my_ice] rx-listen on vsi=%u lport=%u rxq=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            d->io.vsi_num, d->io.lport, d->io.rxqs[0].rxq_id,
            d->io.mac[0], d->io.mac[1], d->io.mac[2],
            d->io.mac[3], d->io.mac[4], d->io.mac[5]);

    if (setup_and_enable_rxq(d, &d->io.rxqs[0]) < 0) {
        fprintf(stderr, "[my_ice] setup/enable rx queue failed\n");
        goto out;
    }

    if (aq_add_rx_mac_rule(d, &rx_mac_rule_idx) < 0) {
        fprintf(stderr, "[my_ice] failed to add RX MAC rule\n");
        goto out;
    }

    gorc_before = read_glv_counter64(d, GLV_GORCL(d->io.vsi_num),
                                     GLV_GORCH(d->io.vsi_num));

    for (i = 0; i < timeout_ms; i++) {
        int got = poll_one_rx_packet(d, rxpkt, sizeof(rxpkt), &rxlen);
        if (got < 0) {
            dump_mdet_regs(d);
            goto out;
        }
        if (got > 0) {
            if (rxlen >= 14) {
                printf("RXLISTEN: received %u bytes, dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x ethertype=0x%02x%02x\n",
                       rxlen,
                       rxpkt[0], rxpkt[1], rxpkt[2], rxpkt[3], rxpkt[4], rxpkt[5],
                       rxpkt[6], rxpkt[7], rxpkt[8], rxpkt[9], rxpkt[10], rxpkt[11],
                       rxpkt[12], rxpkt[13]);
            } else {
                printf("RXLISTEN: received %u bytes\n", rxlen);
            }
            print_payload_dump(rxpkt, rxlen);
            rc = 0;
            goto out;
        }
        usleep(1000);
    }

    gorc_after = read_glv_counter64(d, GLV_GORCL(d->io.vsi_num),
                                    GLV_GORCH(d->io.vsi_num));
    fprintf(stderr, "[my_ice] rx-listen timeout, VSI%u GORC +%" PRIu64 " bytes\n",
            d->io.vsi_num, counter40_delta(gorc_after, gorc_before));
    dump_rx_desc_snapshot(d);
    dump_mdet_regs(d);

out:
    aq_remove_sw_rule_best_effort(d, ICE_AQC_SW_RULES_T_LKUP_RX, rx_mac_rule_idx);
    return rc;
}

static int run_rx_reflect(struct dev_ctx *d, int timeout_ms)
{
    const uint16_t reflect_batch = RX_REFLECT_GLOBAL_BUDGET;
    struct pkt_mempool *pool = &d->reflect_pools[0];
    uint32_t tx_alloc;
    uint16_t first_rxq, last_rxq, first_txq, last_txq, avail_rxq, avail_txq;
    uint16_t rx_mac_rule_idx = UINT16_MAX;
    uint64_t rx_pkts = 0, rx_bytes = 0;
    uint64_t tx_pkts = 0, tx_bytes = 0;
    uint64_t zero_copy_pkts = 0, zero_copy_bytes = 0;
    uint64_t tx_ring_full = 0, pool_empty = 0;
    uint64_t rx_short = 0, rx_errors = 0;
    uint64_t doorbells = 0;
    uint64_t last_report_ns, next_report_ns;
    uint64_t prev_rx_pkts = 0, prev_rx_bytes = 0;
    uint64_t prev_tx_pkts = 0, prev_tx_bytes = 0;
    uint64_t gorc_before, gorc_after, gotc_before, gotc_after;
    uint64_t start_ns, end_ns, active_end_ns, now_ns;
    uint64_t rx_pkts_per_q[ICE_MAX_RX_QUEUES] = {0};
    uint64_t rx_bytes_per_q[ICE_MAX_RX_QUEUES] = {0};
    uint64_t tx_pkts_per_q[ICE_MAX_RX_QUEUES] = {0};
    uint64_t tx_bytes_per_q[ICE_MAX_RX_QUEUES] = {0};
    struct pkt_buf *tx_pending_bufs[ICE_MAX_RX_QUEUES][RX_REFLECT_GLOBAL_BUDGET];
    uint16_t tx_pending_lens[ICE_MAX_RX_QUEUES][RX_REFLECT_GLOBAL_BUDGET];
    uint16_t tx_pending_count[ICE_MAX_RX_QUEUES] = {0};
    uint64_t tx_updates = 0;
    int rc = -1;

    if (get_rx_queue_block(d, &first_rxq, &last_rxq) < 0)
        goto out;
    avail_rxq = (uint16_t)(last_rxq - first_rxq + 1);
    if (avail_rxq == 0) {
        fprintf(stderr, "no available RX queues (first=%u last=%u)\n", first_rxq, last_rxq);
        goto out;
    }
    if (d->io.rxq_count > avail_rxq) {
        fprintf(stderr, "[my_ice] requested %u rx queues, clamping to %u available\n",
                d->io.rxq_count, avail_rxq); // RSS:IMPL
        d->io.rxq_count = avail_rxq;
    }
    assign_rx_queue_ids(d, first_rxq);

    tx_alloc = reg_read32(d, PFLAN_TX_QALLOC);
    if (!(tx_alloc & PFLAN_TX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (TX=0x%08x)\n", tx_alloc);
        goto out;
    }

    first_txq = (uint16_t)(tx_alloc & PFLAN_TX_QALLOC_FIRSTQ_M);
    last_txq = (uint16_t)((tx_alloc & PFLAN_TX_QALLOC_LASTQ_M) >> PFLAN_TX_QALLOC_LASTQ_S);
    avail_txq = (uint16_t)(last_txq - first_txq + 1);
    if (avail_txq == 0) {
        fprintf(stderr, "no available TX queues (first=%u last=%u)\n", first_txq, last_txq);
        goto out;
    }
    d->txq_count = d->io.rxq_count;
    if (d->txq_count > avail_txq) {
        fprintf(stderr, "[my_ice] requested %u tx queues from rx-queues, clamping to %u available\n",
                d->txq_count, avail_txq); // RSS:IMPL
        d->txq_count = avail_txq;
        d->io.rxq_count = d->txq_count;
    }
    assign_rx_queue_ids(d, first_rxq);
    for (uint16_t i = 0; i < d->txq_count; i++)
        d->txqs[i].txq_id = (uint16_t)(first_txq + i);

    if (aq_get_default_vsi_and_lport(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get default VSI/LPORT\n");
        goto out;
    }
    if (aq_get_qparent_teid(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get parent TEID\n");
        goto out;
    }
    for (uint16_t i = 0; i < d->io.rxq_count; i++) {
        if (setup_and_enable_rxq_pool(d, &d->io.rxqs[i], &d->reflect_pools[i]) < 0) {
            fprintf(stderr, "[my_ice] setup/enable rx queue %u failed\n", i);
            goto out;
        }
    }
    if (aq_add_rx_mac_rule(d, &rx_mac_rule_idx) < 0) {
        fprintf(stderr, "[my_ice] failed to add RX MAC rule\n");
        goto out;
    }
    if (setup_rss_scaling(d) < 0) {
        fprintf(stderr, "[my_ice] RSS setup failed\n");
        goto out;
    }
    if (add_tx_queues(d, d->txq_count) < 0) {
        fprintf(stderr, "[my_ice] add tx queues failed\n");
        goto out;
    }

    tx_ring_init(d);

    fprintf(stderr,
            "[my_ice] rx-reflect on vsi=%u lport=%u rxqs=%u rxq_base=%u txqs=%u txq_base=%u local-mac=%02x:%02x:%02x:%02x:%02x:%02x timeout_ms=%d\n",
            d->io.vsi_num, d->io.lport, d->io.rxq_count, d->io.rxqs[0].rxq_id,
            d->txq_count, d->txqs[0].txq_id,
            d->io.mac[0], d->io.mac[1], d->io.mac[2],
            d->io.mac[3], d->io.mac[4], d->io.mac[5], timeout_ms);

    gorc_before = read_glv_counter64(d, GLV_GORCL(d->io.vsi_num),
                                     GLV_GORCH(d->io.vsi_num));
    gotc_before = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                     GLV_GOTCH(d->io.vsi_num));
    start_ns = monotonic_ns();
    end_ns = start_ns + (uint64_t)timeout_ms * 1000000ULL;
    active_end_ns = start_ns;
    last_report_ns = start_ns;
    next_report_ns = start_ns + NS_PER_S;

    while ((now_ns = monotonic_ns()) < end_ns) {
        uint16_t rx_idxs[RX_REFLECT_GLOBAL_BUDGET];
        uint16_t rx_lens[RX_REFLECT_GLOBAL_BUDGET];
        uint16_t rx_qids[RX_REFLECT_GLOBAL_BUDGET];
        uint16_t rearm_idxs[ICE_MAX_RX_QUEUES][RX_REFLECT_GLOBAL_BUDGET];
        uint16_t rearm_counts[ICE_MAX_RX_QUEUES] = {0};
        uint16_t tx_qids[RX_REFLECT_GLOBAL_BUDGET];
        struct pkt_buf *replacement_bufs[RX_REFLECT_GLOBAL_BUDGET];
        struct pkt_buf *tx_bufs[RX_REFLECT_GLOBAL_BUDGET];
        uint16_t tx_lens[RX_REFLECT_GLOBAL_BUDGET];
        uint64_t batch_rx_bytes = 0;
        bool rx_seen = false;
        bool stalled = false;
        uint16_t budget;
        int got;
        uint16_t replacement_needed = 0;
        uint16_t tx_count = 0;
        uint16_t replacement_count;
        uint16_t i;

        if ((tx_updates & 0x3fU) == 0) {
            for (i = 0; i < d->txq_count; i++)
                tx_update_free(d, &d->txqs[i]);
        }
        tx_updates++;

        budget = reflect_batch;
        if (pool->free_count < budget)
            budget = (uint16_t)pool->free_count;

        if (budget == 0) {
            if (pool->free_count == 0)
                pool_empty++;
            stalled = true;
            goto report_progress;
        }

        got = 0;
        for (i = 0; i < d->io.rxq_count && got < budget; i++) {
            uint16_t qidx = (uint16_t)((d->io.rxq_poll_next + i) % d->io.rxq_count);
            uint16_t remaining = (uint16_t)(budget - got);
            int one_got;

            one_got = poll_rx_batch(d, &d->io.rxqs[qidx], &rx_idxs[got], &rx_lens[got], remaining);
            if (one_got < 0) {
                rx_errors++;
                dump_mdet_regs(d);
                goto out;
            }
            for (uint16_t j = 0; j < (uint16_t)one_got; j++)
                rx_qids[got + j] = qidx;
            got += one_got;
        }
        d->io.rxq_poll_next = (uint16_t)((d->io.rxq_poll_next + 1) % d->io.rxq_count);
        if (got == 0)
            goto report_progress;

        rx_seen = true;

        for (i = 0; i < (uint16_t)got; i++) {
            uint16_t rx_idx = rx_idxs[i];
            uint16_t rxq_idx = rx_qids[i];
            struct rxq_ctx *rxq = &d->io.rxqs[rxq_idx];

            if (!rxq->rx_pkt_bufs[rx_idx]) {
                fprintf(stderr, "[my_ice] missing rx pool buffer for q=%u descriptor %u\n",
                        rxq_idx, rx_idx);
                rx_errors++;
                goto out;
            }
            if (rx_lens[i] >= 14)
                replacement_needed++;
        }

        replacement_count = (uint16_t)pkt_buf_alloc_batch(pool, replacement_bufs,
                                                          replacement_needed);
        if (replacement_count != replacement_needed) {
            fprintf(stderr,
                    "[my_ice] reflect pool underflow: needed=%u got=%u free_count=%u\n",
                    replacement_needed, replacement_count, pool->free_count);
            rx_errors++;
            goto out;
        }

        for (i = 0; i < (uint16_t)got; i++) {
            uint16_t rx_idx = rx_idxs[i];
            uint16_t rx_len = rx_lens[i];
            uint16_t rxq_idx = rx_qids[i];
            struct rxq_ctx *rxq = &d->io.rxqs[rxq_idx];
            struct pkt_buf *rx_buf = rxq->rx_pkt_bufs[rx_idx];

            rearm_idxs[rxq_idx][rearm_counts[rxq_idx]++] = rx_idx;

            if (rx_len < 14) {
                rx_short++;
                continue;
            }

            rx_buf->size = rx_len;
            rxq->rx_pkt_bufs[rx_idx] = replacement_bufs[tx_count];
            rewrite_reflect_l2(rx_buf, d->io.mac);

            tx_bufs[tx_count] = rx_buf;
            tx_lens[tx_count] = rx_len;
            tx_qids[tx_count] = rxq_idx;
            rx_pkts_per_q[rxq_idx]++;
            rx_bytes_per_q[rxq_idx] += rx_len;
            batch_rx_bytes += rx_len;
            tx_count++;
        }

        if (tx_count > 0)
            active_end_ns = now_ns;

        for (i = 0; i < d->io.rxq_count; i++) {
            if (rearm_counts[i] > 0)
                rearm_rx_desc_batch(d, &d->io.rxqs[i], rearm_idxs[i], rearm_counts[i]);
        }

        rx_pkts += tx_count;
        rx_bytes += batch_rx_bytes;

        for (i = 0; i < tx_count; i++) {
            uint16_t qidx = (uint16_t)(tx_qids[i] % d->txq_count);
            uint16_t slot = tx_pending_count[qidx]++;

            if (slot >= RX_REFLECT_GLOBAL_BUDGET) {
                pkt_buf_free(tx_bufs[i]);
                tx_ring_full++;
                stalled = true;
                tx_pending_count[qidx] = RX_REFLECT_GLOBAL_BUDGET;
                continue;
            }

            tx_pending_bufs[qidx][slot] = tx_bufs[i];
            tx_pending_lens[qidx][slot] = tx_lens[i];
        }

        for (i = 0; i < d->txq_count; i++) {
            struct txq_ctx *q = &d->txqs[i];
            uint16_t queued = tx_pending_count[i];
            uint16_t sent;

            if (queued < TX_BURST_SIZE && !stalled && now_ns + 1000000ULL < end_ns)
                continue;

            sent = tx_try_enqueue_pkt_buf_batch(d, q, tx_pending_bufs[i], queued);
            if (sent > 0) {
                uint16_t j;

                tx_ring_doorbell(d, q);
                doorbells++;
                for (j = 0; j < sent; j++) {
                    tx_pkts++;
                    tx_bytes += tx_pending_lens[i][j];
                    tx_pkts_per_q[i]++;
                    tx_bytes_per_q[i] += tx_pending_lens[i][j];
                    zero_copy_pkts++;
                    zero_copy_bytes += tx_pending_lens[i][j];
                }
            }
            if (sent < queued) {
                tx_ring_full++;
                stalled = true;
            }
            if (sent < queued) {
                uint16_t remain = (uint16_t)(queued - sent);
                memmove(tx_pending_bufs[i], &tx_pending_bufs[i][sent],
                        (size_t)remain * sizeof(tx_pending_bufs[i][0]));
                memmove(tx_pending_lens[i], &tx_pending_lens[i][sent],
                        (size_t)remain * sizeof(tx_pending_lens[i][0]));
                tx_pending_count[i] = remain;
            } else {
                tx_pending_count[i] = 0;
            }
        }

report_progress:
        now_ns = monotonic_ns();
        if (now_ns >= next_report_ns) {
            uint64_t interval_ns = now_ns - last_report_ns;
            uint64_t interval_rx_pkts = rx_pkts - prev_rx_pkts;
            uint64_t interval_rx_bytes = rx_bytes - prev_rx_bytes;
            uint64_t interval_tx_pkts = tx_pkts - prev_tx_pkts;
            uint64_t interval_tx_bytes = tx_bytes - prev_tx_bytes;

            fprintf(stderr,
                    "[my_ice] rx-reflect t=%.2fs interval: TX=%.3f wire-Gbps RX=%.3f wire-Gbps tx_mpps=%.3f rx_mpps=%.3f\n",
                    (double)(now_ns - start_ns) / (double)NS_PER_S,
                    bytes_ns_to_gbps(l2_bytes_to_wire_bytes(interval_tx_pkts, interval_tx_bytes),
                                     interval_ns),
                    bytes_ns_to_gbps(l2_bytes_to_wire_bytes(interval_rx_pkts, interval_rx_bytes),
                                     interval_ns),
                    pkts_ns_to_mpps(interval_tx_pkts, interval_ns),
                    pkts_ns_to_mpps(interval_rx_pkts, interval_ns));

            fprintf(stderr, "[my_ice] RSS queue distribution"); // RSS:IMPL
            for (i = 0; i < d->io.rxq_count; i++) {
                double rx_pkt_pct = rx_pkts ? (100.0 * (double)rx_pkts_per_q[i] / (double)rx_pkts) : 0.0;
                double tx_pkt_pct = tx_pkts ? (100.0 * (double)tx_pkts_per_q[i] / (double)tx_pkts) : 0.0;
                fprintf(stderr,
                        " rxq%u=%.2f%% txq%u=%.2f%%",
                        d->io.rxqs[i].rxq_id, rx_pkt_pct,
                        d->txqs[i].txq_id, tx_pkt_pct); // RSS:IMPL
            }
            fprintf(stderr, "\n"); // RSS:IMPL

            last_report_ns = now_ns;
            prev_rx_pkts = rx_pkts;
            prev_rx_bytes = rx_bytes;
            prev_tx_pkts = tx_pkts;
            prev_tx_bytes = tx_bytes;
            next_report_ns += NS_PER_S;
            if (next_report_ns < now_ns)
                next_report_ns = now_ns + NS_PER_S;
        }

        if (stalled) {
            for (i = 0; i < d->txq_count; i++)
                tx_update_free(d, &d->txqs[i]);
            usleep(50);
            continue;
        }
        if (!rx_seen)
            usleep(1000);
    }

    for (uint16_t i = 0; i < d->txq_count; i++) {
        while (tx_pending_count[i] > 0) {
            struct txq_ctx *q = &d->txqs[i];
            uint16_t queued = tx_pending_count[i];
            uint16_t sent;

            tx_update_free(d, q);
            sent = tx_try_enqueue_pkt_buf_batch(d, q, tx_pending_bufs[i], queued);
            if (sent == 0)
                break;
            tx_ring_doorbell(d, q);
            doorbells++;
            for (uint16_t j = 0; j < sent; j++) {
                tx_pkts++;
                tx_bytes += tx_pending_lens[i][j];
                tx_pkts_per_q[i]++;
                tx_bytes_per_q[i] += tx_pending_lens[i][j];
                zero_copy_pkts++;
                zero_copy_bytes += tx_pending_lens[i][j];
            }
            if (sent < queued) {
                uint16_t remain = (uint16_t)(queued - sent);
                memmove(tx_pending_bufs[i], &tx_pending_bufs[i][sent],
                        (size_t)remain * sizeof(tx_pending_bufs[i][0]));
                memmove(tx_pending_lens[i], &tx_pending_lens[i][sent],
                        (size_t)remain * sizeof(tx_pending_lens[i][0]));
                tx_pending_count[i] = remain;
            } else {
                tx_pending_count[i] = 0;
            }
        }
        (void)tx_wait_drain(d, &d->txqs[i], 1000);
    }
    now_ns = monotonic_ns();
    gorc_after = read_glv_counter64(d, GLV_GORCL(d->io.vsi_num),
                                    GLV_GORCH(d->io.vsi_num));
    gotc_after = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                    GLV_GOTCH(d->io.vsi_num));
    fprintf(stderr,
            "[my_ice] rx-reflect done: seconds=%.3f TX=%.3f wire-Gbps RX=%.3f wire-Gbps"
            " tx_mpps=%.3f rx_mpps=%.3f tx_l2_gbps=%.3f rx_l2_gbps=%.3f"
            " rx_pkts=%" PRIu64 " rx_bytes=%" PRIu64
            " tx_pkts=%" PRIu64 " tx_bytes=%" PRIu64
            " zero_copy_pkts=%" PRIu64 " zero_copy_bytes=%" PRIu64
            " tx_ring_full=%" PRIu64 " rx_short=%" PRIu64 " rx_errors=%" PRIu64
            " pool_empty=%" PRIu64 " doorbells=%" PRIu64
            " VSI%u GORC_delta=%" PRIu64 " GOTC_delta=%" PRIu64 "\n",
            (double)(active_end_ns - start_ns) / 1e9,
            bytes_ns_to_gbps(l2_bytes_to_wire_bytes(tx_pkts, tx_bytes), active_end_ns - start_ns),
            bytes_ns_to_gbps(l2_bytes_to_wire_bytes(rx_pkts, rx_bytes), active_end_ns - start_ns),
            pkts_ns_to_mpps(tx_pkts, active_end_ns - start_ns),
            pkts_ns_to_mpps(rx_pkts, active_end_ns - start_ns),
            bytes_ns_to_gbps(tx_bytes, active_end_ns - start_ns),
            bytes_ns_to_gbps(rx_bytes, active_end_ns - start_ns),
            rx_pkts, rx_bytes, tx_pkts, tx_bytes, zero_copy_pkts, zero_copy_bytes,
            tx_ring_full, rx_short, rx_errors, pool_empty, doorbells, d->io.vsi_num,
            counter40_delta(gorc_after, gorc_before),
            counter40_delta(gotc_after, gotc_before));
    fprintf(stderr, "[my_ice] RSS final queue stats"); // RSS:IMPL
    for (uint16_t i = 0; i < d->io.rxq_count; i++) {
        double rx_pkt_pct = rx_pkts ? (100.0 * (double)rx_pkts_per_q[i] / (double)rx_pkts) : 0.0;
        double tx_pkt_pct = tx_pkts ? (100.0 * (double)tx_pkts_per_q[i] / (double)tx_pkts) : 0.0;
        fprintf(stderr,
                " rxq%u(pkts=%" PRIu64 ",%.2f%% bytes=%" PRIu64 ") txq%u(pkts=%" PRIu64 ",%.2f%% bytes=%" PRIu64 ")",
                d->io.rxqs[i].rxq_id, rx_pkts_per_q[i], rx_pkt_pct,
                rx_bytes_per_q[i],
                d->txqs[i].txq_id, tx_pkts_per_q[i], tx_pkt_pct,
                tx_bytes_per_q[i]); // RSS:IMPL
    }
    fprintf(stderr, "\n"); // RSS:IMPL
    rc = 0;

out:
    aq_remove_sw_rule_best_effort(d, ICE_AQC_SW_RULES_T_LKUP_RX, rx_mac_rule_idx);
    return rc;
}

struct rx_reflect_counters {
    uint64_t rx_pkts;
    uint64_t rx_bytes;
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint64_t zero_copy_pkts;
    uint64_t zero_copy_bytes;
    uint64_t tx_ring_full;
    uint64_t pool_empty;
    uint64_t rx_short;
    uint64_t rx_errors;
    uint64_t doorbells;
    uint64_t active_end_ns;
};

struct rx_reflect_stats_page {
    atomic_ullong rx_pkts;
    atomic_ullong rx_bytes;
    atomic_ullong tx_pkts;
    atomic_ullong tx_bytes;
    atomic_ullong zero_copy_pkts;
    atomic_ullong zero_copy_bytes;
    atomic_ullong tx_ring_full;
    atomic_ullong pool_empty;
    atomic_ullong rx_short;
    atomic_ullong rx_errors;
    atomic_ullong doorbells;
    atomic_ullong active_end_ns;
    uint8_t pad[RX_REFLECT_STATS_PAGE_SIZE - 12 * sizeof(atomic_ullong)];
} __attribute__((aligned(RX_REFLECT_STATS_PAGE_SIZE)));

struct rx_reflect_worker {
    struct dev_ctx *d;
    struct rxq_ctx *rxq;
    struct txq_ctx *txq;
    struct pkt_mempool *pool;
    struct rx_reflect_stats_page *stats;
    uint64_t end_ns;
    int cpu_id;
    uint16_t qidx;
    int err;
};

static void rx_reflect_publish_stats(struct rx_reflect_stats_page *stats,
                                     const struct rx_reflect_counters *local)
{
    atomic_store_explicit(&stats->rx_pkts, local->rx_pkts, memory_order_relaxed);
    atomic_store_explicit(&stats->rx_bytes, local->rx_bytes, memory_order_relaxed);
    atomic_store_explicit(&stats->tx_pkts, local->tx_pkts, memory_order_relaxed);
    atomic_store_explicit(&stats->tx_bytes, local->tx_bytes, memory_order_relaxed);
    atomic_store_explicit(&stats->zero_copy_pkts, local->zero_copy_pkts, memory_order_relaxed);
    atomic_store_explicit(&stats->zero_copy_bytes, local->zero_copy_bytes, memory_order_relaxed);
    atomic_store_explicit(&stats->tx_ring_full, local->tx_ring_full, memory_order_relaxed);
    atomic_store_explicit(&stats->pool_empty, local->pool_empty, memory_order_relaxed);
    atomic_store_explicit(&stats->rx_short, local->rx_short, memory_order_relaxed);
    atomic_store_explicit(&stats->rx_errors, local->rx_errors, memory_order_relaxed);
    atomic_store_explicit(&stats->doorbells, local->doorbells, memory_order_relaxed);
    atomic_store_explicit(&stats->active_end_ns, local->active_end_ns, memory_order_relaxed);
}

static void *rx_reflect_worker_main(void *arg)
{
    struct rx_reflect_worker *w = arg;
    struct dev_ctx *d = w->d;
    struct rxq_ctx *rxq = w->rxq;
    struct txq_ctx *txq = w->txq;
    struct pkt_mempool *pool = w->pool;
    struct rx_reflect_stats_page *stats = w->stats;
    struct rx_reflect_counters local = {0};
    struct pkt_buf *tx_pending_bufs[RX_REFLECT_GLOBAL_BUDGET];
    uint16_t tx_pending_lens[RX_REFLECT_GLOBAL_BUDGET];
    uint16_t tx_pending_count = 0;
    uint64_t tx_updates = 0;
    uint64_t publish_budget = 0;

    if (w->cpu_id >= 0 && pin_thread_to_cpu(w->cpu_id) != 0) {
        fprintf(stderr, "[my_ice] failed to pin rx-reflect worker q=%u to cpu=%d\n",
                w->qidx, w->cpu_id);
        w->err = -1;
        return NULL;
    }

    while (monotonic_ns() < w->end_ns) {
        uint16_t rx_idxs[RX_REFLECT_GLOBAL_BUDGET];
        uint16_t rx_lens[RX_REFLECT_GLOBAL_BUDGET];
        uint16_t rearm_idxs[RX_REFLECT_GLOBAL_BUDGET];
        struct pkt_buf *replacement_bufs[RX_REFLECT_GLOBAL_BUDGET];
        struct pkt_buf *tx_bufs[RX_REFLECT_GLOBAL_BUDGET];
        uint16_t tx_lens[RX_REFLECT_GLOBAL_BUDGET];
        uint64_t batch_rx_bytes = 0;
        bool stalled = false;
        bool rx_seen = false;
        uint16_t budget = RX_REFLECT_GLOBAL_BUDGET;
        uint16_t replacement_needed = 0;
        uint16_t tx_count = 0;
        uint16_t got;
        uint64_t now_ns;

        if ((tx_updates & 0x3fU) == 0)
            tx_update_free(d, txq);
        tx_updates++;

        if (tx_pending_count > 0) {
            now_ns = monotonic_ns();
            {
                uint16_t sent = tx_try_enqueue_pkt_buf_batch(d, txq, tx_pending_bufs,
                                                             tx_pending_count);

                if (sent > 0) {
                    uint64_t sent_bytes = 0;

                    tx_ring_doorbell(d, txq);
                    local.doorbells++;
                    for (uint16_t i = 0; i < sent; i++)
                        sent_bytes += tx_pending_lens[i];
                    local.tx_pkts += sent;
                    local.tx_bytes += sent_bytes;
                    local.zero_copy_pkts += sent;
                    local.zero_copy_bytes += sent_bytes;
                    local.active_end_ns = now_ns;
                }
                if (sent < tx_pending_count) {
                    uint16_t remain = (uint16_t)(tx_pending_count - sent);

                    local.tx_ring_full++;
                    stalled = true;
                    memmove(tx_pending_bufs, &tx_pending_bufs[sent],
                            (size_t)remain * sizeof(tx_pending_bufs[0]));
                    memmove(tx_pending_lens, &tx_pending_lens[sent],
                            (size_t)remain * sizeof(tx_pending_lens[0]));
                    tx_pending_count = remain;
                    if ((++publish_budget & 0x3ffU) == 0)
                        rx_reflect_publish_stats(stats, &local);
                    goto worker_progress;
                }
                tx_pending_count = 0;
            }
        }

        if (pool->free_count < budget)
            budget = (uint16_t)pool->free_count;
        if (budget == 0) {
            local.pool_empty++;
            stalled = true;
            goto worker_progress;
        }

        {
            int polled = poll_rx_batch(d, rxq, rx_idxs, rx_lens, budget);

            if (polled < 0) {
                local.rx_errors++;
                rx_reflect_publish_stats(stats, &local);
                dump_mdet_regs(d);
                w->err = -1;
                return NULL;
            }
            got = (uint16_t)polled;
        }
        if (got == 0)
            goto worker_progress;

        rx_seen = true;
        for (uint16_t i = 0; i < got; i++) {
            if (!rxq->rx_pkt_bufs[rx_idxs[i]]) {
                fprintf(stderr, "[my_ice] missing rx pool buffer for q=%u descriptor %u\n",
                        w->qidx, rx_idxs[i]);
                local.rx_errors++;
                rx_reflect_publish_stats(stats, &local);
                w->err = -1;
                return NULL;
            }
            if (rx_lens[i] >= 14)
                replacement_needed++;
        }

        if (pkt_buf_alloc_batch(pool, replacement_bufs, replacement_needed) != replacement_needed) {
            fprintf(stderr,
                    "[my_ice] reflect pool underflow: q=%u needed=%u free_count=%u\n",
                    w->qidx, replacement_needed, pool->free_count);
            local.rx_errors++;
            rx_reflect_publish_stats(stats, &local);
            w->err = -1;
            return NULL;
        }

        for (uint16_t i = 0; i < got; i++) {
            uint16_t rx_idx = rx_idxs[i];
            uint16_t rx_len = rx_lens[i];
            struct pkt_buf *rx_buf = rxq->rx_pkt_bufs[rx_idx];

            rearm_idxs[i] = rx_idx;
            if (rx_len < 14) {
                local.rx_short++;
                continue;
            }

            rx_buf->size = rx_len;
            rxq->rx_pkt_bufs[rx_idx] = replacement_bufs[tx_count];
            rewrite_reflect_l2(rx_buf, d->io.mac);

            tx_bufs[tx_count] = rx_buf;
            tx_lens[tx_count] = rx_len;
            batch_rx_bytes += rx_len;
            tx_count++;
        }

        if (got > 0)
            rearm_rx_desc_batch(d, rxq, rearm_idxs, got);

        if (tx_count > 0) {
            local.rx_pkts += tx_count;
            local.rx_bytes += batch_rx_bytes;
        }

        for (uint16_t i = 0; i < tx_count; i++) {
            uint16_t slot = tx_pending_count++;

            if (slot >= RX_REFLECT_GLOBAL_BUDGET) {
                pkt_buf_free(tx_bufs[i]);
                local.tx_ring_full++;
                tx_pending_count = RX_REFLECT_GLOBAL_BUDGET;
                stalled = true;
                continue;
            }

            tx_pending_bufs[slot] = tx_bufs[i];
            tx_pending_lens[slot] = tx_lens[i];
        }

        now_ns = monotonic_ns();
        if (tx_pending_count > 0) {
            uint16_t sent = tx_try_enqueue_pkt_buf_batch(d, txq, tx_pending_bufs, tx_pending_count);

            if (sent > 0) {
                uint64_t sent_bytes = 0;

                tx_ring_doorbell(d, txq);
                local.doorbells++;
                for (uint16_t i = 0; i < sent; i++)
                    sent_bytes += tx_pending_lens[i];
                local.tx_pkts += sent;
                local.tx_bytes += sent_bytes;
                local.zero_copy_pkts += sent;
                local.zero_copy_bytes += sent_bytes;
                local.active_end_ns = now_ns;
            }
            if (sent < tx_pending_count) {
                uint16_t remain = (uint16_t)(tx_pending_count - sent);

                local.tx_ring_full++;
                stalled = true;
                memmove(tx_pending_bufs, &tx_pending_bufs[sent],
                        (size_t)remain * sizeof(tx_pending_bufs[0]));
                memmove(tx_pending_lens, &tx_pending_lens[sent],
                        (size_t)remain * sizeof(tx_pending_lens[0]));
                tx_pending_count = remain;
            } else {
                tx_pending_count = 0;
            }
        }

        if ((++publish_budget & 0x3ffU) == 0)
            rx_reflect_publish_stats(stats, &local);

worker_progress:
        if (stalled) {
            tx_update_free(d, txq);
        } else if (!rx_seen) {
            usleep(50);
        }
    }

    while (tx_pending_count > 0) {
        uint16_t sent;

        tx_update_free(d, txq);
        sent = tx_try_enqueue_pkt_buf_batch(d, txq, tx_pending_bufs, tx_pending_count);
        if (sent == 0)
            break;
        tx_ring_doorbell(d, txq);
        local.doorbells++;
        {
            uint64_t sent_bytes = 0;

            for (uint16_t i = 0; i < sent; i++)
                sent_bytes += tx_pending_lens[i];
            local.tx_pkts += sent;
            local.tx_bytes += sent_bytes;
            local.zero_copy_pkts += sent;
            local.zero_copy_bytes += sent_bytes;
        }
        if (sent < tx_pending_count) {
            uint16_t remain = (uint16_t)(tx_pending_count - sent);

            memmove(tx_pending_bufs, &tx_pending_bufs[sent],
                    (size_t)remain * sizeof(tx_pending_bufs[0]));
            memmove(tx_pending_lens, &tx_pending_lens[sent],
                    (size_t)remain * sizeof(tx_pending_lens[0]));
            tx_pending_count = remain;
        } else {
            tx_pending_count = 0;
        }
    }

    rx_reflect_publish_stats(stats, &local);
    (void)tx_wait_drain(d, txq, 1000);
    return NULL;
}

static int run_rx_reflect_mt(struct dev_ctx *d, int timeout_ms)
{
    uint32_t tx_alloc;
    uint16_t first_rxq, last_rxq, first_txq, last_txq, avail_rxq, avail_txq;
    uint16_t rx_mac_rule_idx = UINT16_MAX;
    uint64_t gorc_before, gorc_after, gotc_before, gotc_after;
    uint64_t start_ns, end_ns, last_report_ns, next_report_ns;
    uint64_t prev_rx_pkts = 0, prev_rx_bytes = 0, prev_tx_pkts = 0, prev_tx_bytes = 0;
    pthread_t threads[ICE_MAX_RX_QUEUES] = {0};
    struct rx_reflect_worker workers[ICE_MAX_RX_QUEUES] = {0};
    struct rx_reflect_stats_page *stats_pages = NULL;
    int worker_cpus[ICE_MAX_RX_QUEUES];
    int worker_cpu_count;
    int reporter_cpu = -1;
    int numa_node = -1;
    bool numa_fallback = false;
    int rc = -1;

    if (get_rx_queue_block(d, &first_rxq, &last_rxq) < 0)
        goto out;
    avail_rxq = (uint16_t)(last_rxq - first_rxq + 1);
    if (avail_rxq == 0) {
        fprintf(stderr, "no available RX queues (first=%u last=%u)\n", first_rxq, last_rxq);
        goto out;
    }
    if (d->io.rxq_count > avail_rxq) {
        fprintf(stderr, "[my_ice] requested %u rx queues, clamping to %u available\n",
                d->io.rxq_count, avail_rxq); // RSS:IMPL
        d->io.rxq_count = avail_rxq;
    }
    assign_rx_queue_ids(d, first_rxq);

    tx_alloc = reg_read32(d, PFLAN_TX_QALLOC);
    if (!(tx_alloc & PFLAN_TX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (TX=0x%08x)\n", tx_alloc);
        goto out;
    }

    first_txq = (uint16_t)(tx_alloc & PFLAN_TX_QALLOC_FIRSTQ_M);
    last_txq = (uint16_t)((tx_alloc & PFLAN_TX_QALLOC_LASTQ_M) >> PFLAN_TX_QALLOC_LASTQ_S);
    avail_txq = (uint16_t)(last_txq - first_txq + 1);
    if (avail_txq == 0) {
        fprintf(stderr, "no available TX queues (first=%u last=%u)\n", first_txq, last_txq);
        goto out;
    }
    d->txq_count = d->io.rxq_count;
    if (d->txq_count > avail_txq) {
        fprintf(stderr, "[my_ice] requested %u tx queues from rx-queues, clamping to %u available\n",
                d->txq_count, avail_txq); // RSS:IMPL
        d->txq_count = avail_txq;
        d->io.rxq_count = d->txq_count;
    }
    assign_rx_queue_ids(d, first_rxq);
    for (uint16_t i = 0; i < d->txq_count; i++)
        d->txqs[i].txq_id = (uint16_t)(first_txq + i);

    worker_cpu_count = select_reflect_worker_cpus(d, worker_cpus, d->io.rxq_count,
                                                  &numa_node, &numa_fallback);
    if (worker_cpu_count < d->io.rxq_count) {
        fprintf(stderr,
                "[my_ice] need %u distinct worker CPUs on the selected NUMA placement, found %d\n",
                d->io.rxq_count, worker_cpu_count);
        goto out;
    }

    if (posix_memalign((void **)&stats_pages, RX_REFLECT_STATS_PAGE_SIZE,
                       sizeof(*stats_pages) * ICE_MAX_RX_QUEUES) != 0) {
        fprintf(stderr, "[my_ice] failed to allocate rx-reflect stats pages\n");
        goto out;
    }
    memset(stats_pages, 0, sizeof(*stats_pages) * ICE_MAX_RX_QUEUES);

    if (worker_cpu_count > d->io.rxq_count)
        reporter_cpu = worker_cpus[d->io.rxq_count];

    if (aq_get_default_vsi_and_lport(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get default VSI/LPORT\n");
        goto out;
    }
    if (aq_get_qparent_teid(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get parent TEID\n");
        goto out;
    }
    for (uint16_t i = 0; i < d->io.rxq_count; i++) {
        if (setup_and_enable_rxq_pool(d, &d->io.rxqs[i], &d->reflect_pools[i]) < 0) {
            fprintf(stderr, "[my_ice] setup/enable rx queue %u failed\n", i);
            goto out;
        }
    }
    if (aq_add_rx_mac_rule(d, &rx_mac_rule_idx) < 0) {
        fprintf(stderr, "[my_ice] failed to add RX MAC rule\n");
        goto out;
    }
    if (setup_rss_scaling(d) < 0) {
        fprintf(stderr, "[my_ice] RSS setup failed\n");
        goto out;
    }
    if (add_tx_queues(d, d->txq_count) < 0) {
        fprintf(stderr, "[my_ice] add tx queues failed\n");
        goto out;
    }

    tx_ring_init(d);

    fprintf(stderr,
            "[my_ice] rx-reflect(mt) on vsi=%u lport=%u rxqs=%u rxq_base=%u txqs=%u txq_base=%u numa=%d%s timeout_ms=%d\n",
            d->io.vsi_num, d->io.lport, d->io.rxq_count, d->io.rxqs[0].rxq_id,
            d->txq_count, d->txqs[0].txq_id, numa_node,
            numa_fallback ? " (affinity fallback)" : "", timeout_ms);

    gorc_before = read_glv_counter64(d, GLV_GORCL(d->io.vsi_num),
                                     GLV_GORCH(d->io.vsi_num));
    gotc_before = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                     GLV_GOTCH(d->io.vsi_num));
    start_ns = monotonic_ns();
    end_ns = start_ns + (uint64_t)timeout_ms * 1000000ULL;
    last_report_ns = start_ns;
    next_report_ns = start_ns + NS_PER_S;

    if (reporter_cpu >= 0 && pin_thread_to_cpu(reporter_cpu) != 0)
        reporter_cpu = -1;

    for (uint16_t i = 0; i < d->io.rxq_count; i++) {
        workers[i].d = d;
        workers[i].rxq = &d->io.rxqs[i];
        workers[i].txq = &d->txqs[i];
        workers[i].pool = &d->reflect_pools[i];
        workers[i].stats = &stats_pages[i];
        workers[i].end_ns = end_ns;
        workers[i].cpu_id = worker_cpus[i];
        workers[i].qidx = i;
        workers[i].err = 0;

        if (pthread_create(&threads[i], NULL, rx_reflect_worker_main, &workers[i]) != 0) {
            fprintf(stderr, "[my_ice] failed to create rx-reflect worker %u\n", i);
            workers[i].err = -1;
            goto join_workers;
        }
    }

    while (true) {
        uint64_t now_ns = monotonic_ns();

        if (now_ns >= end_ns)
            break;
        if (now_ns >= next_report_ns) {
            uint64_t interval_ns = now_ns - last_report_ns;
            uint64_t rx_pkts = 0, rx_bytes = 0, tx_pkts = 0, tx_bytes = 0;

            for (uint16_t i = 0; i < d->io.rxq_count; i++) {
                rx_pkts += atomic_load_explicit(&stats_pages[i].rx_pkts, memory_order_relaxed);
                rx_bytes += atomic_load_explicit(&stats_pages[i].rx_bytes, memory_order_relaxed);
                tx_pkts += atomic_load_explicit(&stats_pages[i].tx_pkts, memory_order_relaxed);
                tx_bytes += atomic_load_explicit(&stats_pages[i].tx_bytes, memory_order_relaxed);
            }
            fprintf(stderr,
                    "[my_ice] rx-reflect t=%.2fs interval: TX=%.3f wire-Gbps RX=%.3f wire-Gbps tx_mpps=%.3f rx_mpps=%.3f\n",
                    (double)(now_ns - start_ns) / (double)NS_PER_S,
                    bytes_ns_to_gbps(l2_bytes_to_wire_bytes(tx_pkts - prev_tx_pkts,
                                                            tx_bytes - prev_tx_bytes),
                                     interval_ns),
                    bytes_ns_to_gbps(l2_bytes_to_wire_bytes(rx_pkts - prev_rx_pkts,
                                                            rx_bytes - prev_rx_bytes),
                                     interval_ns),
                    pkts_ns_to_mpps(tx_pkts - prev_tx_pkts, interval_ns),
                    pkts_ns_to_mpps(rx_pkts - prev_rx_pkts, interval_ns));

            fprintf(stderr, "[my_ice] RSS queue distribution"); // RSS:IMPL
            for (uint16_t i = 0; i < d->io.rxq_count; i++) {
                uint64_t q_rx_pkts = atomic_load_explicit(&stats_pages[i].rx_pkts, memory_order_relaxed);
                uint64_t q_tx_pkts = atomic_load_explicit(&stats_pages[i].tx_pkts, memory_order_relaxed);
                double rx_pkt_pct = rx_pkts ? (100.0 * (double)q_rx_pkts / (double)rx_pkts) : 0.0;
                double tx_pkt_pct = tx_pkts ? (100.0 * (double)q_tx_pkts / (double)tx_pkts) : 0.0;

                fprintf(stderr,
                        " rxq%u=%.2f%% txq%u=%.2f%%",
                        d->io.rxqs[i].rxq_id, rx_pkt_pct,
                        d->txqs[i].txq_id, tx_pkt_pct); // RSS:IMPL
            }
            fprintf(stderr, "\n"); // RSS:IMPL

            last_report_ns = now_ns;
            prev_rx_pkts = rx_pkts;
            prev_rx_bytes = rx_bytes;
            prev_tx_pkts = tx_pkts;
            prev_tx_bytes = tx_bytes;
            next_report_ns += NS_PER_S;
            if (next_report_ns < now_ns)
                next_report_ns = now_ns + NS_PER_S;
        }
        usleep(1000);
    }

join_workers:
    for (uint16_t i = 0; i < d->io.rxq_count; i++) {
        if (threads[i])
            pthread_join(threads[i], NULL);
    }

    for (uint16_t i = 0; i < d->io.rxq_count; i++) {
        if (workers[i].err) {
            rc = -1;
            goto out;
        }
    }

    {
        uint64_t rx_pkts = 0, rx_bytes = 0, tx_pkts = 0, tx_bytes = 0;
        uint64_t zero_copy_pkts = 0, zero_copy_bytes = 0;
        uint64_t tx_ring_full = 0, pool_empty = 0, rx_short = 0, rx_errors = 0, doorbells = 0;
        uint64_t active_end_ns = start_ns;

        for (uint16_t i = 0; i < d->io.rxq_count; i++) {
            rx_pkts += atomic_load_explicit(&stats_pages[i].rx_pkts, memory_order_relaxed);
            rx_bytes += atomic_load_explicit(&stats_pages[i].rx_bytes, memory_order_relaxed);
            tx_pkts += atomic_load_explicit(&stats_pages[i].tx_pkts, memory_order_relaxed);
            tx_bytes += atomic_load_explicit(&stats_pages[i].tx_bytes, memory_order_relaxed);
            zero_copy_pkts += atomic_load_explicit(&stats_pages[i].zero_copy_pkts, memory_order_relaxed);
            zero_copy_bytes += atomic_load_explicit(&stats_pages[i].zero_copy_bytes, memory_order_relaxed);
            tx_ring_full += atomic_load_explicit(&stats_pages[i].tx_ring_full, memory_order_relaxed);
            pool_empty += atomic_load_explicit(&stats_pages[i].pool_empty, memory_order_relaxed);
            rx_short += atomic_load_explicit(&stats_pages[i].rx_short, memory_order_relaxed);
            rx_errors += atomic_load_explicit(&stats_pages[i].rx_errors, memory_order_relaxed);
            doorbells += atomic_load_explicit(&stats_pages[i].doorbells, memory_order_relaxed);
            {
                uint64_t q_active = atomic_load_explicit(&stats_pages[i].active_end_ns, memory_order_relaxed);

                if (q_active > active_end_ns)
                    active_end_ns = q_active;
            }
        }

        gorc_after = read_glv_counter64(d, GLV_GORCL(d->io.vsi_num),
                                        GLV_GORCH(d->io.vsi_num));
        gotc_after = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                        GLV_GOTCH(d->io.vsi_num));
        fprintf(stderr,
                "[my_ice] rx-reflect done: seconds=%.3f TX=%.3f wire-Gbps RX=%.3f wire-Gbps"
                " tx_mpps=%.3f rx_mpps=%.3f tx_l2_gbps=%.3f rx_l2_gbps=%.3f"
                " rx_pkts=%" PRIu64 " rx_bytes=%" PRIu64
                " tx_pkts=%" PRIu64 " tx_bytes=%" PRIu64
                " zero_copy_pkts=%" PRIu64 " zero_copy_bytes=%" PRIu64
                " tx_ring_full=%" PRIu64 " rx_short=%" PRIu64 " rx_errors=%" PRIu64
                " pool_empty=%" PRIu64 " doorbells=%" PRIu64
                " VSI%u GORC_delta=%" PRIu64 " GOTC_delta=%" PRIu64 "\n",
                (double)(active_end_ns - start_ns) / 1e9,
                bytes_ns_to_gbps(l2_bytes_to_wire_bytes(tx_pkts, tx_bytes), active_end_ns - start_ns),
                bytes_ns_to_gbps(l2_bytes_to_wire_bytes(rx_pkts, rx_bytes), active_end_ns - start_ns),
                pkts_ns_to_mpps(tx_pkts, active_end_ns - start_ns),
                pkts_ns_to_mpps(rx_pkts, active_end_ns - start_ns),
                bytes_ns_to_gbps(tx_bytes, active_end_ns - start_ns),
                bytes_ns_to_gbps(rx_bytes, active_end_ns - start_ns),
                rx_pkts, rx_bytes, tx_pkts, tx_bytes, zero_copy_pkts, zero_copy_bytes,
                tx_ring_full, rx_short, rx_errors, pool_empty, doorbells, d->io.vsi_num,
                counter40_delta(gorc_after, gorc_before),
                counter40_delta(gotc_after, gotc_before));
        fprintf(stderr, "[my_ice] RSS final queue stats"); // RSS:IMPL
        for (uint16_t i = 0; i < d->io.rxq_count; i++) {
            uint64_t q_rx_pkts = atomic_load_explicit(&stats_pages[i].rx_pkts, memory_order_relaxed);
            uint64_t q_rx_bytes = atomic_load_explicit(&stats_pages[i].rx_bytes, memory_order_relaxed);
            uint64_t q_tx_pkts = atomic_load_explicit(&stats_pages[i].tx_pkts, memory_order_relaxed);
            uint64_t q_tx_bytes = atomic_load_explicit(&stats_pages[i].tx_bytes, memory_order_relaxed);
            double rx_pkt_pct = rx_pkts ? (100.0 * (double)q_rx_pkts / (double)rx_pkts) : 0.0;
            double tx_pkt_pct = tx_pkts ? (100.0 * (double)q_tx_pkts / (double)tx_pkts) : 0.0;

            fprintf(stderr,
                    " rxq%u(pkts=%" PRIu64 ",%.2f%% bytes=%" PRIu64 ") txq%u(pkts=%" PRIu64 ",%.2f%% bytes=%" PRIu64 ")",
                    d->io.rxqs[i].rxq_id, q_rx_pkts, rx_pkt_pct,
                    q_rx_bytes,
                    d->txqs[i].txq_id, q_tx_pkts, tx_pkt_pct,
                    q_tx_bytes); // RSS:IMPL
        }
        fprintf(stderr, "\n"); // RSS:IMPL
    }

    rc = 0;

out:
    free(stats_pages);
    aq_remove_sw_rule_best_effort(d, ICE_AQC_SW_RULES_T_LKUP_RX, rx_mac_rule_idx);
    return rc;
}

static int run_tx_send(struct dev_ctx *d, const uint8_t *dst_mac, int count,
                       int interval_ms, const char *payload)
{
    uint32_t tx_alloc;
    uint16_t first_q, last_q, avail_q;
    uint8_t pkt[ICE_TX_PKT_BUF_SIZE] = {0};
    size_t payload_len;
    uint16_t frame_len;
    uint64_t gotc_before, gotc_after;
    int i;
    struct txq_ctx *q;

    tx_alloc = reg_read32(d, PFLAN_TX_QALLOC);
    if (!(tx_alloc & PFLAN_TX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (TX=0x%08x)\n", tx_alloc);
        return -1;
    }

    first_q = (uint16_t)(tx_alloc & PFLAN_TX_QALLOC_FIRSTQ_M);
    last_q = (uint16_t)((tx_alloc & PFLAN_TX_QALLOC_LASTQ_M) >> PFLAN_TX_QALLOC_LASTQ_S);
    avail_q = (uint16_t)(last_q - first_q + 1);
    if (avail_q == 0) {
        fprintf(stderr, "no available TX queues (first=%u last=%u)\n", first_q, last_q);
        return -1;
    }
    if (d->txq_count > avail_q) {
        fprintf(stderr, "[my_ice] requested %u tx queues, clamping to %u available\n",
                d->txq_count, avail_q);
        d->txq_count = avail_q;
    }
    for (i = 0; i < d->txq_count; i++)
        d->txqs[i].txq_id = (uint16_t)(first_q + i);

    if (aq_get_default_vsi_and_lport(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get default VSI/LPORT\n");
        return -1;
    }
    if (aq_get_qparent_teid(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get parent TEID\n");
        return -1;
    }
    if (add_tx_queues(d, d->txq_count) < 0) {
        fprintf(stderr, "[my_ice] add tx queues failed\n");
        return -1;
    }

    tx_ring_init(d);
    q = &d->txqs[0];

    memcpy(pkt + 0, dst_mac, ETHER_ADDR_LEN);
    memcpy(pkt + 6, d->io.mac, ETHER_ADDR_LEN);
    pkt[12] = 0x88;
    pkt[13] = 0xB5;

    payload_len = payload ? strlen(payload) : 0;
    if (payload_len > (size_t)(ICE_TX_PKT_BUF_SIZE - 14))
        payload_len = ICE_TX_PKT_BUF_SIZE - 14;
    if (payload_len > 0)
        memcpy(pkt + 14, payload, payload_len);

    frame_len = (uint16_t)(14 + payload_len);
    if (frame_len < 60)
        frame_len = 60;

    fprintf(stderr,
            "[my_ice] tx-send on vsi=%u lport=%u txq=%u src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x count=%d interval_ms=%d payload_len=%zu frame_len=%u\n",
            d->io.vsi_num, d->io.lport, q->txq_id,
            d->io.mac[0], d->io.mac[1], d->io.mac[2],
            d->io.mac[3], d->io.mac[4], d->io.mac[5],
            dst_mac[0], dst_mac[1], dst_mac[2],
            dst_mac[3], dst_mac[4], dst_mac[5],
            count, interval_ms, payload_len, frame_len);

    gotc_before = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                     GLV_GOTCH(d->io.vsi_num));

    for (i = 0; i < count; i++) {
        int tries;
        for (tries = 0; tries < 100000; tries++) {
            if (q->tx_free == 0)
                tx_update_free(d, q);
            int enq = tx_try_enqueue(d, q, pkt, frame_len, true, NULL, NULL);
            if (enq == 0) {
                tx_ring_doorbell(d, q);
                break;
            }
            if (enq < 0) {
                fprintf(stderr, "[my_ice] tx-send invalid packet at %d/%d\n",
                        i + 1, count);
                return -1;
            }
        }
        if (tries == 100000) {
            fprintf(stderr, "[my_ice] tx-send ring full at packet %d/%d\n",
                    i + 1, count);
            return -1;
        }
        if (interval_ms > 0)
            usleep((useconds_t)interval_ms * 1000U);
    }

    (void)tx_wait_drain(d, q, 1000);

    gotc_after = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                    GLV_GOTCH(d->io.vsi_num));
    fprintf(stderr, "[my_ice] tx-send done, VSI%u GOTC +%" PRIu64 " bytes\n",
            d->io.vsi_num, counter40_delta(gotc_after, gotc_before));
    return 0;
}

struct tx_bench_worker {
    struct dev_ctx *d;
    struct txq_ctx *q;
    uint64_t end_ns;
    uint16_t frame_len;
    int cpu_id;
    uint8_t pkt[ICE_TX_PKT_BUF_SIZE];
    int err;
};

static void *tx_bench_worker_main(void *arg)
{
    struct tx_bench_worker *w = arg;

    if (w->cpu_id >= 0)
        pin_thread_to_cpu(w->cpu_id);

    while (true) {
        uint16_t burst_pkts = 0;
        int i;

        if (monotonic_ns() >= w->end_ns)
            break;

        if (w->q->tx_free < TX_BURST_SIZE)
            tx_update_free(w->d, w->q);

        for (i = 0; i < TX_BURST_SIZE; i++) {
            int enq = tx_try_enqueue(w->d, w->q, w->pkt, w->frame_len, false, NULL, NULL);
            if (enq == 0) {
                burst_pkts++;
                continue;
            }
            if (enq < 0) {
                w->err = -1;
                return NULL;
            }
            break;
        }

        if (burst_pkts > 0) {
            tx_ring_doorbell(w->d, w->q);
        } else {
            tx_update_free(w->d, w->q);
        }
    }

    return NULL;
}

static int run_tx_bench(struct dev_ctx *d, const uint8_t *dst_mac, int seconds,
                        int payload_len, bool pin_cpus)
{
    const uint64_t one_sec_ns = 1000000000ULL;
    uint32_t tx_alloc;
    uint16_t first_q, last_q, avail_q;
    uint8_t pkt[ICE_TX_PKT_BUF_SIZE] = {0};
    uint16_t frame_len;
    uint64_t gotc_start, gotc_prev, gotc_now;
    uint64_t start_ns, end_ns, now_ns, next_report_ns, last_report_ns;
    uint16_t q_idx = 0;
    pthread_t *threads = NULL;
    struct tx_bench_worker *workers = NULL;
    int cpu_list[CPU_SETSIZE];
    int cpu_count = 0;

    tx_alloc = reg_read32(d, PFLAN_TX_QALLOC);
    if (!(tx_alloc & PFLAN_TX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (TX=0x%08x)\n", tx_alloc);
        return -1;
    }

    first_q = (uint16_t)(tx_alloc & PFLAN_TX_QALLOC_FIRSTQ_M);
    last_q = (uint16_t)((tx_alloc & PFLAN_TX_QALLOC_LASTQ_M) >> PFLAN_TX_QALLOC_LASTQ_S);
    avail_q = (uint16_t)(last_q - first_q + 1);
    if (avail_q == 0) {
        fprintf(stderr, "no available TX queues (first=%u last=%u)\n", first_q, last_q);
        return -1;
    }
    if (d->txq_count > avail_q) {
        fprintf(stderr, "[my_ice] requested %u tx queues, clamping to %u available\n",
                d->txq_count, avail_q);
        d->txq_count = avail_q;
    }
    for (q_idx = 0; q_idx < d->txq_count; q_idx++)
        d->txqs[q_idx].txq_id = (uint16_t)(first_q + q_idx);
    q_idx = 0;

    if (aq_get_default_vsi_and_lport(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get default VSI/LPORT\n");
        return -1;
    }
    if (aq_get_qparent_teid(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get parent TEID\n");
        return -1;
    }
    if (add_tx_queues(d, d->txq_count) < 0) {
        fprintf(stderr, "[my_ice] add tx queues failed\n");
        return -1;
    }

    tx_ring_init(d);

    memcpy(pkt + 0, dst_mac, ETHER_ADDR_LEN);
    memcpy(pkt + 6, d->io.mac, ETHER_ADDR_LEN);
    pkt[12] = 0x88;
    pkt[13] = 0xB5;
    if (payload_len > 0)
        memset(pkt + 14, 'A', (size_t)payload_len);

    frame_len = (uint16_t)(14 + payload_len);
    if (frame_len < 60)
        frame_len = 60;

    for (q_idx = 0; q_idx < d->txq_count; q_idx++) {
        struct txq_ctx *q = &d->txqs[q_idx];
        uint16_t di;
        for (di = 0; di < q->desc_count; di++) {
            uint8_t *buf = q->tx_pkt_bufs + ((size_t)di * ICE_TX_PKT_BUF_SIZE);
            memcpy(buf, pkt, frame_len);
        }
    }

    if (pin_cpus) {
        cpu_count = build_cpu_list(cpu_list, (int)(sizeof(cpu_list) / sizeof(cpu_list[0])));
        if (cpu_count <= 0) {
            fprintf(stderr, "[my_ice] warning: failed to read CPU affinity, disabling pinning\n");
            pin_cpus = false;
        } else {
            fprintf(stderr, "[my_ice] pinning tx threads to CPUs (count=%d)\n", cpu_count);
        }
    }

    fprintf(stderr,
            "[my_ice] tx-bench on vsi=%u lport=%u txqs=%u txq_base=%u src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x seconds=%d payload_len=%d frame_len=%u\n",
            d->io.vsi_num, d->io.lport, d->txq_count, d->txqs[0].txq_id,
            d->io.mac[0], d->io.mac[1], d->io.mac[2],
            d->io.mac[3], d->io.mac[4], d->io.mac[5],
            dst_mac[0], dst_mac[1], dst_mac[2],
            dst_mac[3], dst_mac[4], dst_mac[5],
            seconds, payload_len, frame_len);

    gotc_start = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                    GLV_GOTCH(d->io.vsi_num));
    gotc_prev = gotc_start;

    start_ns = monotonic_ns();
    end_ns = start_ns + (uint64_t)seconds * one_sec_ns;
    last_report_ns = start_ns;
    next_report_ns = start_ns + one_sec_ns;

    threads = calloc(d->txq_count, sizeof(*threads));
    workers = calloc(d->txq_count, sizeof(*workers));
    if (!threads || !workers) {
        fprintf(stderr, "[my_ice] failed to allocate tx bench workers\n");
        free(threads);
        free(workers);
        return -1;
    }

    for (q_idx = 0; q_idx < d->txq_count; q_idx++) {
        struct tx_bench_worker *w = &workers[q_idx];

        w->d = d;
        w->q = &d->txqs[q_idx];
        w->end_ns = end_ns;
        w->frame_len = frame_len;
        w->cpu_id = -1;
        w->err = 0;
        memcpy(w->pkt, pkt, frame_len);

        if (pin_cpus && cpu_count > 0)
            w->cpu_id = cpu_list[q_idx % cpu_count];

        if (pthread_create(&threads[q_idx], NULL, tx_bench_worker_main, w) != 0) {
            fprintf(stderr, "[my_ice] failed to create tx thread %u\n", q_idx);
            workers[q_idx].err = -1;
            threads[q_idx] = 0;
        }
    }

    while (true) {
        now_ns = monotonic_ns();
        if (now_ns >= end_ns)
            break;

        if (now_ns >= next_report_ns) {
            uint64_t interval_ns;
            uint64_t interval_bytes;
            double interval_s;
            double gbps;

            gotc_now = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                          GLV_GOTCH(d->io.vsi_num));
            interval_ns = now_ns - last_report_ns;
            interval_bytes = gotc_now - gotc_prev;
            interval_s = (double)interval_ns / (double)one_sec_ns;

            gbps = interval_s > 0.0 ?
                ((double)interval_bytes * 8.0) / (interval_s * 1e9) : 0.0;

            fprintf(stderr,
                    "[my_ice] tx-bench t=%.2fs interval: Gbps=%.3f\n",
                    (double)(now_ns - start_ns) / (double)one_sec_ns,
                    gbps);

            last_report_ns = now_ns;
            gotc_prev = gotc_now;
            next_report_ns += one_sec_ns;
            if (next_report_ns < now_ns)
                next_report_ns = now_ns + one_sec_ns;
        }
        usleep(1000);
    }

    for (q_idx = 0; q_idx < d->txq_count; q_idx++) {
        if (threads[q_idx])
            pthread_join(threads[q_idx], NULL);
    }

    for (q_idx = 0; q_idx < d->txq_count; q_idx++)
        (void)tx_wait_drain(d, &d->txqs[q_idx], 1000);

    for (q_idx = 0; q_idx < d->txq_count; q_idx++) {
        if (workers[q_idx].err) {
            fprintf(stderr, "[my_ice] tx-bench worker %u reported error\n", q_idx);
            free(threads);
            free(workers);
            return -1;
        }
    }

    free(threads);
    free(workers);

    now_ns = monotonic_ns();
    gotc_now = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                  GLV_GOTCH(d->io.vsi_num));

    {
        uint64_t total_ns = now_ns - start_ns;
        uint64_t total_bytes = gotc_now - gotc_start;
        double total_s = (double)total_ns / (double)one_sec_ns;
        double avg_gbps = total_s > 0.0 ?
            ((double)total_bytes * 8.0) / (total_s * 1e9) : 0.0;

        fprintf(stderr,
                "[my_ice] tx-bench done: seconds=%.3f avg_Gbps=%.3f\n",
                total_s, avg_gbps);
    }

    return 0;
}

static void cleanup(struct dev_ctx *d)
{
    uint16_t i;

    if (d->bar0 && d->bar0 != MAP_FAILED)
        munmap(d->bar0, d->bar0_size);

    if (d->dma.vaddr && d->dma.vaddr != MAP_FAILED) {
        struct vfio_iommu_type1_dma_unmap unmap = {0};
        unmap.argsz = sizeof(unmap);
        unmap.iova = d->dma.iova;
        unmap.size = d->dma.size;
        if (d->container_fd >= 0)
            (void)ioctl(d->container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
        munmap(d->dma.vaddr, d->dma.size);
    }

    if (d->device_fd >= 0)
        close(d->device_fd);
    if (d->group_fd >= 0)
        close(d->group_fd);
    if (d->container_fd >= 0)
        close(d->container_fd);
    if (d->huge_fd >= 0)
        close(d->huge_fd);
    if (d->huge_path[0] != '\0')
        unlink(d->huge_path);
    for (i = 0; i < d->io.rxq_count; i++) {
        free(d->io.rxqs[i].rx_pkt_bufs);
        free(d->reflect_pools[i].free_ring);
    }
    for (i = 0; i < d->txq_alloc_count; i++)
        free(d->txqs[i].tx_pkt_buf_refs);
    free(d->txqs);
}

static size_t get_hugepage_size(void)
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

static size_t prepare_hugepage_file(struct dev_ctx *d, size_t size, const char *dir)
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
    return aligned;
}

int main(int argc, char **argv)
{
    struct dev_ctx d = {
        .container_fd = -1,
        .group_fd = -1,
        .device_fd = -1,
        .huge_fd = -1,
        .huge_path = {0},
    };
    const char *bdf;
    bool run_rx_listen_mode = false;
    bool run_rx_reflect_mode = false;
    bool run_tx_send_mode = false;
    bool run_tx_bench_mode = false;
    bool use_hugepages = false;
    const char *huge_dir = "/mnt/huge";
    uint16_t rxq_count = 1;
    uint16_t txq_count = 1;
    uint16_t tx_desc_count = ICE_TX_DESC_COUNT;
    bool pin_cpus = false;
    bool dump_topo = false;
    uint32_t qparent_override = 0;
    bool qparent_override_set = false;
    int rx_listen_timeout_s = 30;
    int rx_reflect_timeout_s = 30;
    uint8_t tx_send_dst_mac[ETHER_ADDR_LEN] = {0};
    int tx_send_count = 1;
    int tx_send_interval_ms = 100;
    const char *tx_send_payload = "my_ice-userspace-tx";
    uint8_t tx_bench_dst_mac[ETHER_ADDR_LEN] = {0};
    int tx_bench_seconds = 0;
    int tx_bench_payload_len = 0;
    size_t dma_bytes;
    size_t dma_map_bytes;
    int rc = EXIT_FAILURE;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <BDF> [--rx-listen [seconds]|--rx-reflect [seconds]|--tx-send <dst-mac> [count] [interval-ms] [payload]|--tx-bench <seconds> <dst-mac> <payload-len>] [--rx-queues <1|2|4|8>] [--tx-queues <n>] [--tx-desc-count <n>] [--pin-cpus] [--dump-topo] [--qparent-teid <hex>] [--hugepages [--hugepage-dir <dir>]]\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-listen\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-listen 60\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-reflect 60\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-send aa:bb:cc:dd:ee:ff\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-send aa:bb:cc:dd:ee:ff 20 100 hello\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-bench 10 aa:bb:cc:dd:ee:ff 46\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-bench 15 aa:bb:cc:dd:ee:ff 1472 --hugepages --hugepage-dir /mnt/huge\n", argv[0]);
        return EXIT_FAILURE;
    }

    bdf = argv[1];
    if (snprintf(d.pci_bdf, sizeof(d.pci_bdf), "%s", bdf) < 0 ||
        strlen(bdf) >= sizeof(d.pci_bdf)) {
        fprintf(stderr, "pci bdf too long: '%s'\n", bdf);
        return EXIT_FAILURE;
    }
    if (argc >= 3) {
        int i = 2;
        bool mode_set = false;

        while (i < argc) {
            if (strcmp(argv[i], "--rx-listen") == 0) {
                if (mode_set) {
                    fprintf(stderr, "only one mode allowed (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
                    return EXIT_FAILURE;
                }
                mode_set = true;
                run_rx_listen_mode = true;
                i++;
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    if (parse_int_range(argv[i], 1, 3600, &rx_listen_timeout_s) < 0) {
                        fprintf(stderr, "invalid --rx-listen timeout '%s' (expected 1..3600 seconds)\n",
                                argv[i]);
                        return EXIT_FAILURE;
                    }
                    i++;
                }
            } else if (strcmp(argv[i], "--rx-reflect") == 0) {
                if (mode_set) {
                    fprintf(stderr, "only one mode allowed (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
                    return EXIT_FAILURE;
                }
                mode_set = true;
                run_rx_reflect_mode = true;
                i++;
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    if (parse_int_range(argv[i], 1, 3600, &rx_reflect_timeout_s) < 0) {
                        fprintf(stderr, "invalid --rx-reflect timeout '%s' (expected 1..3600 seconds)\n",
                                argv[i]);
                        return EXIT_FAILURE;
                    }
                    i++;
                }
            } else if (strcmp(argv[i], "--tx-send") == 0) {
                if (mode_set) {
                    fprintf(stderr, "only one mode allowed (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
                    return EXIT_FAILURE;
                }
                mode_set = true;
                run_tx_send_mode = true;
                i++;
                if (i >= argc) {
                    fprintf(stderr, "--tx-send requires <dst-mac>\n");
                    return EXIT_FAILURE;
                }
                if (parse_mac_addr(argv[i], tx_send_dst_mac) < 0) {
                    fprintf(stderr, "invalid dst MAC '%s' (expected aa:bb:cc:dd:ee:ff)\n",
                            argv[i]);
                    return EXIT_FAILURE;
                }
                i++;
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    if (parse_int_range(argv[i], 1, 1000000, &tx_send_count) < 0) {
                        fprintf(stderr, "invalid --tx-send count '%s' (expected 1..1000000)\n",
                                argv[i]);
                        return EXIT_FAILURE;
                    }
                    i++;
                }
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    if (parse_int_range(argv[i], 0, 60000, &tx_send_interval_ms) < 0) {
                        fprintf(stderr, "invalid --tx-send interval '%s' ms (expected 0..60000)\n",
                                argv[i]);
                        return EXIT_FAILURE;
                    }
                    i++;
                }
                if (i < argc && strncmp(argv[i], "--", 2) != 0) {
                    tx_send_payload = argv[i];
                    i++;
                }
            } else if (strcmp(argv[i], "--tx-bench") == 0) {
                if (mode_set) {
                    fprintf(stderr, "only one mode allowed (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
                    return EXIT_FAILURE;
                }
                mode_set = true;
                run_tx_bench_mode = true;
                if (i + 3 >= argc) {
                    fprintf(stderr, "--tx-bench requires <seconds> <dst-mac> <payload-len>\n");
                    return EXIT_FAILURE;
                }
                if (parse_int_range(argv[i + 1], 1, 3600, &tx_bench_seconds) < 0) {
                    fprintf(stderr, "invalid --tx-bench seconds '%s' (expected 1..3600)\n",
                            argv[i + 1]);
                    return EXIT_FAILURE;
                }
                if (parse_mac_addr(argv[i + 2], tx_bench_dst_mac) < 0) {
                    fprintf(stderr, "invalid dst MAC '%s' (expected aa:bb:cc:dd:ee:ff)\n",
                            argv[i + 2]);
                    return EXIT_FAILURE;
                }
                if (parse_int_range(argv[i + 3], 0, ICE_TX_PKT_BUF_SIZE - 14,
                                    &tx_bench_payload_len) < 0) {
                    fprintf(stderr, "invalid --tx-bench payload-len '%s' (expected 0..%d)\n",
                            argv[i + 3], ICE_TX_PKT_BUF_SIZE - 14);
                    return EXIT_FAILURE;
                }
                i += 4;
            } else if (strcmp(argv[i], "--rx-queues") == 0) {
                int v;
                if (i + 1 >= argc) {
                    fprintf(stderr, "--rx-queues requires <n>\n");
                    return EXIT_FAILURE;
                }
                if (parse_int_range(argv[i + 1], 1, ICE_MAX_RX_QUEUES, &v) < 0) {
                    fprintf(stderr, "invalid --rx-queues '%s' (expected 1,2,4,8)\n",
                            argv[i + 1]);
                    return EXIT_FAILURE;
                }
                if (!(v == 1 || v == 2 || v == 4 || v == 8)) {
                    fprintf(stderr, "unsupported --rx-queues '%d' (expected 1,2,4,8)\n", v);
                    return EXIT_FAILURE;
                }
                rxq_count = (uint16_t)v;
                i += 2;
            } else if (strcmp(argv[i], "--hugepages") == 0) {
                use_hugepages = true;
                i++;
            } else if (strcmp(argv[i], "--hugepage-dir") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "--hugepage-dir requires <dir>\n");
                    return EXIT_FAILURE;
                }
                huge_dir = argv[i + 1];
                i += 2;
            } else if (strcmp(argv[i], "--tx-queues") == 0) {
                int v;
                if (i + 1 >= argc) {
                    fprintf(stderr, "--tx-queues requires <n>\n");
                    return EXIT_FAILURE;
                }
                if (parse_int_range(argv[i + 1], 1, 1024, &v) < 0) {
                    fprintf(stderr, "invalid --tx-queues '%s' (expected 1..1024)\n",
                            argv[i + 1]);
                    return EXIT_FAILURE;
                }
                txq_count = (uint16_t)v;
                i += 2;
            } else if (strcmp(argv[i], "--tx-desc-count") == 0) {
                int v;
                if (i + 1 >= argc) {
                    fprintf(stderr, "--tx-desc-count requires <n>\n");
                    return EXIT_FAILURE;
                }
                if (parse_int_range(argv[i + 1], 32, 8192, &v) < 0) {
                    fprintf(stderr, "invalid --tx-desc-count '%s' (expected 32..8192)\n",
                            argv[i + 1]);
                    return EXIT_FAILURE;
                }
                tx_desc_count = (uint16_t)v;
                i += 2;
            } else if (strcmp(argv[i], "--pin-cpus") == 0) {
                pin_cpus = true;
                i++;
            } else if (strcmp(argv[i], "--dump-topo") == 0) {
                dump_topo = true;
                i++;
            } else if (strcmp(argv[i], "--qparent-teid") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "--qparent-teid requires <hex>\n");
                    return EXIT_FAILURE;
                }
                if (parse_u32_hex(argv[i + 1], &qparent_override) < 0) {
                    fprintf(stderr, "invalid --qparent-teid '%s'\n", argv[i + 1]);
                    return EXIT_FAILURE;
                }
                qparent_override_set = true;
                i += 2;
            } else {
                fprintf(stderr, "unknown option: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
        }

        if (!mode_set) {
            fprintf(stderr, "one mode is required (--rx-listen/--rx-reflect/--tx-send/--tx-bench)\n");
            return EXIT_FAILURE;
        }
    }

    if (!run_rx_reflect_mode && rxq_count > 1) {
        fprintf(stderr, "[my_ice] ignoring --rx-queues=%u outside --rx-reflect\n", rxq_count); // RSS:IMPL
        rxq_count = 1;
    }
    if (run_rx_reflect_mode)
        txq_count = rxq_count;

    d.io.rxq_count = rxq_count;
    d.io.rxq_poll_next = 0;
    d.txq_count = txq_count;
    d.txq_alloc_count = txq_count;
    d.tx_desc_count = tx_desc_count;
    d.txqs = calloc(d.txq_count, sizeof(*d.txqs));
    if (!d.txqs)
        die_errno("calloc txqs");
    alloc_queue_sw_state(&d);
    g_dump_topo = dump_topo;
    g_qparent_override_set = qparent_override_set;
    g_qparent_override = qparent_override;

    fprintf(stderr, "[my_ice] opening VFIO device %s\n", bdf);
    vfio_init(&d, bdf);
    fprintf(stderr, "[my_ice] vfio init done, bar0_size=0x%zx\n", d.bar0_size);

    dma_bytes =
        ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc) +
        ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc) +
        ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN +
        ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN +
        (size_t)d.txq_alloc_count * d.tx_desc_count * sizeof(struct ice_tx_desc) +
        (size_t)d.txq_alloc_count * d.tx_desc_count * ICE_TX_PKT_BUF_SIZE +
        (size_t)d.io.rxq_count * ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc) +
        (size_t)d.io.rxq_count * ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE +
        reflect_pool_dma_bytes_total(&d) +
        8192;

    dma_map_bytes = dma_bytes;
    if (use_hugepages) {
        fprintf(stderr, "[my_ice] using hugepages from %s\n", huge_dir);
        dma_map_bytes = prepare_hugepage_file(&d, dma_bytes, huge_dir);
    }

    dma_map(&d, dma_map_bytes);
    layout_dma(&d);
    pkt_pool_init(&d);
    adminq_hw_init(&d);

    fprintf(stderr, "[my_ice] adminq initialized, sending GET_VER\n");
    if (aq_get_fw_ver(&d) < 0)
        goto out;

    fprintf(stderr, "[my_ice] GET_VER ok, sending MANAGE_MAC_READ\n");
    if (aq_manage_mac_read(&d) < 0)
        goto out;

    if (run_rx_listen_mode) {
        fprintf(stderr, "[my_ice] running rx listen path\n");
        if (run_rx_listen(&d, rx_listen_timeout_s * 1000) < 0)
            goto out;
    } else if (run_rx_reflect_mode) {
        fprintf(stderr, "[my_ice] running rx reflect path%s\n",
                d.io.rxq_count > 1 ? " (multithreaded)" : "");
        if ((d.io.rxq_count > 1 ? run_rx_reflect_mt(&d, rx_reflect_timeout_s * 1000)
                                : run_rx_reflect(&d, rx_reflect_timeout_s * 1000)) < 0)
            goto out;
    } else if (run_tx_send_mode) {
        fprintf(stderr, "[my_ice] running tx send path\n");
        if (run_tx_send(&d, tx_send_dst_mac, tx_send_count,
                        tx_send_interval_ms, tx_send_payload) < 0)
            goto out;
    } else if (run_tx_bench_mode) {
        fprintf(stderr, "[my_ice] running tx bench path\n");
        if (run_tx_bench(&d, tx_bench_dst_mac, tx_bench_seconds,
                         tx_bench_payload_len, pin_cpus) < 0)
            goto out;
    }

    rc = EXIT_SUCCESS;

out:
    cleanup(&d);
    return rc;
}

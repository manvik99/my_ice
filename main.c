#define _GNU_SOURCE
#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/vfio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

#define PCI_COMMAND_OFF 0x04
#define PCI_COMMAND_MEM 0x2
#define PCI_COMMAND_MASTER 0x4

#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((size_t)((a) - 1)))

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

struct io_ring_ctx {
    uint8_t mac[ETHER_ADDR_LEN];
    uint8_t lport;
    uint16_t vsi_num;
    uint16_t txq_id;
    uint16_t rxq_id;
    uint32_t qparent_teid;

    struct ice_tx_desc *tx_desc;
    uint64_t tx_desc_iova;
    uint8_t *tx_pkt_buf;
    uint64_t tx_pkt_iova;
    uint16_t tx_tail;

    union ice_32b_rx_flex_desc *rx_desc;
    uint64_t rx_desc_iova;
    uint8_t *rx_bufs;
    uint64_t rx_bufs_iova;
    uint16_t rx_ntc;
};

struct dev_ctx {
    int container_fd;
    int group_fd;
    int device_fd;
    int group_id;
    uint8_t *bar0;
    size_t bar0_size;

    struct dma_block dma;
    struct aq_ring_ctx atq;
    struct aq_ring_ctx arq;
    struct io_ring_ctx io;
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

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        die_errno("clock_gettime");

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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
    d->dma.vaddr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

    PLACE(d->io.tx_desc, d->io.tx_desc_iova, ICE_TX_DESC_COUNT * sizeof(struct ice_tx_desc), 128);
    PLACE(d->io.tx_pkt_buf, d->io.tx_pkt_iova, ICE_TX_PKT_BUF_SIZE, 128);
    PLACE(d->io.rx_desc, d->io.rx_desc_iova, ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc), 128);
    PLACE(d->io.rx_bufs, d->io.rx_bufs_iova, ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE, 128);

#undef PLACE

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

    memset(topo_buf, 0, sizeof(topo_buf));
    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_GET_DFLT_TOPO);
    desc.params.get_topo.port_num = d->io.lport;

    if (aq_send_cmd(d, &desc, topo_buf, sizeof(topo_buf)) < 0) {
        fprintf(stderr, "[my_ice] GET_DFLT_TOPO failed for lport=%u\n", d->io.lport);
        return -1;
    }

    num_branches = desc.params.get_topo.num_branches;
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

static int add_one_tx_queue(struct dev_ctx *d)
{
    struct ice_aqc_add_tx_qgrp qg;
    struct ice_aq_desc desc;
    struct ice_tlan_ctx tlan = {0};

    memset(&qg, 0, sizeof(qg));

    tlan.port_num = d->io.lport;
    tlan.qlen = ICE_TX_DESC_COUNT;
    tlan.base = d->io.tx_desc_iova >> 7;
    tlan.pf_num = 0;
    tlan.vmvf_type = ICE_TLAN_CTX_VMVF_TYPE_PF;
    tlan.src_vsi = d->io.vsi_num;
    tlan.tso_ena = 1;
    tlan.internal_usage_flag = 1;
    tlan.tso_qnum = d->io.txq_id;
    tlan.legacy_int = 1;

    qg.parent_teid = htole32(d->io.qparent_teid);
    qg.num_txqs = 1;
    qg.txqs[0].txq_id = htole16(d->io.txq_id);
    set_ctx_bits((uint8_t *)&tlan, qg.txqs[0].txq_ctx, tlan_ctx_info);

    qg.txqs[0].info.valid_sections =
        ICE_AQC_ELEM_VALID_GENERIC | ICE_AQC_ELEM_VALID_CIR | ICE_AQC_ELEM_VALID_EIR;
    qg.txqs[0].info.generic = 0;
    qg.txqs[0].info.cir_bw.bw_profile_idx = htole16(ICE_SCHED_DFLT_RL_PROF_ID);
    qg.txqs[0].info.cir_bw.bw_alloc = htole16(ICE_SCHED_DFLT_BW_WT);
    qg.txqs[0].info.eir_bw.bw_profile_idx = htole16(ICE_SCHED_DFLT_RL_PROF_ID);
    qg.txqs[0].info.eir_bw.bw_alloc = htole16(ICE_SCHED_DFLT_BW_WT);

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_ADD_TXQS);
    desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);
    desc.params.add_txqs.num_qgrps = 1;

    if (aq_send_cmd(d, &desc, &qg, sizeof(qg)) < 0)
        return -1;

    fprintf(stderr, "[my_ice] ADD_TXQS resp: txq_id=%u q_teid=0x%x\n",
            le16toh(qg.txqs[0].txq_id), le32toh(qg.txqs[0].q_teid));

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

static int setup_and_enable_rxq(struct dev_ctx *d)
{
    struct ice_rlan_ctx rlan = {0};
    uint8_t ctx_buf[ICE_RXQ_CTX_SZ] = {0};
    uint16_t q = d->io.rxq_id;
    uint32_t reg;
    int i;

    memset(d->io.rx_desc, 0, ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc));
    memset(d->io.rx_bufs, 0, ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE);

    for (i = 0; i < ICE_RX_DESC_COUNT; i++) {
        uint64_t biova = d->io.rx_bufs_iova + (uint64_t)i * ICE_RX_BUF_SIZE;
        d->io.rx_desc[i].read.pkt_addr = htole64(biova);
        d->io.rx_desc[i].read.hdr_addr = 0;
    }

    rlan.base = d->io.rx_desc_iova >> 7;
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

    d->io.rx_ntc = 0;
    reg_write32(d, QRX_TAIL(q), ICE_RX_DESC_COUNT - 1);
    return 0;
}

static int send_one_packet(struct dev_ctx *d, const uint8_t *pkt, uint16_t len)
{
    uint16_t idx = d->io.tx_tail;
    uint16_t next = (uint16_t)((idx + 1) % ICE_TX_DESC_COUNT);
    uint64_t qw1;
    struct ice_tx_desc *txd;
    struct timespec start, now;
    int saw_desc_done = 0;
    int i;

    if (len > ICE_TX_PKT_BUF_SIZE)
        return -1;

    memcpy(d->io.tx_pkt_buf, pkt, len);

    txd = &d->io.tx_desc[idx];
    memset(txd, 0, sizeof(*txd));
    txd->buf_addr = htole64(d->io.tx_pkt_iova);

    qw1 = ((uint64_t)ICE_TX_DESC_DTYPE_DATA << ICE_TXD_QW1_DTYPE_S) |
          ((uint64_t)(ICE_TX_DESC_CMD_EOP | ICE_TX_DESC_CMD_RS) << ICE_TXD_QW1_CMD_S) |
          ((uint64_t)len << ICE_TXD_QW1_TX_BUF_SZ_S);
    txd->cmd_type_offset_bsz = htole64(qw1);

    __sync_synchronize();
    d->io.tx_tail = next;
    reg_write32(d, QTX_COMM_DBELL(d->io.txq_id), d->io.tx_tail);

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        die_errno("clock_gettime start");

    for (i = 0; i < 20000; i++) {
        uint64_t ctb = le64toh(txd->cmd_type_offset_bsz);
        uint64_t dtype = (ctb & (0xFULL << ICE_TXD_QW1_DTYPE_S)) >>
                         ICE_TXD_QW1_DTYPE_S;
        long elapsed_ms;

        if (dtype == ICE_TX_DESC_DTYPE_DESC_DONE) {
            saw_desc_done = 1;
            break;
        }

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            die_errno("clock_gettime now");
        elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
                     (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= 200)
            break;

        usleep(10);
    }

    if (!saw_desc_done) {
        uint32_t head = reg_read32(d, QTX_COMM_HEAD(d->io.txq_id)) & 0x1FFFU;
        fprintf(stderr, "[my_ice] tx desc done not observed (QTX head=%u tail=%u), continuing to RX poll\n",
                head, d->io.tx_tail);
    }

    return 0;
}

static void rearm_rx_desc(struct dev_ctx *d, uint16_t idx)
{
    d->io.rx_desc[idx].read.pkt_addr =
        htole64(d->io.rx_bufs_iova + (uint64_t)idx * ICE_RX_BUF_SIZE);
    d->io.rx_desc[idx].read.hdr_addr = 0;
    d->io.rx_desc[idx].read.rsvd1 = 0;
    d->io.rx_desc[idx].read.rsvd2 = 0;

    reg_write32(d, QRX_TAIL(d->io.rxq_id), idx);
    d->io.rx_ntc = (uint16_t)((idx + 1) % ICE_RX_DESC_COUNT);
}

static int poll_one_rx_packet(struct dev_ctx *d, uint8_t *out, uint16_t out_sz, uint16_t *out_len)
{
    uint16_t n;

    for (n = 0; n < ICE_RX_DESC_COUNT; n++) {
        uint16_t idx = (uint16_t)((d->io.rx_ntc + n) % ICE_RX_DESC_COUNT);
        union ice_32b_rx_flex_desc *rxd = &d->io.rx_desc[idx];
        uint16_t status0 = le16toh(rxd->wb.status_error0);
        uint16_t pkt_len;

        if (!(status0 & BIT(ICE_RX_FLEX_DESC_STATUS0_DD_S)))
            continue;

        pkt_len = (uint16_t)(le16toh(rxd->wb.pkt_len) & ICE_RX_FLX_DESC_PKT_LEN_M);
        if (!(status0 & BIT(ICE_RX_FLEX_DESC_STATUS0_EOF_S)) ||
            (status0 & BIT(ICE_RX_FLEX_DESC_STATUS0_RXE_S)) ||
            pkt_len == 0) {
            fprintf(stderr,
                    "[my_ice] rx descriptor error idx=%u status0=0x%04x pkt_len=%u\n",
                    idx, status0, pkt_len);
            rearm_rx_desc(d, idx);
            return -1;
        }

        *out_len = pkt_len;
        if (*out_len > out_sz)
            *out_len = out_sz;

        memcpy(out, d->io.rx_bufs + ((size_t)idx * ICE_RX_BUF_SIZE), *out_len);
        rearm_rx_desc(d, idx);
        return 1;
    }

    return 0;
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

    for (i = 0; i < 8; i++) {
        uint16_t idx = (uint16_t)((d->io.rx_ntc + i) % ICE_RX_DESC_COUNT);
        union ice_32b_rx_flex_desc *rxd = &d->io.rx_desc[idx];
        uint16_t status0 = le16toh(rxd->wb.status_error0);
        uint16_t pkt_len = (uint16_t)(le16toh(rxd->wb.pkt_len) & ICE_RX_FLX_DESC_PKT_LEN_M);
        fprintf(stderr,
                "[my_ice] rxdesc[%u] rxdid=%u status0=0x%04x pkt_len=%u\n",
                idx, rxd->wb.rxdid, status0, pkt_len);
    }
}

static uint64_t read_glv_counter64(struct dev_ctx *d, uint32_t lo_off, uint32_t hi_off)
{
    uint64_t lo = reg_read32(d, lo_off);
    uint64_t hi = reg_read32(d, hi_off) & 0xFFU;
    return lo | (hi << 32);
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

static int run_rx_listen(struct dev_ctx *d, int timeout_ms)
{
    uint32_t rx_alloc;
    uint8_t rxpkt[2048];
    uint16_t rxlen = 0;
    uint16_t rx_mac_rule_idx = UINT16_MAX;
    uint64_t gorc_before, gorc_after;
    int rc = -1;
    int i;

    rx_alloc = reg_read32(d, PFLAN_RX_QALLOC);
    if (!(rx_alloc & PFLAN_RX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (RX=0x%08x)\n", rx_alloc);
        goto out;
    }

    d->io.rxq_id = (uint16_t)(rx_alloc & PFLAN_RX_QALLOC_FIRSTQ_M);

    if (aq_get_default_vsi_and_lport(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get default VSI/LPORT\n");
        goto out;
    }

    fprintf(stderr, "[my_ice] rx-listen on vsi=%u lport=%u rxq=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            d->io.vsi_num, d->io.lport, d->io.rxq_id,
            d->io.mac[0], d->io.mac[1], d->io.mac[2],
            d->io.mac[3], d->io.mac[4], d->io.mac[5]);

    if (setup_and_enable_rxq(d) < 0) {
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
            d->io.vsi_num, gorc_after - gorc_before);
    dump_rx_desc_snapshot(d);
    dump_mdet_regs(d);

out:
    aq_remove_sw_rule_best_effort(d, ICE_AQC_SW_RULES_T_LKUP_RX, rx_mac_rule_idx);
    return rc;
}

static int run_tx_send(struct dev_ctx *d, const uint8_t *dst_mac, int count,
                       int interval_ms, const char *payload)
{
    uint32_t tx_alloc;
    uint8_t pkt[ICE_TX_PKT_BUF_SIZE] = {0};
    size_t payload_len;
    uint16_t frame_len;
    uint64_t gotc_before, gotc_after;
    int i;

    tx_alloc = reg_read32(d, PFLAN_TX_QALLOC);
    if (!(tx_alloc & PFLAN_TX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (TX=0x%08x)\n", tx_alloc);
        return -1;
    }

    d->io.txq_id = (uint16_t)(tx_alloc & PFLAN_TX_QALLOC_FIRSTQ_M);

    if (aq_get_default_vsi_and_lport(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get default VSI/LPORT\n");
        return -1;
    }
    if (aq_get_qparent_teid(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get parent TEID\n");
        return -1;
    }
    if (add_one_tx_queue(d) < 0) {
        fprintf(stderr, "[my_ice] add tx queue failed\n");
        return -1;
    }

    memset(d->io.tx_desc, 0, ICE_TX_DESC_COUNT * sizeof(struct ice_tx_desc));
    d->io.tx_tail = 0;

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
            d->io.vsi_num, d->io.lport, d->io.txq_id,
            d->io.mac[0], d->io.mac[1], d->io.mac[2],
            d->io.mac[3], d->io.mac[4], d->io.mac[5],
            dst_mac[0], dst_mac[1], dst_mac[2],
            dst_mac[3], dst_mac[4], dst_mac[5],
            count, interval_ms, payload_len, frame_len);

    gotc_before = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                     GLV_GOTCH(d->io.vsi_num));

    for (i = 0; i < count; i++) {
        if (send_one_packet(d, pkt, frame_len) < 0) {
            fprintf(stderr, "[my_ice] tx-send failed at packet %d/%d\n",
                    i + 1, count);
            return -1;
        }
        if (interval_ms > 0)
            usleep((useconds_t)interval_ms * 1000U);
    }

    gotc_after = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                    GLV_GOTCH(d->io.vsi_num));
    fprintf(stderr, "[my_ice] tx-send done, VSI%u GOTC +%" PRIu64 " bytes\n",
            d->io.vsi_num, gotc_after - gotc_before);
    return 0;
}

static int run_tx_bench(struct dev_ctx *d, const uint8_t *dst_mac, int seconds,
                        int payload_len)
{
    const uint64_t one_sec_ns = 1000000000ULL;
    uint32_t tx_alloc;
    uint8_t pkt[ICE_TX_PKT_BUF_SIZE] = {0};
    uint16_t frame_len;
    uint64_t gotc_start, gotc_prev, gotc_now;
    uint64_t start_ns, end_ns, now_ns, next_report_ns, last_report_ns;
    uint64_t pkts_total = 0;
    uint64_t pkts_prev = 0;

    tx_alloc = reg_read32(d, PFLAN_TX_QALLOC);
    if (!(tx_alloc & PFLAN_TX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (TX=0x%08x)\n", tx_alloc);
        return -1;
    }

    d->io.txq_id = (uint16_t)(tx_alloc & PFLAN_TX_QALLOC_FIRSTQ_M);

    if (aq_get_default_vsi_and_lport(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get default VSI/LPORT\n");
        return -1;
    }
    if (aq_get_qparent_teid(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get parent TEID\n");
        return -1;
    }
    if (add_one_tx_queue(d) < 0) {
        fprintf(stderr, "[my_ice] add tx queue failed\n");
        return -1;
    }

    memset(d->io.tx_desc, 0, ICE_TX_DESC_COUNT * sizeof(struct ice_tx_desc));
    d->io.tx_tail = 0;

    memcpy(pkt + 0, dst_mac, ETHER_ADDR_LEN);
    memcpy(pkt + 6, d->io.mac, ETHER_ADDR_LEN);
    pkt[12] = 0x88;
    pkt[13] = 0xB5;
    if (payload_len > 0)
        memset(pkt + 14, 'A', (size_t)payload_len);

    frame_len = (uint16_t)(14 + payload_len);
    if (frame_len < 60)
        frame_len = 60;

    fprintf(stderr,
            "[my_ice] tx-bench on vsi=%u lport=%u txq=%u src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x seconds=%d payload_len=%d frame_len=%u\n",
            d->io.vsi_num, d->io.lport, d->io.txq_id,
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

    while (true) {
        now_ns = monotonic_ns();
        if (now_ns >= end_ns)
            break;

        if (send_one_packet(d, pkt, frame_len) < 0) {
            fprintf(stderr, "[my_ice] tx-bench send failed after %" PRIu64 " packets\n",
                    pkts_total);
            return -1;
        }
        pkts_total++;

        if ((pkts_total & 0x3FFU) == 0) {
            now_ns = monotonic_ns();
            if (now_ns >= next_report_ns) {
                uint64_t interval_ns;
                uint64_t interval_pkts;
                uint64_t interval_bytes;
                double interval_s;
                double mbps;
                double pps;
                double mpps;

                gotc_now = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                              GLV_GOTCH(d->io.vsi_num));
                interval_ns = now_ns - last_report_ns;
                interval_pkts = pkts_total - pkts_prev;
                interval_bytes = gotc_now - gotc_prev;
                interval_s = (double)interval_ns / (double)one_sec_ns;

                mbps = interval_s > 0.0 ?
                    ((double)interval_bytes * 8.0) / (interval_s * 1e6) : 0.0;
                pps = interval_s > 0.0 ?
                    (double)interval_pkts / interval_s : 0.0;
                mpps = pps / 1e6;

                fprintf(stderr,
                        "[my_ice] tx-bench t=%.2fs interval: Mbps=%.3f Mpps=%.3f PPS=%.0f bytes=%" PRIu64 " pkts=%" PRIu64 "\n",
                        (double)(now_ns - start_ns) / (double)one_sec_ns,
                        mbps, mpps, pps, interval_bytes, interval_pkts);

                last_report_ns = now_ns;
                gotc_prev = gotc_now;
                pkts_prev = pkts_total;
                next_report_ns += one_sec_ns;
                if (next_report_ns < now_ns)
                    next_report_ns = now_ns + one_sec_ns;
            }
        }
    }

    now_ns = monotonic_ns();
    gotc_now = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                  GLV_GOTCH(d->io.vsi_num));

    {
        uint64_t total_ns = now_ns - start_ns;
        uint64_t total_bytes = gotc_now - gotc_start;
        double total_s = (double)total_ns / (double)one_sec_ns;
        double avg_mbps = total_s > 0.0 ?
            ((double)total_bytes * 8.0) / (total_s * 1e6) : 0.0;
        double avg_pps = total_s > 0.0 ?
            (double)pkts_total / total_s : 0.0;
        double avg_mpps = avg_pps / 1e6;

        fprintf(stderr,
                "[my_ice] tx-bench done: seconds=%.3f total_bytes=%" PRIu64 " total_pkts=%" PRIu64 " avg_Mbps=%.3f avg_Mpps=%.3f avg_PPS=%.0f\n",
                total_s, total_bytes, pkts_total, avg_mbps, avg_mpps, avg_pps);
    }

    return 0;
}

static void cleanup(struct dev_ctx *d)
{
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
}

int main(int argc, char **argv)
{
    struct dev_ctx d = {
        .container_fd = -1,
        .group_fd = -1,
        .device_fd = -1,
    };
    const char *bdf;
    bool run_rx_listen_mode = false;
    bool run_tx_send_mode = false;
    bool run_tx_bench_mode = false;
    int rx_listen_timeout_s = 30;
    uint8_t tx_send_dst_mac[ETHER_ADDR_LEN] = {0};
    int tx_send_count = 1;
    int tx_send_interval_ms = 100;
    const char *tx_send_payload = "my_ice-userspace-tx";
    uint8_t tx_bench_dst_mac[ETHER_ADDR_LEN] = {0};
    int tx_bench_seconds = 0;
    int tx_bench_payload_len = 0;
    size_t dma_bytes;
    int rc = EXIT_FAILURE;

    if (argc < 2 || argc > 7) {
        fprintf(stderr, "Usage: %s <BDF> [--rx-listen [seconds]|--tx-send <dst-mac> [count] [interval-ms] [payload]|--tx-bench <seconds> <dst-mac> <payload-len>]\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-listen\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --rx-listen 60\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-send 40:a6:b7:c3:43:e8\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-send 40:a6:b7:c3:43:e8 20 100 hello\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:17:00.0 --tx-bench 10 40:a6:b7:c3:43:e8 46\n", argv[0]);
        return EXIT_FAILURE;
    }

    bdf = argv[1];
    if (argc >= 3) {
        if (strcmp(argv[2], "--rx-listen") == 0) {
            run_rx_listen_mode = true;
            if (argc == 4 &&
                parse_int_range(argv[3], 1, 3600, &rx_listen_timeout_s) < 0) {
                fprintf(stderr, "invalid --rx-listen timeout '%s' (expected 1..3600 seconds)\n",
                        argv[3]);
                return EXIT_FAILURE;
            }
            if (argc > 4) {
                fprintf(stderr, "too many arguments for --rx-listen\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[2], "--tx-send") == 0) {
            run_tx_send_mode = true;
            if (argc < 4) {
                fprintf(stderr, "--tx-send requires <dst-mac>\n");
                return EXIT_FAILURE;
            }
            if (parse_mac_addr(argv[3], tx_send_dst_mac) < 0) {
                fprintf(stderr, "invalid dst MAC '%s' (expected aa:bb:cc:dd:ee:ff)\n",
                        argv[3]);
                return EXIT_FAILURE;
            }
            if (argc >= 5 &&
                parse_int_range(argv[4], 1, 1000000, &tx_send_count) < 0) {
                fprintf(stderr, "invalid --tx-send count '%s' (expected 1..1000000)\n",
                        argv[4]);
                return EXIT_FAILURE;
            }
            if (argc >= 6 &&
                parse_int_range(argv[5], 0, 60000, &tx_send_interval_ms) < 0) {
                fprintf(stderr, "invalid --tx-send interval '%s' ms (expected 0..60000)\n",
                        argv[5]);
                return EXIT_FAILURE;
            }
            if (argc >= 7)
                tx_send_payload = argv[6];
        } else if (strcmp(argv[2], "--tx-bench") == 0) {
            run_tx_bench_mode = true;
            if (argc != 6) {
                fprintf(stderr, "--tx-bench requires <seconds> <dst-mac> <payload-len>\n");
                return EXIT_FAILURE;
            }
            if (parse_int_range(argv[3], 1, 3600, &tx_bench_seconds) < 0) {
                fprintf(stderr, "invalid --tx-bench seconds '%s' (expected 1..3600)\n",
                        argv[3]);
                return EXIT_FAILURE;
            }
            if (parse_mac_addr(argv[4], tx_bench_dst_mac) < 0) {
                fprintf(stderr, "invalid dst MAC '%s' (expected aa:bb:cc:dd:ee:ff)\n",
                        argv[4]);
                return EXIT_FAILURE;
            }
            if (parse_int_range(argv[5], 0, ICE_TX_PKT_BUF_SIZE - 14,
                                &tx_bench_payload_len) < 0) {
                fprintf(stderr, "invalid --tx-bench payload-len '%s' (expected 0..%d)\n",
                        argv[5], ICE_TX_PKT_BUF_SIZE - 14);
                return EXIT_FAILURE;
            }
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
    }

    fprintf(stderr, "[my_ice] opening VFIO device %s\n", bdf);
    vfio_init(&d, bdf);
    fprintf(stderr, "[my_ice] vfio init done, bar0_size=0x%zx\n", d.bar0_size);

    dma_bytes =
        ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc) +
        ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc) +
        ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN +
        ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN +
        ICE_TX_DESC_COUNT * sizeof(struct ice_tx_desc) +
        ICE_TX_PKT_BUF_SIZE +
        ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc) +
        ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE +
        8192;

    dma_map(&d, dma_bytes);
    layout_dma(&d);
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
    } else if (run_tx_send_mode) {
        fprintf(stderr, "[my_ice] running tx send path\n");
        if (run_tx_send(&d, tx_send_dst_mac, tx_send_count,
                        tx_send_interval_ms, tx_send_payload) < 0)
            goto out;
    } else if (run_tx_bench_mode) {
        fprintf(stderr, "[my_ice] running tx bench path\n");
        if (run_tx_bench(&d, tx_bench_dst_mac, tx_bench_seconds,
                         tx_bench_payload_len) < 0)
            goto out;
    }

    rc = EXIT_SUCCESS;

out:
    cleanup(&d);
    return rc;
}

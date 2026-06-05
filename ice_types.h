#ifndef ICE_TYPES_H
#define ICE_TYPES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ice_min.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((size_t)((a) - 1)))
#define NS_PER_S 1000000000ULL
#define TX_BURST_SIZE 64
#define DEFAULT_REFLECT_BATCH 128
#define MAX_REFLECT_BATCH 256
#define REFLECT_TX_DOORBELL_BATCH 256U
#define TX_RS_THRESH 256
#define ICE_PKT_BUF_DATA_SIZE \
    ((ICE_RX_BUF_SIZE > ICE_TX_PKT_BUF_SIZE) ? ICE_RX_BUF_SIZE : ICE_TX_PKT_BUF_SIZE)
#define ICE_PKT_BUF_INVALID_IDX UINT32_MAX

struct pkt_mempool;

struct pkt_buf {
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

struct io_ring_ctx {
    uint8_t mac[ETHER_ADDR_LEN];
    uint8_t lport;
    uint16_t vsi_num;
    uint16_t rxq_id;
    uint32_t qparent_teid;

    union ice_32b_rx_flex_desc *rx_desc;
    uint64_t rx_desc_iova;
    uint32_t *rx_pkt_buf_idxs;
    uint16_t rx_ntc;
    uint16_t rx_tail;
    uint16_t rx_rearmed_pending;
    uint32_t rx_tail_reg;
};

struct txq_ctx {
    uint16_t txq_id;
    uint16_t desc_count;
    struct ice_tx_desc *tx_desc;
    uint64_t tx_desc_iova;
    uint16_t tx_next_to_use;
    uint16_t tx_next_to_clean;
    uint16_t tx_free;
    uint16_t tx_pkts_since_rs;
    uint32_t *tx_pkt_buf_idxs;
    uint16_t *tx_rsq;
    uint16_t tx_rsq_count;
    uint16_t tx_rsq_pidx;
    uint16_t tx_rsq_cidx;
    uint32_t dbell_reg;
    uint32_t head_reg;
};

struct ice_vfio_dev {
    int container_fd;
    int group_fd;
    int device_fd;
    int group_id;
    int huge_fd;
    char huge_path[PATH_MAX];
    uint8_t *bar0;
    size_t bar0_size;
    size_t huge_alloc_size;
    bool pin_cpus;
    uint16_t tx_desc_count;
    uint16_t reflect_batch;
    struct txq_ctx txq;

    struct dma_block dma;
    struct aq_ring_ctx atq;
    struct aq_ring_ctx arq;
    struct io_ring_ctx io;
    struct pkt_mempool reflect_pool;
};

#endif

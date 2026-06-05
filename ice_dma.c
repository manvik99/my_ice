#define _GNU_SOURCE
#include <errno.h>
#include <linux/vfio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ice_dma.h"
#include "ice_utils.h"

uint32_t reflect_pool_entry_count(const struct ice_vfio_dev *d)
{
    uint16_t reflect_batch = d->reflect_batch > MAX_REFLECT_BATCH ? MAX_REFLECT_BATCH
                                                                  : d->reflect_batch;
    /*
     * Reflect mode needs enough pooled packet objects for three populations at once:
     *   1. every Rx descriptor armed up front,
     *   2. every Tx descriptor that may still be holding a borrowed reflected buffer,
     *   3. one extra burst of slack so the hot loop can grab replacements before freeing completions.
     */
    return (uint32_t)ICE_RX_DESC_COUNT + (uint32_t)d->tx_desc_count +
           (uint32_t)reflect_batch;
}

size_t reflect_pool_dma_bytes(const struct ice_vfio_dev *d)
{
    return (size_t)reflect_pool_entry_count(d) * ICE_PKT_BUF_ENTRY_SIZE;
}

void alloc_queue_sw_state(struct ice_vfio_dev *d)
{
    struct txq_ctx *q = &d->txq;

    d->io.rx_pkt_buf_idxs = calloc(ICE_RX_DESC_COUNT, sizeof(*d->io.rx_pkt_buf_idxs));
    if (!d->io.rx_pkt_buf_idxs)
        die_errno("calloc rx pkt buf slots");

    q->tx_pkt_buf_idxs = calloc(d->tx_desc_count, sizeof(*q->tx_pkt_buf_idxs));
    if (!q->tx_pkt_buf_idxs)
        die_errno("calloc tx pkt buf refs");

    q->tx_rsq_count = (uint16_t)(d->tx_desc_count / TX_RS_THRESH + 2);
    q->tx_rsq = calloc(q->tx_rsq_count, sizeof(*q->tx_rsq));
    if (!q->tx_rsq)
        die_errno("calloc tx rsq");

    d->reflect_pool.entry_size = ICE_PKT_BUF_ENTRY_SIZE;
    d->reflect_pool.num_entries = reflect_pool_entry_count(d);
    d->reflect_pool.free_ring =
        calloc(d->reflect_pool.num_entries, sizeof(*d->reflect_pool.free_ring));
    if (!d->reflect_pool.free_ring)
        die_errno("calloc reflect pool free ring");
}

void pkt_pool_init(struct ice_vfio_dev *d)
{
    struct pkt_mempool *pool = &d->reflect_pool;
    uint32_t i;

    /* Seed every pkt_buf with its DMA address and push every entry onto the free ring once at startup. */
    memset(pool->base, 0, (size_t)pool->num_entries * pool->entry_size);
    for (i = 0; i < pool->num_entries; i++) {
        pool->free_ring[i] = i;
    }
    pool->free_head = 0;
    pool->free_tail = 0;
    pool->free_count = pool->num_entries;
}



size_t ice_dma_required_bytes(const struct ice_vfio_dev *d)
{
    return ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc) +
           ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc) +
           ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN +
           ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN +
           (size_t)d->tx_desc_count * sizeof(struct ice_tx_desc) +
           ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc) +
           reflect_pool_dma_bytes(d) +
           8192;
}

void dma_map(struct ice_vfio_dev *d, size_t size)
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

void layout_dma(struct ice_vfio_dev *d)
{
    uint8_t *base = (uint8_t *)d->dma.vaddr;
    size_t off = 0;
    struct txq_ctx *q = &d->txq;

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

    PLACE(q->tx_desc, q->tx_desc_iova,
          (size_t)d->tx_desc_count * sizeof(struct ice_tx_desc), 128);
    PLACE(d->io.rx_desc, d->io.rx_desc_iova, ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc), 128);
    PLACE(d->reflect_pool.base, d->reflect_pool.base_iova, reflect_pool_dma_bytes(d), 4096);

#undef PLACE

    q->desc_count = d->tx_desc_count;
    q->tx_next_to_use = 0;
    q->tx_next_to_clean = 0;
    q->tx_pkts_since_rs = 0;
    q->tx_rsq_pidx = 0;
    q->tx_rsq_cidx = 0;
    q->dbell_reg = 0;
    q->head_reg = 0;

    if (off > d->dma.size)
        die_msg("DMA layout overflow");
}

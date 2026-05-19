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
    return (uint32_t)ICE_RX_DESC_COUNT + (uint32_t)d->tx_desc_count + ICE_REFLECT_POOL_EXTRA;
}

size_t reflect_pool_dma_bytes(const struct ice_vfio_dev *d)
{
    return (size_t)reflect_pool_entry_count(d) * ICE_PKT_BUF_ENTRY_SIZE;
}

static struct pkt_buf *pkt_pool_get_entry(struct pkt_mempool *pool, uint32_t idx)
{
    return (struct pkt_buf *)(pool->base + (size_t)idx * pool->entry_size);
}

void alloc_queue_sw_state(struct ice_vfio_dev *d)
{
    uint16_t i;

    d->io.rx_pkt_bufs = calloc(ICE_RX_DESC_COUNT, sizeof(*d->io.rx_pkt_bufs));
    if (!d->io.rx_pkt_bufs)
        die_errno("calloc rx pkt buf slots");

    for (i = 0; i < d->txq_alloc_count; i++) {
        d->txqs[i].tx_pkt_buf_refs =
            calloc(d->tx_desc_count, sizeof(*d->txqs[i].tx_pkt_buf_refs));
        if (!d->txqs[i].tx_pkt_buf_refs)
            die_errno("calloc tx pkt buf refs");

        d->txqs[i].tx_rsq_count = (uint16_t)(d->tx_desc_count / TX_RS_THRESH + 2);
        d->txqs[i].tx_rsq =
            calloc(d->txqs[i].tx_rsq_count, sizeof(*d->txqs[i].tx_rsq));
        if (!d->txqs[i].tx_rsq)
            die_errno("calloc tx rsq");
    }

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

    memset(pool->base, 0, (size_t)pool->num_entries * pool->entry_size);
    for (i = 0; i < pool->num_entries; i++) {
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

struct pkt_buf *pkt_buf_alloc(struct pkt_mempool *pool)
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

uint32_t pkt_buf_alloc_batch(struct pkt_mempool *pool, struct pkt_buf *bufs[],
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

uint32_t pkt_buf_alloc_batch_noinit(struct pkt_mempool *pool, struct pkt_buf *bufs[],
                                    uint32_t num_bufs)
{
    uint32_t avail;
    uint32_t head;
    uint32_t i;

    if (!pool || !bufs || num_bufs == 0)
        return 0;

    /* This noinit variant exists for rx-reflect: callers overwrite size/header data immediately. */
    avail = pool->free_count < num_bufs ? pool->free_count : num_bufs;
    head = pool->free_head;
    for (i = 0; i < avail; i++) {
        uint32_t idx = pool->free_ring[head];

        head++;
        if (head == pool->num_entries)
            head = 0;
        bufs[i] = pkt_pool_get_entry(pool, idx);
    }

    /* Simple hot-path fix: commit free_head/free_count once, not once per borrowed buffer. */
    pool->free_head = head;
    pool->free_count -= avail;

    return avail;
}

void pkt_buf_free(struct pkt_buf *buf)
{
    pkt_buf_free_fast(buf);
}



size_t ice_dma_required_bytes(const struct ice_vfio_dev *d)
{
    return ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc) +
           ICE_AQ_NUM_DESC * sizeof(struct ice_aq_desc) +
           ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN +
           ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN +
           (size_t)d->txq_alloc_count * d->tx_desc_count * sizeof(struct ice_tx_desc) +
           (size_t)d->txq_alloc_count * d->tx_desc_count * ICE_TX_PKT_BUF_SIZE +
           ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc) +
           ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE +
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
    struct ice_tx_desc *tx_desc_base = NULL;
    uint64_t tx_desc_iova = 0;
    uint8_t *tx_buf_base = NULL;
    uint64_t tx_buf_iova = 0;
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
    PLACE(d->io.rx_desc, d->io.rx_desc_iova, ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc), 128);
    PLACE(d->io.rx_bufs, d->io.rx_bufs_iova, ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE, 128);
    PLACE(d->reflect_pool.base, d->reflect_pool.base_iova, reflect_pool_dma_bytes(d), 4096);

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
        q->tx_rsq_pidx = 0;
        q->tx_rsq_cidx = 0;
    }

    if (off > d->dma.size)
        die_msg("DMA layout overflow");
}

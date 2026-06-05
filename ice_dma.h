#ifndef ICE_DMA_H
#define ICE_DMA_H

#include <stddef.h>
#include <stdint.h>

#include "ice_types.h"

uint32_t reflect_pool_entry_count(const struct ice_vfio_dev *d);
size_t reflect_pool_dma_bytes(const struct ice_vfio_dev *d);
size_t ice_dma_required_bytes(const struct ice_vfio_dev *d);
void alloc_queue_sw_state(struct ice_vfio_dev *d);
void dma_map(struct ice_vfio_dev *d, size_t size);
void layout_dma(struct ice_vfio_dev *d);
void pkt_pool_init(struct ice_vfio_dev *d);

static inline struct pkt_buf *pkt_pool_get_entry(const struct pkt_mempool *pool, uint32_t idx)
{
    return (struct pkt_buf *)(pool->base + (size_t)idx * pool->entry_size);
}

static inline uint64_t pkt_pool_get_data_iova(const struct pkt_mempool *pool, uint32_t idx)
{
    return pool->base_iova + (uint64_t)idx * pool->entry_size + offsetof(struct pkt_buf, data);
}

static inline uint32_t pkt_pool_pop_idx_noinit(struct pkt_mempool *pool)
{
    if (pool->free_count == 0)
        return ICE_PKT_BUF_INVALID_IDX;

    pool->free_count--;
    return pool->free_ring[pool->free_count];
}

static inline uint32_t pkt_pool_pop_idx(struct pkt_mempool *pool)
{
    return pkt_pool_pop_idx_noinit(pool);
}

static inline void pkt_pool_push_idx(struct pkt_mempool *pool, uint32_t idx)
{
    if (idx == ICE_PKT_BUF_INVALID_IDX || pool->free_count >= pool->num_entries)
        return;

    pool->free_ring[pool->free_count] = idx;
    pool->free_count++;
}

#endif

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
struct pkt_buf *pkt_buf_alloc(struct pkt_mempool *pool);
uint32_t pkt_buf_alloc_batch(struct pkt_mempool *pool, struct pkt_buf *bufs[],
                             uint32_t num_bufs);
uint32_t pkt_buf_alloc_batch_noinit(struct pkt_mempool *pool, struct pkt_buf *bufs[],
                                    uint32_t num_bufs);

static inline void pkt_buf_free_fast(struct pkt_buf *buf)
{
    struct pkt_mempool *pool;
    uint32_t tail;

    /*
     * Return a reflected pkt_buf to the shared pool with one free-ring push.
     * tx_update_free() calls this after hardware advances past a Tx descriptor that borrowed the buffer.
     */
    if (!buf || !buf->mempool)
        return;

    pool = buf->mempool;
    if (pool->free_count >= pool->num_entries)
        return;

    tail = pool->free_tail;
    pool->free_ring[tail] = buf->mempool_idx;
    tail++;
    if (tail == pool->num_entries)
        tail = 0;
    pool->free_tail = tail;
    pool->free_count++;
}

void pkt_buf_free(struct pkt_buf *buf);

#endif

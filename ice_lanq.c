#define _GNU_SOURCE
#include <endian.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ice_adminq.h"
#include "ice_controlq.h"
#include "ice_dma.h"
#include "ice_lanq.h"
#include "ice_regs.h"
#include "ice_utils.h"

#define ICE_RX_DESC_MASK (ICE_RX_DESC_COUNT - 1)
#define REFLECT_TIME_CHECK_BURSTS 1024U

#if (ICE_RX_DESC_COUNT & ICE_RX_DESC_MASK) != 0
#error "RX fast path assumes ICE_RX_DESC_COUNT is a power of two"
#endif

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

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

struct reflect_l2_ctx {
    uint8_t local_mac[ETHER_ADDR_LEN];
#if defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t local_mac01;
    uint32_t local_mac25;
#endif
};

static inline uint16_t rx_ring_add(uint16_t base, uint16_t add)
{
    uint16_t sum = (uint16_t)(base + add);

    if (sum >= ICE_RX_DESC_COUNT)
        return (uint16_t)(sum - ICE_RX_DESC_COUNT);
    return sum;
}

#if MY_ICE_RX_PREFETCH_WINDOW > 0
static inline uint16_t rx_ring_prefetch_idx(uint16_t idx)
{
    return rx_ring_add(idx, MY_ICE_RX_PREFETCH_WINDOW);
}
#endif

static inline uint16_t rx_ring_next(uint16_t idx)
{
    return (uint16_t)((idx + 1) & ICE_RX_DESC_MASK);
}

static inline uint16_t tx_ring_next(uint16_t idx, uint16_t desc_count)
{
    idx++;
    if (idx == desc_count)
        return 0;
    return idx;
}

static inline uint16_t tx_ring_clamp(uint16_t idx, uint16_t desc_count)
{
    if (idx >= desc_count)
        return (uint16_t)(idx - desc_count);
    return idx;
}

static inline struct pkt_buf *reflect_buf(const struct ice_vfio_dev *d, uint32_t idx)
{
    return pkt_pool_get_entry(&d->reflect_pool, idx);
}

static inline uint8_t *reflect_data(const struct ice_vfio_dev *d, uint32_t idx)
{
    return reflect_buf(d, idx)->data;
}

static inline uint64_t reflect_data_iova(const struct ice_vfio_dev *d, uint32_t idx)
{
    return pkt_pool_get_data_iova(&d->reflect_pool, idx);
}

static void maybe_pin_runtime_thread(const struct ice_vfio_dev *d, const char *context)
{
    int cpus[1024];
    int count;

#if !MY_ICE_ENABLE_INFO
    (void)context;
#endif

    if (!d->pin_cpus)
        return;

    count = build_cpu_list(cpus, (int)(sizeof(cpus) / sizeof(cpus[0])));
    if (count > 0) {
        pin_thread_to_cpu(cpus[0]);
        MY_ICE_INFO("[my_ice] %s: pinned runtime thread to CPU %d\n", context, cpus[0]);
    } else {
        MY_ICE_INFO("[my_ice] %s: failed to determine CPU affinity, running unpinned\n",
                    context);
    }
}

static int add_tx_queue(struct ice_vfio_dev *d)
{
    struct ice_aqc_add_tx_qgrp *qg;
    struct ice_aq_desc desc;
    struct txq_ctx *q = &d->txq;
    struct ice_tlan_ctx tlan = {0};
    int rc = -1;

    qg = calloc(1, sizeof(*qg));
    if (!qg)
        return -1;

    qg->parent_teid = htole32(d->io.qparent_teid);
    qg->num_txqs = 1;

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

    qg->txqs[0].txq_id = htole16(q->txq_id);
    set_ctx_bits((uint8_t *)&tlan, qg->txqs[0].txq_ctx, tlan_ctx_info);
    qg->txqs[0].info.valid_sections =
        ICE_AQC_ELEM_VALID_GENERIC | ICE_AQC_ELEM_VALID_CIR | ICE_AQC_ELEM_VALID_EIR;
    qg->txqs[0].info.generic = 0;
    qg->txqs[0].info.cir_bw.bw_profile_idx = htole16(ICE_SCHED_DFLT_RL_PROF_ID);
    qg->txqs[0].info.cir_bw.bw_alloc = htole16(ICE_SCHED_DFLT_BW_WT);
    qg->txqs[0].info.eir_bw.bw_profile_idx = htole16(ICE_SCHED_DFLT_RL_PROF_ID);
    qg->txqs[0].info.eir_bw.bw_alloc = htole16(ICE_SCHED_DFLT_BW_WT);

    fill_dflt_direct_desc(&desc, ICE_AQC_OPC_ADD_TXQS);
    desc.flags = htole16(le16toh(desc.flags) | ICE_AQ_FLAG_RD);
    desc.params.add_txqs.num_qgrps = 1;

    if (aq_send_cmd(d, &desc, qg, (uint16_t)sizeof(*qg)) == 0) {
        MY_ICE_INFO("[my_ice] ADD_TXQS resp: txq_id=%u q_teid=0x%x\n",
                    le16toh(qg->txqs[0].txq_id), le32toh(qg->txqs[0].q_teid));
        rc = 0;
    }

    free(qg);
    return rc;
}

static int wait_rxq_ready(struct ice_vfio_dev *d, uint16_t rxq, uint32_t *reg)
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

static inline void publish_rx_tail(struct ice_vfio_dev *d, uint16_t tail)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    reg_write32(d, d->io.rx_tail_reg, tail);
}

static int setup_and_enable_rxq_ctx_only(struct ice_vfio_dev *d)
{
    struct ice_rlan_ctx rlan = {0};
    uint8_t ctx_buf[ICE_RXQ_CTX_SZ] = {0};
    uint16_t q = d->io.rxq_id;
    uint32_t reg;
    int i;

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
    d->io.rx_tail_reg = QRX_TAIL(q);
    d->io.rx_tail = ICE_RX_DESC_COUNT - 1;
    d->io.rx_rearmed_pending = 0;
    publish_rx_tail(d, d->io.rx_tail);
    return 0;
}

static int setup_and_enable_rxq_pool(struct ice_vfio_dev *d)
{
    struct pkt_mempool *pool = &d->reflect_pool;
    uint32_t *rx_pkt_buf_idxs = d->io.rx_pkt_buf_idxs;
    int i;

    memset(d->io.rx_desc, 0, ICE_RX_DESC_COUNT * sizeof(*d->io.rx_desc));
    for (i = 0; i < ICE_RX_DESC_COUNT; i++)
        rx_pkt_buf_idxs[i] = ICE_PKT_BUF_INVALID_IDX;

    for (i = 0; i < ICE_RX_DESC_COUNT; i++) {
        uint32_t buf_idx = pkt_pool_pop_idx(pool);

        if (buf_idx == ICE_PKT_BUF_INVALID_IDX)
            return -1;
        rx_pkt_buf_idxs[i] = buf_idx;
        d->io.rx_desc[i].read.pkt_addr = htole64(reflect_data_iova(d, buf_idx));
        d->io.rx_desc[i].read.hdr_addr = 0;
        d->io.rx_desc[i].read.rsvd1 = 0;
        d->io.rx_desc[i].read.rsvd2 = 0;
    }

    return setup_and_enable_rxq_ctx_only(d);
}

static void tx_ring_init(struct ice_vfio_dev *d)
{
    struct txq_ctx *q = &d->txq;
    uint16_t i;

    memset(q->tx_desc, 0, (size_t)q->desc_count * sizeof(*q->tx_desc));
    for (i = 0; i < q->desc_count; i++)
        q->tx_pkt_buf_idxs[i] = ICE_PKT_BUF_INVALID_IDX;
    memset(q->tx_rsq, 0, (size_t)q->tx_rsq_count * sizeof(*q->tx_rsq));
    q->tx_next_to_use = 0;
    q->tx_next_to_clean = 0;
    q->tx_free = (uint16_t)(q->desc_count - 1);
    q->tx_pkts_since_rs = 0;
    q->tx_rsq_pidx = 0;
    q->tx_rsq_cidx = 0;
    q->dbell_reg = QTX_COMM_DBELL(q->txq_id);
    q->head_reg = QTX_COMM_HEAD(q->txq_id);
}

static int tx_init_reflect_slots(struct ice_vfio_dev *d)
{
    struct pkt_mempool *pool = &d->reflect_pool;
    struct txq_ctx *q = &d->txq;
    uint16_t idx;

    for (idx = 0; idx < q->desc_count; idx++) {
        uint32_t buf_idx = pkt_pool_pop_idx_noinit(pool);

        if (buf_idx == ICE_PKT_BUF_INVALID_IDX)
            return -1;
        q->tx_desc[idx].buf_addr = htole64(reflect_data_iova(d, buf_idx));
        q->tx_desc[idx].cmd_type_offset_bsz = 0;
        q->tx_pkt_buf_idxs[idx] = buf_idx;
    }

    return 0;
}

static void tx_update_free(struct ice_vfio_dev *d, struct txq_ctx *q)
{
    uint16_t head = (uint16_t)(reg_read32(d, q->head_reg) & 0x1FFFU);
    uint16_t ntu = q->tx_next_to_use;
    uint16_t used;

    head = tx_ring_clamp(head, q->desc_count);
    if (head == q->tx_next_to_clean)
        return;

    q->tx_next_to_clean = head;

    while (q->tx_rsq_cidx != q->tx_rsq_pidx) {
        uint16_t rs_slot = q->tx_rsq[q->tx_rsq_cidx];
        bool in_flight;

        if (head <= ntu)
            in_flight = rs_slot >= head && rs_slot < ntu;
        else
            in_flight = rs_slot >= head || rs_slot < ntu;

        if (in_flight)
            break;

        q->tx_rsq_cidx = tx_ring_next(q->tx_rsq_cidx, q->tx_rsq_count);
    }

    if (ntu >= head)
        used = (uint16_t)(ntu - head);
    else
        used = (uint16_t)(q->desc_count - (head - ntu));

    q->tx_free = (uint16_t)(q->desc_count - used - 1);
}

static inline void tx_record_rs_slot(struct txq_ctx *q, uint16_t idx)
{
    q->tx_rsq[q->tx_rsq_pidx] = idx;
    q->tx_rsq_pidx = tx_ring_next(q->tx_rsq_pidx, q->tx_rsq_count);
}

static inline uint16_t tx_reflect_enqueue_slot_trusted(struct ice_vfio_dev *d, struct txq_ctx *q,
                                                       uint32_t buf_idx, uint16_t len)
{
    struct ice_tx_desc *txd;
    struct pkt_mempool *pool = &d->reflect_pool;
    uint32_t old_buf_idx;
    uint16_t idx = q->tx_next_to_use;
    uint16_t cmd = ICE_TX_DESC_CMD_EOP;
    uint64_t qw1;

    q->tx_pkts_since_rs++;
    if (q->tx_pkts_since_rs >= TX_RS_THRESH) {
        cmd |= ICE_TX_DESC_CMD_RS;
        tx_record_rs_slot(q, idx);
        q->tx_pkts_since_rs = 0;
    }

    txd = &q->tx_desc[idx];
    txd->buf_addr = htole64(reflect_data_iova(d, buf_idx));
    qw1 = ((uint64_t)ICE_TX_DESC_DTYPE_DATA << ICE_TXD_QW1_DTYPE_S) |
          ((uint64_t)cmd << ICE_TXD_QW1_CMD_S) |
          ((uint64_t)len << ICE_TXD_QW1_TX_BUF_SZ_S);
    txd->cmd_type_offset_bsz = htole64(qw1);

    old_buf_idx = q->tx_pkt_buf_idxs[idx];
    if (unlikely(old_buf_idx == ICE_PKT_BUF_INVALID_IDX))
        die_msg("missing tx parked slot");
    pkt_pool_push_idx(pool, old_buf_idx);
    q->tx_pkt_buf_idxs[idx] = buf_idx;

    q->tx_free--;
    q->tx_next_to_use = tx_ring_next(idx, q->desc_count);
    return len;
}

static void tx_mark_rs_on_last(struct txq_ctx *q)
{
    struct ice_tx_desc *txd;
    volatile uint64_t *qw1_ptr;
    uint16_t last_idx;
    uint64_t qw1;

    if (q->tx_pkts_since_rs == 0)
        return;

    last_idx = (q->tx_next_to_use == 0) ? (uint16_t)(q->desc_count - 1)
                                        : (uint16_t)(q->tx_next_to_use - 1);
    txd = &q->tx_desc[last_idx];
    qw1_ptr = (volatile uint64_t *)&txd->cmd_type_offset_bsz;
    qw1 = le64toh(*qw1_ptr);
    qw1 |= (uint64_t)ICE_TX_DESC_CMD_RS << ICE_TXD_QW1_CMD_S;
    *qw1_ptr = htole64(qw1);
    tx_record_rs_slot(q, last_idx);
    q->tx_pkts_since_rs = 0;
}

static inline void tx_ring_doorbell(struct ice_vfio_dev *d, struct txq_ctx *q)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    reg_write32(d, q->dbell_reg, q->tx_next_to_use);
}

static void flush_reflect_tx_batch(struct ice_vfio_dev *d, struct txq_ctx *q,
                                   uint16_t *tx_pkts_pending_db)
{
    if (*tx_pkts_pending_db == 0)
        return;

    tx_mark_rs_on_last(q);
    tx_ring_doorbell(d, q);
    *tx_pkts_pending_db = 0;
}

static int tx_wait_drain(struct ice_vfio_dev *d, struct txq_ctx *q, int timeout_ms)
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

static int poll_and_rearm_reflect_batch(struct ice_vfio_dev *d, uint32_t completed_buf_idxs[],
                                        uint16_t completed_lens[], uint16_t max_count)
{
    struct pkt_mempool *pool = &d->reflect_pool;
    uint32_t *rx_pkt_buf_idxs = d->io.rx_pkt_buf_idxs;
    union ice_32b_rx_flex_desc *rx_desc = d->io.rx_desc;
    uint16_t count = max_count;
    uint16_t idx;
    uint16_t processed = 0;

    if (count == 0)
        return 0;

    idx = d->io.rx_ntc;
    while (processed < count) {
        union ice_32b_rx_flex_desc *rxd = &rx_desc[idx];
        uint16_t status0 = le16toh(rxd->wb.status_error0);
        uint16_t pkt_len = le16toh(rxd->wb.pkt_len);
        uint16_t next_idx;
        uint32_t replacement_buf_idx;
        uint32_t completed_buf_idx;

        if (!(status0 & (1U << ICE_RX_FLEX_DESC_STATUS0_DD_S)))
            break;
        if (!(status0 & (1U << ICE_RX_FLEX_DESC_STATUS0_EOF_S)) ||
            (status0 & (1U << ICE_RX_FLEX_DESC_STATUS0_RXE_S))) {
            fprintf(stderr,
                    "[my_ice] rx descriptor error idx=%u status0=0x%04x pkt_len=%u\n",
                    idx, status0, pkt_len);
            return -1;
        }

        next_idx = rx_ring_next(idx);
#if defined(__x86_64__) && MY_ICE_RX_PREFETCH_WINDOW > 0
        if ((uint16_t)(processed + MY_ICE_RX_PREFETCH_WINDOW) < count)
            __builtin_prefetch(&rx_desc[rx_ring_prefetch_idx(idx)], 0, 1);
#endif

        replacement_buf_idx = pkt_pool_pop_idx_noinit(pool);
        if (unlikely(replacement_buf_idx == ICE_PKT_BUF_INVALID_IDX)) {
            fprintf(stderr, "[my_ice] reflect pool underflow during immediate rearm\n");
            return -1;
        }

        completed_buf_idx = rx_pkt_buf_idxs[idx];
        rx_pkt_buf_idxs[idx] = replacement_buf_idx;
        rxd->read.pkt_addr = htole64(reflect_data_iova(d, replacement_buf_idx));
        rxd->read.hdr_addr = 0;
        rxd->read.rsvd1 = 0;
        rxd->read.rsvd2 = 0;
        completed_buf_idxs[processed] = completed_buf_idx;
        completed_lens[processed] = pkt_len;

        processed++;
        idx = next_idx;
    }

    d->io.rx_ntc = rx_ring_add(d->io.rx_ntc, processed);
    d->io.rx_rearmed_pending = (uint16_t)(d->io.rx_rearmed_pending + processed);
    return (int)processed;
}

static void flush_rx_rearmed_tail(struct ice_vfio_dev *d)
{
    uint16_t new_tail;

    if (d->io.rx_rearmed_pending == 0)
        return;

    new_tail = rx_ring_add(d->io.rx_tail, d->io.rx_rearmed_pending);
    publish_rx_tail(d, new_tail);
    d->io.rx_tail = new_tail;
    d->io.rx_rearmed_pending = 0;
}

static void dump_mdet_regs(struct ice_vfio_dev *d)
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

static inline void reflect_l2_ctx_init(struct reflect_l2_ctx *ctx, const uint8_t *local_mac)
{
    memcpy(ctx->local_mac, local_mac, ETHER_ADDR_LEN);
#if defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN
    memcpy(&ctx->local_mac01, local_mac, sizeof(ctx->local_mac01));
    memcpy(&ctx->local_mac25, local_mac + sizeof(ctx->local_mac01),
           sizeof(ctx->local_mac25));
#endif
}

static inline void rewrite_reflect_l2(uint8_t *data, const struct reflect_l2_ctx *ctx)
{
#if defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN
    uint64_t word0;
    uint64_t word1;
    uint64_t new_word0;

    memcpy(&word0, data, sizeof(word0));
    memcpy(&word1, data + sizeof(word0), sizeof(word1));

    new_word0 = (word0 >> 48) |
                ((word1 & UINT64_C(0x00000000ffffffff)) << 16) |
                ((uint64_t)ctx->local_mac01 << 48);
    word1 = (word1 & UINT64_C(0xffffffff00000000)) |
            (uint64_t)ctx->local_mac25;

    memcpy(data, &new_word0, sizeof(new_word0));
    memcpy(data + sizeof(new_word0), &word1, sizeof(word1));
#else
    uint8_t original_src[ETHER_ADDR_LEN];

    memcpy(original_src, data + ETHER_ADDR_LEN, ETHER_ADDR_LEN);
    memcpy(data, original_src, ETHER_ADDR_LEN);
    memcpy(data + ETHER_ADDR_LEN, ctx->local_mac, ETHER_ADDR_LEN);
#endif
}

int run_rx_reflect(struct ice_vfio_dev *d, int timeout_ms, uint16_t reflect_batch,
                   const struct ice_analysis_config *analysis)
{
    bool analysis_enabled = analysis && analysis->enabled;
    uint64_t analysis_next_id = 0;
    uint32_t analysis_warmup_accepted = 0;
    uint32_t analysis_accepted = 0;
    uint32_t rx_alloc;
    uint32_t tx_alloc;
#if MY_ICE_ENABLE_INFO
    uint16_t requested_reflect_batch = reflect_batch;
#endif
    uint16_t tx_doorbell_batch = REFLECT_TX_DOORBELL_BATCH;
    uint16_t tx_pkts_pending_db = 0;
    uint16_t rx_mac_rule_idx = UINT16_MAX;
    uint64_t rx_pkts = 0;
    uint64_t rx_bytes = 0;
    uint64_t tx_pkts = 0;
    uint64_t tx_bytes = 0;
    uint64_t rx_oversize_drops = 0;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t active_elapsed_ns;
    uint32_t time_check_countdown = 0;
    struct pkt_mempool *pool = &d->reflect_pool;
    struct reflect_l2_ctx l2_ctx;
    struct txq_ctx *q = &d->txq;
    uint32_t *completed_buf_idxs = NULL;
    uint16_t *completed_lens = NULL;
    int rc = -1;

    maybe_pin_runtime_thread(d, "rx-reflect");
    if (reflect_batch > MAX_REFLECT_BATCH)
        reflect_batch = MAX_REFLECT_BATCH;
    if (analysis_enabled && reflect_batch != REFLECT_TX_DOORBELL_BATCH) {
        fprintf(stderr,
                "[my_ice-analysis] analysis requires --reflect-batch %u so each accepted region includes its doorbell\n",
                REFLECT_TX_DOORBELL_BATCH);
        return -1;
    }
    d->reflect_batch = reflect_batch;

    completed_buf_idxs = calloc(reflect_batch, sizeof(*completed_buf_idxs));
    completed_lens = calloc(reflect_batch, sizeof(*completed_lens));
    if (!completed_buf_idxs || !completed_lens) {
        fprintf(stderr, "[my_ice] failed to allocate reflect scratch buffers for batch=%u\n",
                reflect_batch);
        goto out;
    }

    rx_alloc = reg_read32(d, PFLAN_RX_QALLOC);
    if (!(rx_alloc & PFLAN_RX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (RX=0x%08x)\n", rx_alloc);
        goto out;
    }
    d->io.rxq_id = (uint16_t)(rx_alloc & PFLAN_RX_QALLOC_FIRSTQ_M);

    tx_alloc = reg_read32(d, PFLAN_TX_QALLOC);
    if (!(tx_alloc & PFLAN_TX_QALLOC_VALID_M)) {
        fprintf(stderr, "queue alloc not valid (TX=0x%08x)\n", tx_alloc);
        goto out;
    }
    q->txq_id = (uint16_t)(tx_alloc & PFLAN_TX_QALLOC_FIRSTQ_M);

    if (aq_get_default_vsi_and_lport(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get default VSI/LPORT\n");
        goto out;
    }
    if (aq_get_qparent_teid(d) < 0) {
        fprintf(stderr, "[my_ice] failed to get parent TEID\n");
        goto out;
    }
    if (setup_and_enable_rxq_pool(d) < 0) {
        fprintf(stderr, "[my_ice] setup/enable rx queue failed\n");
        goto out;
    }
    if (aq_add_rx_mac_rule(d, &rx_mac_rule_idx) < 0) {
        fprintf(stderr, "[my_ice] failed to add RX MAC rule\n");
        goto out;
    }
    if (add_tx_queue(d) < 0) {
        fprintf(stderr, "[my_ice] add tx queue failed\n");
        goto out;
    }

    tx_ring_init(d);
    if (tx_init_reflect_slots(d) < 0) {
        fprintf(stderr, "[my_ice] reflect pool exhausted during tx descriptor setup\n");
        goto out;
    }

    if (tx_doorbell_batch > q->tx_free)
        tx_doorbell_batch = q->tx_free;

    MY_ICE_INFO(
        "[my_ice] rx-reflect on vsi=%u lport=%u rxq=%u txq=%u local-mac=%02x:%02x:%02x:%02x:%02x:%02x timeout_ms=%d reflect_batch=%u effective_reflect_batch=%u tx_doorbell_batch=%u\n",
        d->io.vsi_num, d->io.lport, d->io.rxq_id, q->txq_id,
        d->io.mac[0], d->io.mac[1], d->io.mac[2],
        d->io.mac[3], d->io.mac[4], d->io.mac[5], timeout_ms,
        requested_reflect_batch, reflect_batch, tx_doorbell_batch);

    reflect_l2_ctx_init(&l2_ctx, d->io.mac);
    start_ns = monotonic_ns();
    end_ns = start_ns + (uint64_t)timeout_ms * 1000000ULL;
    rc = 0;

    for (;;) {
        bool rx_seen = false;
        bool stalled = false;
        bool analysis_warmup = false;
        bool analysis_measured = false;
        uint64_t analysis_sample_id = 0;

        /*
         * A candidate must begin with no staged doorbell work.  Starting the
         * marker here keeps the capacity-refresh branch inside the region.
         */
        if (analysis_enabled && tx_pkts_pending_db == 0 &&
            q->tx_free >= reflect_batch && pool->free_count >= reflect_batch) {
            if (analysis_warmup_accepted < analysis->warmup_batches) {
                analysis_warmup = true;
            } else if (analysis_accepted < analysis->samples) {
                analysis_measured = true;
                analysis_sample_id = analysis_next_id++;
                ice_analysis_marker_begin(analysis_sample_id, reflect_batch);
            }
        }

        if (q->tx_free < reflect_batch)
            tx_update_free(d, q);

        if (q->tx_free == 0) {
            if (tx_pkts_pending_db != 0)
                flush_reflect_tx_batch(d, q, &tx_pkts_pending_db);
            stalled = true;
        } else if (pool->free_count == 0) {
            stalled = true;
        } else {
            int ready;
            uint16_t budget = reflect_batch;
            uint16_t sent = 0;
            uint64_t sent_bytes = 0;
            uint16_t i;

            if (q->tx_free < budget)
                budget = q->tx_free;
            if (pool->free_count < budget)
                budget = (uint16_t)pool->free_count;

            ready = poll_and_rearm_reflect_batch(d, completed_buf_idxs, completed_lens,
                                                 budget);
            if (ready < 0) {
                dump_mdet_regs(d);
                rc = -1;
                ready = 0;
            }

            if (ready != 0)
                rx_seen = true;

            for (i = 0; i < (uint16_t)ready; i++) {
                uint32_t completed_buf_idx = completed_buf_idxs[i];
                uint16_t completed_len = completed_lens[i];

                if (unlikely(!ice_reflect_pkt_len_fits(completed_len))) {
                    /* The Rx descriptor has already been rearmed; recycle only the old slot. */
                    pkt_pool_push_idx(pool, completed_buf_idx);
                    rx_oversize_drops++;
                    continue;
                }
                if (completed_len < 16) {
                    pkt_pool_push_idx(pool, completed_buf_idx);
                    continue;
                }

                rewrite_reflect_l2(reflect_data(d, completed_buf_idx), &l2_ctx);
                sent_bytes += tx_reflect_enqueue_slot_trusted(d, q, completed_buf_idx,
                                                              completed_len);
                sent++;
            }

            rx_pkts += sent;
            rx_bytes += sent_bytes;
            tx_pkts += sent;
            tx_bytes += sent_bytes;

            flush_rx_rearmed_tail(d);

            if (sent > 0) {
                tx_pkts_pending_db = (uint16_t)(tx_pkts_pending_db + sent);
                if (tx_pkts_pending_db >= tx_doorbell_batch)
                    flush_reflect_tx_batch(d, q, &tx_pkts_pending_db);
            }

            if (analysis_warmup || analysis_measured) {
                enum ice_analysis_outcome outcome = ICE_ANALYSIS_ACCEPTED;

                if (rc < 0)
                    outcome = ICE_ANALYSIS_REJECT_ERROR;
                else if (ready != (int)reflect_batch)
                    outcome = ICE_ANALYSIS_REJECT_PARTIAL_RX;
                else if (sent != reflect_batch)
                    outcome = ICE_ANALYSIS_REJECT_PACKET;
                else if (tx_pkts_pending_db != 0)
                    outcome = ICE_ANALYSIS_REJECT_PENDING_DOORBELL;

                if (analysis_measured) {
                    ice_analysis_marker_end(analysis_sample_id, reflect_batch, outcome);
                    if (outcome != ICE_ANALYSIS_ACCEPTED)
                        ice_analysis_marker_rejected(analysis_sample_id, outcome);
                    ice_analysis_record(stderr, analysis_sample_id, analysis_warmup_accepted,
                                        reflect_batch, outcome);
                    if (outcome == ICE_ANALYSIS_ACCEPTED)
                        analysis_accepted++;
                } else if (outcome == ICE_ANALYSIS_ACCEPTED) {
                    analysis_warmup_accepted++;
                }

                if (analysis_measured && outcome == ICE_ANALYSIS_ACCEPTED &&
                    analysis_accepted == analysis->samples)
                    break;
            }

            if (rc < 0)
                break;
        }

        if (!rx_seen && tx_pkts_pending_db != 0)
            flush_reflect_tx_batch(d, q, &tx_pkts_pending_db);

        if (time_check_countdown == 0 || stalled || !rx_seen) {
            uint64_t now_ns;

            time_check_countdown = REFLECT_TIME_CHECK_BURSTS;
            now_ns = monotonic_ns();
            if (now_ns >= end_ns)
                break;
        } else {
            time_check_countdown--;
        }

        if (stalled) {
            tx_update_free(d, q);
            usleep(50);
            continue;
        }

        if (!rx_seen)
            usleep(1000);
    }

    if (tx_pkts_pending_db != 0)
        flush_reflect_tx_batch(d, q, &tx_pkts_pending_db);
    /* Include final submitted packets, but exclude bounded completion cleanup. */
    active_elapsed_ns = monotonic_ns() - start_ns;

    flush_rx_rearmed_tail(d);
    (void)tx_wait_drain(d, q, 1000);
    tx_update_free(d, q);

    MY_ICE_REPORT(
        "[my_ice] rx-reflect done: seconds=%.6f tx_l2_gbps=%.6f rx_l2_gbps=%.6f tx_mpps=%.6f rx_mpps=%.6f oversize_drops=%llu\n",
        (double)active_elapsed_ns / (double)NS_PER_S,
        bytes_ns_to_gbps(tx_bytes, active_elapsed_ns),
        bytes_ns_to_gbps(rx_bytes, active_elapsed_ns),
        pkts_ns_to_mpps(tx_pkts, active_elapsed_ns),
        pkts_ns_to_mpps(rx_pkts, active_elapsed_ns),
        (unsigned long long)rx_oversize_drops);

out:
    free(completed_lens);
    free(completed_buf_idxs);
    aq_remove_sw_rule_best_effort(d, ICE_AQC_SW_RULES_T_LKUP_RX, rx_mac_rule_idx);
    return rc;
}

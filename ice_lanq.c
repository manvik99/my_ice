#define _GNU_SOURCE
#include <endian.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
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
#define REFLECT_TX_DOORBELL_BATCH 256U

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


static int add_tx_queues(struct ice_vfio_dev *d, uint16_t count)
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

static int enable_rxq(struct ice_vfio_dev *d)
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
    reg_write32(d, QRX_TAIL(q), ICE_RX_DESC_COUNT - 1);
    return 0;
}

static int setup_and_enable_rxq(struct ice_vfio_dev *d)
{
    int i;

    memset(d->io.rx_desc, 0, ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc));
    memset(d->io.rx_bufs, 0, ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE);

    for (i = 0; i < ICE_RX_DESC_COUNT; i++) {
        uint64_t biova = d->io.rx_bufs_iova + (uint64_t)i * ICE_RX_BUF_SIZE;
        d->io.rx_desc[i].read.pkt_addr = htole64(biova);
        d->io.rx_desc[i].read.hdr_addr = 0;
    }

    return enable_rxq(d);
}

static int setup_and_enable_rxq_pool(struct ice_vfio_dev *d)
{
    int i;

    memset(d->io.rx_desc, 0, ICE_RX_DESC_COUNT * sizeof(union ice_32b_rx_flex_desc));
    memset(d->io.rx_pkt_bufs, 0, ICE_RX_DESC_COUNT * sizeof(*d->io.rx_pkt_bufs));

    for (i = 0; i < ICE_RX_DESC_COUNT; i++) {
        struct pkt_buf *buf = pkt_buf_alloc(&d->reflect_pool);

        if (!buf)
            return -1;
        d->io.rx_pkt_bufs[i] = buf;
        d->io.rx_desc[i].read.pkt_addr = htole64(buf->buf_addr_iova);
        d->io.rx_desc[i].read.hdr_addr = 0;
    }

    return enable_rxq(d);
}

static void tx_ring_init(struct ice_vfio_dev *d)
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

static void tx_update_free(struct ice_vfio_dev *d, struct txq_ctx *q)
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

static int poll_rx_batch(struct ice_vfio_dev *d, uint16_t out_idxs[], uint16_t out_lens[],
                         uint16_t max_count)
{
    uint16_t idx;
    uint16_t count = 0;

    if (max_count == 0)
        return 0;

    idx = d->io.rx_ntc;
    while (count < max_count) {
        union ice_32b_rx_flex_desc *rxd = &d->io.rx_desc[idx];
        uint16_t status0 = le16toh(rxd->wb.status_error0);
        uint16_t pkt_len;
        uint16_t next_idx = (uint16_t)((idx + 1) & ICE_RX_DESC_MASK);

        if (likely(count + 1 < max_count))
            __builtin_prefetch(&d->io.rx_desc[next_idx], 0, 1);

        if (!(status0 & BIT(ICE_RX_FLEX_DESC_STATUS0_DD_S)))
            break;

        pkt_len = (uint16_t)(le16toh(rxd->wb.pkt_len) & ICE_RX_FLX_DESC_PKT_LEN_M);
        if (unlikely(!(status0 & BIT(ICE_RX_FLEX_DESC_STATUS0_EOF_S)) ||
            (status0 & BIT(ICE_RX_FLEX_DESC_STATUS0_RXE_S)) ||
            pkt_len == 0)) {
            fprintf(stderr,
                    "[my_ice] rx descriptor error idx=%u status0=0x%04x pkt_len=%u\n",
                    idx, status0, pkt_len);
            return -1;
        }

        out_idxs[count] = idx;
        out_lens[count] = pkt_len;
        count++;
        idx = next_idx;
    }

    return count;
}

static int poll_one_rx_desc(struct ice_vfio_dev *d, uint16_t *out_idx, uint16_t *out_len)
{
    uint16_t idx;
    uint16_t len;
    int count;

    count = poll_rx_batch(d, &idx, &len, 1);
    if (count <= 0)
        return count;

    *out_idx = idx;
    *out_len = len;
    return 1;
}

static int tx_try_reserve_slot(struct ice_vfio_dev *d, struct txq_ctx *q, uint16_t *out_idx)
{
    if (q->tx_free == 0) {
        tx_update_free(d, q);
        if (q->tx_free == 0)
            return 1;
    }

    *out_idx = q->tx_next_to_use;
    return 0;
}

static inline void tx_commit_slot(struct txq_ctx *q, uint16_t idx, struct pkt_buf *buf_ref)
{
    q->tx_pkt_buf_refs[idx] = buf_ref;
    q->tx_next_to_use = (uint16_t)((idx + 1) % q->desc_count);
    q->tx_free--;
}

static inline void tx_prepare_desc(struct txq_ctx *q, uint16_t idx, uint64_t buf_iova, uint16_t len)
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
static int tx_try_enqueue(struct ice_vfio_dev *d, struct txq_ctx *q, const uint8_t *pkt,
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

static uint16_t tx_reflect_enqueue_batch(struct ice_vfio_dev *d, struct txq_ctx *q,
                                         struct pkt_buf *bufs[], uint16_t num_bufs,
                                         uint64_t *bytes_sent)
{
    uint16_t desc_count = q->desc_count;
    uint16_t sent = 0;
    uint16_t idx = q->tx_next_to_use;
    uint64_t bytes = 0;

    if (q->tx_free < num_bufs) {
        tx_update_free(d, q);
        if (q->tx_free < num_bufs)
            num_bufs = q->tx_free;
    }

    while (sent < num_bufs) {
        struct pkt_buf *buf = bufs[sent];
        uint16_t len;

        len = (uint16_t)buf->size;
        tx_prepare_desc(q, idx, buf->buf_addr_iova, len);
        q->tx_pkt_buf_refs[idx] = buf;
        bytes += len;
        sent++;
        q->tx_free--;

        idx++;
        if (idx == desc_count)
            idx = 0;
    }

    q->tx_next_to_use = idx;
    *bytes_sent = bytes;
    return sent;
}

static void rearm_rx_desc_batch(struct ice_vfio_dev *d, const uint16_t idxs[], uint16_t count)
{
    uint16_t i;

    if (!count)
        return;

    for (i = 0; i < count; i++) {
        uint16_t idx = idxs[i];
        uint64_t buf_iova;

        if (d->io.rx_pkt_bufs && d->io.rx_pkt_bufs[idx]) {
            d->io.rx_pkt_bufs[idx]->size = 0;
            buf_iova = d->io.rx_pkt_bufs[idx]->buf_addr_iova;
        } else {
            buf_iova = d->io.rx_bufs_iova + (uint64_t)idx * ICE_RX_BUF_SIZE;
        }

        d->io.rx_desc[idx].read.pkt_addr = htole64(buf_iova);
        d->io.rx_desc[idx].read.hdr_addr = 0;
        d->io.rx_desc[idx].read.rsvd1 = 0;
        d->io.rx_desc[idx].read.rsvd2 = 0;
    }

    reg_write32(d, QRX_TAIL(d->io.rxq_id), idxs[count - 1]);
    d->io.rx_ntc = (uint16_t)((idxs[count - 1] + 1) & ICE_RX_DESC_MASK);
}

static void rearm_rx_desc_pool_batch(struct ice_vfio_dev *d, const uint16_t idxs[],
                                     struct pkt_buf *const bufs[], uint16_t count)
{
    union ice_32b_rx_flex_desc *rx_desc = d->io.rx_desc;
    uint16_t i;

    for (i = 0; i < count; i++) {
        uint16_t idx = idxs[i];
        struct pkt_buf *buf = bufs[i];

        rx_desc[idx].read.pkt_addr = htole64(buf->buf_addr_iova);
        rx_desc[idx].read.hdr_addr = 0;
        rx_desc[idx].read.rsvd1 = 0;
        rx_desc[idx].read.rsvd2 = 0;
    }

    reg_write32(d, QRX_TAIL(d->io.rxq_id), idxs[count - 1]);
    d->io.rx_ntc = (uint16_t)((idxs[count - 1] + 1) & ICE_RX_DESC_MASK);
}

static void rearm_rx_desc(struct ice_vfio_dev *d, uint16_t idx)
{
    uint16_t one = idx;

    rearm_rx_desc_batch(d, &one, 1);
}

static int poll_one_rx_packet(struct ice_vfio_dev *d, uint8_t *out, uint16_t out_sz, uint16_t *out_len)
{
    uint16_t idx;
    int got = poll_one_rx_desc(d, &idx, out_len);

    if (got <= 0)
        return got;

    if (*out_len > out_sz)
        *out_len = out_sz;

    memcpy(out, d->io.rx_bufs + ((size_t)idx * ICE_RX_BUF_SIZE), *out_len);
    rearm_rx_desc(d, idx);
    return 1;
}

static inline void tx_ring_doorbell(struct ice_vfio_dev *d, struct txq_ctx *q)
{
    __sync_synchronize();
    reg_write32(d, QTX_COMM_DBELL(q->txq_id), q->tx_next_to_use);
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

static void dump_rx_desc_snapshot(struct ice_vfio_dev *d)
{
    uint16_t i;

    for (i = 0; i < 8; i++) {
        uint16_t idx = (uint16_t)((d->io.rx_ntc + i) & ICE_RX_DESC_MASK);
        union ice_32b_rx_flex_desc *rxd = &d->io.rx_desc[idx];
        uint16_t status0 = le16toh(rxd->wb.status_error0);
        uint16_t pkt_len = (uint16_t)(le16toh(rxd->wb.pkt_len) & ICE_RX_FLX_DESC_PKT_LEN_M);
        fprintf(stderr,
                "[my_ice] rxdesc[%u] rxdid=%u status0=0x%04x pkt_len=%u\n",
                idx, rxd->wb.rxdid, status0, pkt_len);
    }
}

static uint64_t read_glv_counter64(struct ice_vfio_dev *d, uint32_t lo_off, uint32_t hi_off)
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

struct reflect_l2_ctx {
    uint8_t local_mac[ETHER_ADDR_LEN];
#if defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t local_mac01;
    uint32_t local_mac25;
#endif
};

static inline void reflect_l2_ctx_init(struct reflect_l2_ctx *ctx, const uint8_t *local_mac)
{
    memcpy(ctx->local_mac, local_mac, ETHER_ADDR_LEN);
#if defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN
    memcpy(&ctx->local_mac01, local_mac, sizeof(ctx->local_mac01));
    memcpy(&ctx->local_mac25, local_mac + sizeof(ctx->local_mac01),
           sizeof(ctx->local_mac25));
#endif
}

static inline void rewrite_reflect_l2(struct pkt_buf *buf, const struct reflect_l2_ctx *ctx)
{
#if defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t *data = buf->data;
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

    memcpy(original_src, buf->data + ETHER_ADDR_LEN, ETHER_ADDR_LEN);
    memcpy(buf->data, original_src, ETHER_ADDR_LEN);
    memcpy(buf->data + ETHER_ADDR_LEN, ctx->local_mac, ETHER_ADDR_LEN);
#endif
}

int run_rx_listen(struct ice_vfio_dev *d, int timeout_ms)
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

int run_rx_reflect(struct ice_vfio_dev *d, int timeout_ms, uint16_t reflect_batch)
{
    uint32_t rx_alloc;
    uint32_t tx_alloc;
    uint16_t first_q, last_q, avail_q;
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
    uint64_t start_ns, end_ns, now_ns;
    uint32_t time_check_countdown = 0;
    uint16_t tx_doorbell_batch = REFLECT_TX_DOORBELL_BATCH;
    uint16_t tx_pkts_pending_db = 0;
    struct reflect_l2_ctx l2_ctx;
    struct rx_reflect_metrics metrics;
    struct txq_ctx *q;
    int rc = -1;

    if (d->txq_count != 1) {
        fprintf(stderr, "[my_ice] rx-reflect uses one TX queue, using txq[0] only (configured=%u)\n",
                d->txq_count);
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

    first_q = (uint16_t)(tx_alloc & PFLAN_TX_QALLOC_FIRSTQ_M);
    last_q = (uint16_t)((tx_alloc & PFLAN_TX_QALLOC_LASTQ_M) >> PFLAN_TX_QALLOC_LASTQ_S);
    avail_q = (uint16_t)(last_q - first_q + 1);
    if (avail_q == 0) {
        fprintf(stderr, "no available TX queues (first=%u last=%u)\n", first_q, last_q);
        goto out;
    }
    d->txqs[0].txq_id = first_q;

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
    if (add_tx_queues(d, 1) < 0) {
        fprintf(stderr, "[my_ice] add tx queues failed\n");
        goto out;
    }

    tx_ring_init(d);
    q = &d->txqs[0];
    if (tx_doorbell_batch < reflect_batch)
        tx_doorbell_batch = reflect_batch;
    if (tx_doorbell_batch > q->tx_free)
        tx_doorbell_batch = q->tx_free;

    fprintf(stderr,
            "[my_ice] rx-reflect on vsi=%u lport=%u rxq=%u txq=%u local-mac=%02x:%02x:%02x:%02x:%02x:%02x timeout_ms=%d reflect_batch=%u tx_doorbell_batch=%u\n",
            d->io.vsi_num, d->io.lport, d->io.rxq_id, q->txq_id,
            d->io.mac[0], d->io.mac[1], d->io.mac[2],
            d->io.mac[3], d->io.mac[4], d->io.mac[5], timeout_ms, reflect_batch,
            tx_doorbell_batch);

    reflect_l2_ctx_init(&l2_ctx, d->io.mac);

    gorc_before = read_glv_counter64(d, GLV_GORCL(d->io.vsi_num),
                                     GLV_GORCH(d->io.vsi_num));
    gotc_before = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                     GLV_GOTCH(d->io.vsi_num));
    start_ns = monotonic_ns();
    end_ns = start_ns + (uint64_t)timeout_ms * 1000000ULL;
    last_report_ns = start_ns;
    next_report_ns = start_ns + NS_PER_S;

    while (true) {
        uint16_t rx_idxs[MAX_REFLECT_BATCH];
        uint16_t rx_lens[MAX_REFLECT_BATCH];
        struct pkt_buf *replacement_bufs[MAX_REFLECT_BATCH];
        struct pkt_buf *tx_bufs[MAX_REFLECT_BATCH];
        uint64_t batch_rx_bytes = 0;
        uint64_t batch_tx_bytes = 0;
        bool rx_seen = false;
        bool stalled = false;
        uint16_t budget;
        int got;
        uint16_t tx_count = 0;
        uint16_t replacement_count;
        uint16_t sent = 0;
        uint16_t i;

        if (q->tx_free < reflect_batch || d->reflect_pool.free_count < reflect_batch)
            tx_update_free(d, q);

        budget = reflect_batch;
        if (q->tx_free < budget)
            budget = q->tx_free;
        if (d->reflect_pool.free_count < budget)
            budget = (uint16_t)d->reflect_pool.free_count;

        if (budget == 0) {
            if (tx_pkts_pending_db != 0) {
                tx_ring_doorbell(d, q);
                doorbells++;
                tx_pkts_pending_db = 0;
            }
            if (q->tx_free == 0)
                tx_ring_full++;
            if (d->reflect_pool.free_count == 0)
                pool_empty++;
            stalled = true;
            goto report_progress;
        }

        got = poll_rx_batch(d, rx_idxs, rx_lens, budget);
        if (unlikely(got < 0)) {
            rx_errors++;
            dump_mdet_regs(d);
            goto out;
        }
        if (got == 0)
            goto report_progress;

        rx_seen = true;

        replacement_count = (uint16_t)pkt_buf_alloc_batch_noinit(&d->reflect_pool,
                                                                 replacement_bufs,
                                                                 (uint16_t)got);
        if (unlikely(replacement_count != (uint16_t)got)) {
            fprintf(stderr,
                    "[my_ice] reflect pool underflow: needed=%u got=%u free_count=%u\n",
                    (uint16_t)got, replacement_count, d->reflect_pool.free_count);
            rx_errors++;
            goto out;
        }

        for (i = 0; i < (uint16_t)got; i++) {
            uint16_t rx_idx = rx_idxs[i];
            uint16_t rx_len = rx_lens[i];
            struct pkt_buf *rx_buf = d->io.rx_pkt_bufs[rx_idx];

            if (unlikely(!rx_buf)) {
                fprintf(stderr, "[my_ice] missing rx pool buffer for descriptor %u\n", rx_idx);
                rx_errors++;
                goto out;
            }

            d->io.rx_pkt_bufs[rx_idx] = replacement_bufs[i];

            if (rx_len < 14) {
                rx_short++;
                pkt_buf_free(rx_buf);
                continue;
            }

            rx_buf->size = rx_len;
            rewrite_reflect_l2(rx_buf, &l2_ctx);

            tx_bufs[tx_count] = rx_buf;
            batch_rx_bytes += rx_len;
            tx_count++;
        }

        rearm_rx_desc_pool_batch(d, rx_idxs, replacement_bufs, (uint16_t)got);

        rx_pkts += tx_count;
        rx_bytes += batch_rx_bytes;

        if (tx_count > 0)
            sent = tx_reflect_enqueue_batch(d, q, tx_bufs, tx_count, &batch_tx_bytes);

        if (sent > 0) {
            tx_pkts += sent;
            tx_bytes += batch_tx_bytes;
            zero_copy_pkts += sent;
            zero_copy_bytes += batch_tx_bytes;
            tx_pkts_pending_db = (uint16_t)(tx_pkts_pending_db + sent);
            if (tx_pkts_pending_db >= tx_doorbell_batch || sent < tx_count) {
                tx_ring_doorbell(d, q);
                doorbells++;
                tx_pkts_pending_db = 0;
            }
        }

        if (sent < tx_count) {
            for (i = sent; i < tx_count; i++)
                pkt_buf_free(tx_bufs[i]);
            tx_ring_full++;
            stalled = true;
        }

report_progress:
        if (!rx_seen && tx_pkts_pending_db != 0) {
            tx_ring_doorbell(d, q);
            doorbells++;
            tx_pkts_pending_db = 0;
        }
        if (time_check_countdown == 0 || stalled || !rx_seen) {
            time_check_countdown = REFLECT_TIME_CHECK_BURSTS;
            now_ns = monotonic_ns();
            if (now_ns >= end_ns)
                break;
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

                last_report_ns = now_ns;
                prev_rx_pkts = rx_pkts;
                prev_rx_bytes = rx_bytes;
                prev_tx_pkts = tx_pkts;
                prev_tx_bytes = tx_bytes;
                next_report_ns += NS_PER_S;
                if (next_report_ns < now_ns)
                    next_report_ns = now_ns + NS_PER_S;
            }
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

    if (tx_pkts_pending_db != 0) {
        tx_ring_doorbell(d, q);
        doorbells++;
        tx_pkts_pending_db = 0;
    }
    (void)tx_wait_drain(d, q, 1000);
    now_ns = end_ns;
    gorc_after = read_glv_counter64(d, GLV_GORCL(d->io.vsi_num),
                                    GLV_GORCH(d->io.vsi_num));
    gotc_after = read_glv_counter64(d, GLV_GOTCL(d->io.vsi_num),
                                    GLV_GOTCH(d->io.vsi_num));
    metrics.seconds_total = (double)(now_ns - start_ns) / 1e9;
    metrics.tx_wire_gbps =
        bytes_ns_to_gbps(l2_bytes_to_wire_bytes(tx_pkts, tx_bytes), now_ns - start_ns);
    metrics.rx_wire_gbps =
        bytes_ns_to_gbps(l2_bytes_to_wire_bytes(rx_pkts, rx_bytes), now_ns - start_ns);
    metrics.tx_mpps = pkts_ns_to_mpps(tx_pkts, now_ns - start_ns);
    metrics.rx_mpps = pkts_ns_to_mpps(rx_pkts, now_ns - start_ns);
    metrics.tx_l2_gbps = bytes_ns_to_gbps(tx_bytes, now_ns - start_ns);
    metrics.rx_l2_gbps = bytes_ns_to_gbps(rx_bytes, now_ns - start_ns);
    metrics.rx_pkts = rx_pkts;
    metrics.rx_bytes = rx_bytes;
    metrics.tx_pkts = tx_pkts;
    metrics.tx_bytes = tx_bytes;
    metrics.zero_copy_pkts = zero_copy_pkts;
    metrics.zero_copy_bytes = zero_copy_bytes;
    metrics.tx_ring_full = tx_ring_full;
    metrics.rx_short = rx_short;
    metrics.rx_errors = rx_errors;
    metrics.pool_empty = pool_empty;
    metrics.doorbells = doorbells;
    metrics.vsi_num = d->io.vsi_num;
    metrics.reflect_batch = reflect_batch;
    metrics.gorc_delta = gorc_after - gorc_before;
    metrics.gotc_delta = gotc_after - gotc_before;
    fprintf(stderr,
            "[my_ice] rx-reflect done: seconds=%.3f TX=%.3f wire-Gbps RX=%.3f wire-Gbps"
            " tx_mpps=%.3f rx_mpps=%.3f tx_l2_gbps=%.3f rx_l2_gbps=%.3f"
            " rx_pkts=%" PRIu64 " rx_bytes=%" PRIu64
            " tx_pkts=%" PRIu64 " tx_bytes=%" PRIu64
            " zero_copy_pkts=%" PRIu64 " zero_copy_bytes=%" PRIu64
            " tx_ring_full=%" PRIu64 " rx_short=%" PRIu64 " rx_errors=%" PRIu64
            " pool_empty=%" PRIu64 " doorbells=%" PRIu64
            " VSI%u GORC_delta=%" PRIu64 " GOTC_delta=%" PRIu64 "\n",
            metrics.seconds_total,
            metrics.tx_wire_gbps,
            metrics.rx_wire_gbps,
            metrics.tx_mpps,
            metrics.rx_mpps,
            metrics.tx_l2_gbps,
            metrics.rx_l2_gbps,
            rx_pkts, rx_bytes, tx_pkts, tx_bytes, zero_copy_pkts, zero_copy_bytes,
            tx_ring_full, rx_short, rx_errors, pool_empty, doorbells, d->io.vsi_num,
            metrics.gorc_delta, metrics.gotc_delta);

    if (write_rx_reflect_metrics_log(d, &metrics) < 0)
        goto out;

    rc = 0;

out:
    aq_remove_sw_rule_best_effort(d, ICE_AQC_SW_RULES_T_LKUP_RX, rx_mac_rule_idx);
    return rc;
}

int run_tx_send(struct ice_vfio_dev *d, const uint8_t *dst_mac, int count,
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
            d->io.vsi_num, gotc_after - gotc_before);
    return 0;
}

struct tx_bench_worker {
    struct ice_vfio_dev *d;
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

int run_tx_bench(struct ice_vfio_dev *d, const uint8_t *dst_mac, int seconds,
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

#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ice_adminq.h"
#include "ice_regs.h"
#include "ice_utils.h"

static bool g_dump_topo = false;
static bool g_qparent_override_set = false;
static uint32_t g_qparent_override = 0;

void aq_set_topology_options(bool dump_topo, bool qparent_override_set,
                             uint32_t qparent_override)
{
    g_dump_topo = dump_topo;
    g_qparent_override_set = qparent_override_set;
    g_qparent_override = qparent_override;
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


void fill_dflt_direct_desc(struct ice_aq_desc *desc, uint16_t opcode)
{
    memset(desc, 0, sizeof(*desc));
    desc->opcode = htole16(opcode);
    desc->flags = htole16(ICE_AQ_FLAG_SI);
}

void adminq_hw_init(struct ice_vfio_dev *d)
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

int aq_send_cmd(struct ice_vfio_dev *d, struct ice_aq_desc *desc, void *buf, uint16_t buf_size)
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

int aq_get_fw_ver(struct ice_vfio_dev *d)
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

int aq_manage_mac_read(struct ice_vfio_dev *d)
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

int aq_get_default_vsi_and_lport(struct ice_vfio_dev *d)
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

int aq_get_qparent_teid(struct ice_vfio_dev *d)
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

int aq_add_rx_mac_rule(struct ice_vfio_dev *d, uint16_t *rule_idx)
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

void aq_remove_sw_rule_best_effort(struct ice_vfio_dev *d, uint16_t rule_type,
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

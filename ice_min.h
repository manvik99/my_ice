#ifndef MY_ICE_MIN_H
#define MY_ICE_MIN_H

#include <stddef.h>
#include <stdint.h>

#define BIT(n) (1U << (n))
#define BIT_ULL(n) (1ULL << (n))

#define ETHER_ADDR_LEN 6
#define STRUCT_HACK_VAR_LEN 1

/* AdminQ sizing */
#define ICE_AQ_MAX_BUF_LEN 4096
#define ICE_AQ_LG_BUF 512
#define ICE_AQ_NUM_DESC 64

/* Datapath demo sizing */
#define ICE_TX_DESC_COUNT 128
#define ICE_RX_DESC_COUNT 128
#define ICE_RX_BUF_SIZE 2048
#define ICE_TX_PKT_BUF_SIZE 2048

/* PF FW AdminQ registers (BAR0 offsets) */
#define PF_FW_ATQBAL                0x00080000
#define PF_FW_ATQBAH                0x00080100
#define PF_FW_ATQLEN                0x00080200
#define PF_FW_ATQH                  0x00080300
#define PF_FW_ATQT                  0x00080400

#define PF_FW_ARQBAL                0x00080080
#define PF_FW_ARQBAH                0x00080180
#define PF_FW_ARQLEN                0x00080280
#define PF_FW_ARQH                  0x00080380
#define PF_FW_ARQT                  0x00080480

#define PF_FW_ATQLEN_ATQENABLE_M    BIT(31)
#define PF_FW_ARQLEN_ARQENABLE_M    BIT(31)

/* Queue allocation + queue register space */
#define PFLAN_RX_QALLOC             0x001D2500
#define PFLAN_RX_QALLOC_FIRSTQ_M    0x000007FFU
#define PFLAN_RX_QALLOC_LASTQ_S     16
#define PFLAN_RX_QALLOC_LASTQ_M     0x07FF0000U
#define PFLAN_RX_QALLOC_VALID_M     BIT(31)

#define PFLAN_TX_QALLOC             0x001D2580
#define PFLAN_TX_QALLOC_FIRSTQ_M    0x00003FFFU
#define PFLAN_TX_QALLOC_LASTQ_S     16
#define PFLAN_TX_QALLOC_LASTQ_M     0x3FFF0000U
#define PFLAN_TX_QALLOC_VALID_M     BIT(31)

#define QRX_CONTEXT(i, q)           (0x00280000 + ((i) * 8192U) + ((q) * 4U))
#define QRX_CTRL(q)                 (0x00120000 + ((q) * 4U))
#define QRX_TAIL(q)                 (0x00290000 + ((q) * 4U))
#define QRXFLXP_CNTXT(q)            (0x00480000 + ((q) * 4U))
#define QTX_COMM_DBELL(q)           (0x002C0000 + ((q) * 4U))
#define QTX_COMM_HEAD(q)            (0x000E0000 + ((q) * 4U))

#define QRX_CTRL_QENA_REQ_M         BIT(0)
#define QRX_CTRL_QENA_STAT_M        BIT(2)
#define QRXFLXP_CNTXT_RXDID_IDX_M   0x3FU
#define QRXFLXP_CNTXT_RXDID_IDX_S   0
#define QRXFLXP_CNTXT_RXDID_PRIO_M  (0x7U << 8)
#define QRXFLXP_CNTXT_RXDID_PRIO_S  8

/* MDD event registers */
#define GL_MDET_TX_TCLAN            0x000FC068
#define GL_MDET_TX_TCLAN_VALID_M    BIT(31)
#define GL_MDET_TX_PQM              0x002D2E00
#define GL_MDET_TX_PQM_VALID_M      BIT(31)
#define GL_MDET_RX                  0x00294C00
#define GL_MDET_RX_VALID_M          BIT(31)
#define PF_MDET_TX_TCLAN            0x000FC000
#define PF_MDET_TX_TCLAN_VALID_M    BIT(0)
#define PF_MDET_TX_PQM              0x002D2C80
#define PF_MDET_TX_PQM_VALID_M      BIT(0)
#define PF_MDET_RX                  0x00294280
#define PF_MDET_RX_VALID_M          BIT(0)

/* Per-VSI traffic counters */
#define GLV_GOTCL(vsi)              (0x00300000 + ((vsi) * 8U))
#define GLV_GOTCH(vsi)              (0x00300004 + ((vsi) * 8U))
#define GLV_GORCL(vsi)              (0x003B0000 + ((vsi) * 8U))
#define GLV_GORCH(vsi)              (0x003B0004 + ((vsi) * 8U))

/* AQ flags */
#define ICE_AQ_FLAG_LB              BIT(9)
#define ICE_AQ_FLAG_RD              BIT(10)
#define ICE_AQ_FLAG_BUF             BIT(12)
#define ICE_AQ_FLAG_SI              BIT(13)

/* AQ opcodes */
#define ICE_AQC_OPC_GET_VER         0x0001
#define ICE_AQC_OPC_MANAGE_MAC_READ 0x0107
#define ICE_AQC_OPC_GET_SW_CFG      0x0200
#define ICE_AQC_OPC_ADD_SW_RULES    0x02A0
#define ICE_AQC_OPC_REMOVE_SW_RULES 0x02A2
#define ICE_AQC_OPC_GET_DFLT_TOPO   0x0400
#define ICE_AQC_OPC_SET_MAC_LB      0x0620
#define ICE_AQC_OPC_SET_PHY_LB      0x0619
#define ICE_AQC_OPC_UPDATE_VSI      0x0211
#define ICE_AQC_OPC_SET_RSS_KEY     0x0B02
#define ICE_AQC_OPC_SET_RSS_LUT     0x0B03
#define ICE_AQC_OPC_ADD_TXQS        0x0C30

/* Manage MAC read */
#define ICE_AQC_MAN_MAC_LAN_ADDR_VALID   BIT(4)
#define ICE_AQC_MAN_MAC_READ_S           4
#define ICE_AQC_MAN_MAC_READ_M           (0xFU << ICE_AQC_MAN_MAC_READ_S)
#define ICE_AQC_MAN_MAC_ADDR_TYPE_LAN    0

/* Get switch config element parsing */
#define ICE_AQC_GET_SW_CONF_RESP_VSI_PORT_NUM_M 0x03FFU
#define ICE_AQC_GET_SW_CONF_RESP_TYPE_S         14
#define ICE_AQC_GET_SW_CONF_RESP_TYPE_M         0xC000U
#define ICE_AQC_GET_SW_CONF_RESP_PHYS_PORT      0
#define ICE_AQC_GET_SW_CONF_RESP_VIRT_PORT      1
#define ICE_AQC_GET_SW_CONF_RESP_VSI            2
#define ICE_AQC_GET_SW_CONF_RESP_FUNC_NUM_M     0x7FFFU
#define ICE_AQC_GET_SW_CONF_RESP_IS_VF          BIT(15)

/* Switch rules */
#define ICE_DUMMY_ETH_HDR_LEN                16
#define ICE_AQC_SW_RULES_T_LKUP_RX           0x0
#define ICE_AQC_SW_RULES_T_LKUP_TX           0x1
#define ICE_SW_LKUP_MAC                      1
#define ICE_SW_LKUP_PROMISC                  3
#define ICE_SINGLE_ACT_VSI_FORWARDING        0x0
#define ICE_SINGLE_ACT_VSI_ID_S              4
#define ICE_SINGLE_ACT_VSI_ID_M              (0x3FFU << ICE_SINGLE_ACT_VSI_ID_S)
#define ICE_SINGLE_ACT_LB_ENABLE             BIT(2)
#define ICE_SINGLE_ACT_LAN_ENABLE            BIT(3)
#define ICE_SINGLE_ACT_VALID_BIT             BIT(17)

/* Scheduler / Add TxQ */
#define ICE_AQC_TOPO_MAX_LEVEL_NUM       9
#define ICE_AQC_ELEM_TYPE_LEAF           0x5
#define ICE_AQC_ELEM_VALID_GENERIC       BIT(0)
#define ICE_AQC_ELEM_VALID_CIR           BIT(1)
#define ICE_AQC_ELEM_VALID_EIR           BIT(2)
#define ICE_SCHED_DFLT_RL_PROF_ID        0
#define ICE_SCHED_DFLT_BW_WT             4

#define ICE_AQ_MAC_LB_EN                 BIT(0)

/* Rx context / descriptor constants */
#define ICE_RXDID_FLEX_NIC               2
#define ICE_RX_DTYPE_NO_SPLIT            0
#define ICE_RLAN_CTX_DBUF_S              7
#define ICE_RXQ_CTX_SIZE_DWORDS          8
#define ICE_RXQ_CTX_SZ                   (ICE_RXQ_CTX_SIZE_DWORDS * sizeof(uint32_t))

#define ICE_RX_FLX_DESC_PKT_LEN_M        0x3FFF
#define ICE_RX_FLEX_DESC_STATUS0_DD_S    0
#define ICE_RX_FLEX_DESC_STATUS0_EOF_S   1
#define ICE_RX_FLEX_DESC_STATUS0_RXE_S   10

/* Tx descriptor constants */
#define ICE_TX_DESC_DTYPE_DATA           0x0
#define ICE_TX_DESC_DTYPE_DESC_DONE      0xF
#define ICE_TXD_QW1_DTYPE_S              0
#define ICE_TXD_QW1_CMD_S                4
#define ICE_TXD_QW1_TX_BUF_SZ_S          34
#define ICE_TX_DESC_CMD_EOP              0x0001
#define ICE_TX_DESC_CMD_RS               0x0002

struct ice_aqc_generic {
    uint32_t param0;
    uint32_t param1;
    uint32_t addr_high;
    uint32_t addr_low;
};

struct ice_aqc_get_ver {
    uint32_t rom_ver;
    uint32_t fw_build;
    uint8_t fw_branch;
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
    uint8_t api_branch;
    uint8_t api_major;
    uint8_t api_minor;
    uint8_t api_patch;
};

struct ice_aqc_manage_mac_read {
    uint16_t flags;
    uint8_t rsvd[2];
    uint8_t num_addr;
    uint8_t rsvd1[3];
    uint32_t addr_high;
    uint32_t addr_low;
};

struct ice_aqc_manage_mac_read_resp {
    uint8_t lport_num;
    uint8_t addr_type;
    uint8_t mac_addr[ETHER_ADDR_LEN];
};

struct ice_aqc_get_sw_cfg {
    uint16_t flags;
    uint16_t element;
    uint16_t num_elems;
    uint16_t rsvd;
    uint32_t addr_high;
    uint32_t addr_low;
};

struct ice_aqc_get_sw_cfg_resp_elem {
    uint16_t vsi_port_num;
    uint16_t swid;
    uint16_t pf_vf_num;
} __attribute__((packed));

struct ice_aqc_get_topo {
    uint8_t port_num;
    uint8_t num_branches;
    uint16_t reserved1;
    uint32_t reserved2;
    uint32_t addr_high;
    uint32_t addr_low;
};

struct ice_aqc_elem_info_bw {
    uint16_t bw_profile_idx;
    uint16_t bw_alloc;
};

struct ice_aqc_txsched_elem {
    uint8_t elem_type;
    uint8_t valid_sections;
    uint8_t generic;
    uint8_t flags;
    struct ice_aqc_elem_info_bw cir_bw;
    struct ice_aqc_elem_info_bw eir_bw;
    uint16_t srl_id;
    uint16_t reserved2;
} __attribute__((packed));

struct ice_aqc_txsched_elem_data {
    uint32_t parent_teid;
    uint32_t node_teid;
    struct ice_aqc_txsched_elem data;
} __attribute__((packed));

struct ice_aqc_txsched_topo_grp_info_hdr {
    uint32_t parent_teid;
    uint16_t num_elems;
    uint16_t reserved2;
} __attribute__((packed));

struct ice_aqc_get_topo_elem {
    struct ice_aqc_txsched_topo_grp_info_hdr hdr;
    struct ice_aqc_txsched_elem_data generic[ICE_AQC_TOPO_MAX_LEVEL_NUM];
} __attribute__((packed));

struct ice_aqc_set_mac_lb {
    uint8_t lb_mode;
    uint8_t reserved[15];
};

struct ice_aqc_set_phy_lb {
    uint8_t lport_num;
    uint8_t lport_num_valid;
#define ICE_AQ_PHY_LB_PORT_NUM_VALID BIT(0)
    uint8_t phy_index;
    uint8_t lb_mode;
#define ICE_AQ_PHY_LB_EN             BIT(0)
#define ICE_AQ_PHY_LB_TYPE_M         BIT(1)
#define ICE_AQ_PHY_LB_TYPE_LOCAL     0
#define ICE_AQ_PHY_LB_TYPE_REMOTE    ICE_AQ_PHY_LB_TYPE_M
#define ICE_AQ_PHY_LB_LEVEL_M        BIT(2)
#define ICE_AQ_PHY_LB_LEVEL_PMD      0
#define ICE_AQ_PHY_LB_LEVEL_PCS      ICE_AQ_PHY_LB_LEVEL_M
    uint8_t reserved2[12];
};

struct ice_aqc_add_txqs {
    uint8_t num_qgrps;
    uint8_t reserved[3];
    uint32_t reserved1;
    uint32_t addr_high;
    uint32_t addr_low;
};

struct ice_aqc_sw_rules {
    uint16_t num_rules_fltr_entry_index;
    uint8_t reserved[6];
    uint32_t addr_high;
    uint32_t addr_low;
};

struct ice_aqc_add_txqs_perq {
    uint16_t txq_id;
    uint8_t rsvd[2];
    uint32_t q_teid;
    uint8_t txq_ctx[22];
    uint8_t rsvd2[2];
    struct ice_aqc_txsched_elem info;
} __attribute__((packed));

struct ice_aqc_add_tx_qgrp {
    uint32_t parent_teid;
    uint8_t num_txqs;
    uint8_t rsvd[3];
    struct ice_aqc_add_txqs_perq txqs[STRUCT_HACK_VAR_LEN];
} __attribute__((packed));

/* Add/Get/Update/Free VSI (0x0210/0x0212/0x0211/0x0213) */
struct ice_aqc_vsi_cmd {
    uint16_t vsi_num;
#define ICE_AQ_VSI_IS_VALID             BIT(15)
    uint16_t cmd_flags;
    uint8_t vf_id;
    uint8_t reserved;
    uint16_t vsi_flags;
    uint32_t addr_high;
    uint32_t addr_low;
};

/* Get/Set RSS key (indirect 0x0B04/0x0B02) */
struct ice_aqc_get_set_rss_key_cmd {
    uint16_t vsi_id;
    uint8_t reserved2[6];
    uint32_t addr_high;
    uint32_t addr_low;
};

/* Get/Set RSS LUT (indirect 0x0B05/0x0B03) */
struct ice_aqc_get_set_rss_lut_cmd {
    uint16_t vsi_id;
    uint16_t flags;
    uint32_t reserved2;
    uint32_t addr_high;
    uint32_t addr_low;
};

struct ice_aq_desc {
    uint16_t flags;
    uint16_t opcode;
    uint16_t datalen;
    uint16_t retval;
    uint32_t cookie_high;
    uint32_t cookie_low;
    union {
        uint8_t raw[16];
        struct ice_aqc_generic generic;
        struct ice_aqc_get_ver get_ver;
        struct ice_aqc_manage_mac_read mac_read;
        struct ice_aqc_get_sw_cfg get_sw_conf;
        struct ice_aqc_sw_rules sw_rules;
        struct ice_aqc_get_topo get_topo;
        struct ice_aqc_set_phy_lb set_phy_lb;
        struct ice_aqc_set_mac_lb set_mac_lb;
        struct ice_aqc_add_txqs add_txqs;
        struct ice_aqc_vsi_cmd vsi_cmd;
        struct ice_aqc_get_set_rss_key_cmd get_set_rss_key;
        struct ice_aqc_get_set_rss_lut_cmd get_set_rss_lut;
    } params;
};

union ice_32b_rx_flex_desc {
    struct {
        uint64_t pkt_addr;
        uint64_t hdr_addr;
        uint64_t rsvd1;
        uint64_t rsvd2;
    } read;
    struct {
        uint8_t rxdid;
        uint8_t mir_id_umb_cast;
        uint16_t ptype_flex_flags0;
        uint16_t pkt_len;
        uint16_t hdr_len_sph_flex_flags1;
        uint16_t status_error0;
        uint16_t l2tag1;
        uint32_t rss_hash;
        uint16_t status_error1;
        uint8_t flex_flags2;
        uint8_t ts_low;
        uint16_t l2tag2_1st;
        uint16_t l2tag2_2nd;
        uint32_t flow_id;
        union {
            struct {
                uint16_t rsvd;
                uint16_t flow_id_ipv6;
            } flex;
            uint32_t ts_high;
        } flex_ts;
    } wb;
};

struct ice_tx_desc {
    uint64_t buf_addr;
    uint64_t cmd_type_offset_bsz;
};

struct ice_rlan_ctx {
    uint16_t head;
    uint16_t cpuid;
    uint64_t base;
    uint16_t qlen;
    uint16_t dbuf;
    uint16_t hbuf;
    uint8_t dtype;
    uint8_t dsize;
    uint8_t crcstrip;
    uint8_t l2tsel;
    uint8_t hsplit_0;
    uint8_t hsplit_1;
    uint8_t showiv;
    uint32_t rxmax;
    uint8_t tphrdesc_ena;
    uint8_t tphwdesc_ena;
    uint8_t tphdata_ena;
    uint8_t tphhead_ena;
    uint16_t lrxqthresh;
    uint8_t prefena;
};

struct ice_tlan_ctx {
    uint64_t base;
    uint8_t port_num;
    uint16_t cgd_num;
    uint8_t pf_num;
    uint16_t vmvf_num;
    uint8_t vmvf_type;
#define ICE_TLAN_CTX_VMVF_TYPE_PF 2
    uint16_t src_vsi;
    uint8_t tsyn_ena;
    uint8_t internal_usage_flag;
    uint8_t alt_vlan;
    uint16_t cpuid;
    uint8_t wb_mode;
    uint8_t tphrd_desc;
    uint8_t tphrd;
    uint8_t tphwr_desc;
    uint16_t cmpq_id;
    uint16_t qnum_in_func;
    uint8_t itr_notification_mode;
    uint8_t adjust_prof_id;
    uint32_t qlen;
    uint8_t quanta_prof_idx;
    uint8_t tso_ena;
    uint16_t tso_qnum;
    uint8_t legacy_int;
    uint8_t drop_ena;
    uint8_t cache_prof_idx;
    uint8_t pkt_shaper_prof_idx;
    uint8_t int_q_state;
    uint16_t tail;
};

struct ice_ctx_ele {
    uint16_t offset;
    uint16_t size_of;
    uint16_t width;
    uint16_t lsb;
};

#define ICE_CTX_STORE(_struct, _ele, _width, _lsb) { \
    .offset = offsetof(struct _struct, _ele), \
    .size_of = sizeof(((struct _struct *)0)->_ele), \
    .width = (_width), \
    .lsb = (_lsb), \
}

/* ---- RSS / VSI Update ---- */

/* VSI context valid_sections bits */
#define ICE_AQ_VSI_PROP_RXQ_MAP_VALID   BIT(6)
#define ICE_AQ_VSI_PROP_Q_OPT_VALID     BIT(7)

/* Queue mapping flags */
#define ICE_AQ_VSI_Q_MAP_CONTIG         0x0000

/* tc_mapping encoding */
#define ICE_AQ_VSI_TC_Q_OFFSET_S        0
#define ICE_AQ_VSI_TC_Q_NUM_S           11

/* q_opt_rss encoding */
#define ICE_AQ_VSI_Q_OPT_RSS_LUT_S     0
#define ICE_AQ_VSI_Q_OPT_RSS_LUT_M     (0x3U << ICE_AQ_VSI_Q_OPT_RSS_LUT_S)
#define ICE_AQ_VSI_Q_OPT_RSS_LUT_VSI   0x0U
#define ICE_AQ_VSI_Q_OPT_RSS_HASH_S    6
#define ICE_AQ_VSI_Q_OPT_RSS_HASH_M    (0x3U << ICE_AQ_VSI_Q_OPT_RSS_HASH_S)
#define ICE_AQ_VSI_Q_OPT_RSS_HASH_TPLZ 0x0U

/* RSS AQ descriptor flags */
#define ICE_AQC_RSS_VSI_VALID           BIT(15)

/* RSS key sizes */
#define ICE_AQC_RSS_KEY_DATA_SIZE       0x28
#define ICE_AQC_RSS_KEY_EXT_SIZE        0x0C
#define ICE_AQC_RSS_KEY_TOTAL_SIZE      (ICE_AQC_RSS_KEY_DATA_SIZE + ICE_AQC_RSS_KEY_EXT_SIZE)

/* RSS LUT sizes */
#define ICE_AQC_LUT_VSI_SIZE            64
#define ICE_AQC_LUT_FLAG_VSI_SIZE       0x0000

/* ---- VSI properties (indirect buffer for GET_VSI / UPDATE_VSI) ---- */

/*
 * Minimal VSI properties — we only use the queue-mapping and RSS sections.
 * The full struct is 192 bytes; we zero-fill unused fields.
 */
#define ICE_AQC_VSI_PROPS_SIZE          192
struct ice_aqc_vsi_props {
    uint16_t valid_sections;
    /* switch section (bytes 2..7) */
    uint8_t sw_id;
    uint8_t sw_flags;
    uint8_t sw_flags2;
    uint8_t veb_stat_id;
    /* security section (bytes 8..9) */
    uint8_t sec_flags;
    uint8_t sec_reserved;
    /* VLAN section (bytes 10..17) */
    uint16_t port_based_inner_vlan;
    uint8_t inner_vlan_reserved[2];
    uint8_t inner_vlan_flags;
    uint8_t inner_vlan_reserved2[3];
    /* ingress/egress up (bytes 18..25) */
    uint32_t ingress_table;
    uint32_t egress_table;
    /* outer tags (bytes 26..29) */
    uint16_t port_based_outer_vlan;
    uint8_t outer_vlan_flags;
    uint8_t outer_vlan_reserved;
    /* queue mapping (bytes 30..77) */
    uint16_t mapping_flags;
    uint16_t q_mapping[16];
    uint16_t tc_mapping[8];
    /* queueing option (bytes 78..83) */
    uint8_t q_opt_rss;
    uint8_t q_opt_tc;
    uint8_t q_opt_flags;
    uint8_t q_opt_reserved[3];
    /* outer_up + sect10 + fd section + PASID + reserved to fill to 192 */
    uint8_t tail_pad[ICE_AQC_VSI_PROPS_SIZE - 84];
} __attribute__((packed));

/* RSS key data (indirect buffer for SET_RSS_KEY / GET_RSS_KEY) */
struct ice_aqc_rss_key_data {
    uint8_t standard_rss_key[ICE_AQC_RSS_KEY_DATA_SIZE];
    uint8_t extended_hash_key[ICE_AQC_RSS_KEY_EXT_SIZE];
};

#endif

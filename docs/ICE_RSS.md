# ICE Driver RSS Setup via AdminQ Instructions

## Overview

The Intel ICE driver uses a specific sequence of Admin Queue (AdminQ) commands to configure RSS with multiple RX queues. The process involves VSI context configuration, RSS LUT programming, and RSS key setup.

## AdminQ Command Sequence

### 1. VSI Context RSS Configuration

During VSI creation, the driver sets RSS parameters in the VSI context:

```c
// ice_set_rss_vsi_ctx() - Sets RSS LUT type and hash function
ctxt->info.q_opt_rss = 
    FIELD_PREP(ICE_AQ_VSI_Q_OPT_RSS_LUT_M, lut_type) |
    FIELD_PREP(ICE_AQ_VSI_Q_OPT_RSS_HASH_M, hash_type);
``` [1](#4-0) 

The LUT type varies by VSI type:
- PF VSI: `ICE_AQ_VSI_Q_OPT_RSS_LUT_PF` (uses PF-level LUT)
- VF VSI: `ICE_AQ_VSI_Q_OPT_RSS_LUT_VSI` (uses VSI-level LUT)

### 2. RSS LUT Programming (AdminQ 0x0B03)

The driver programs the RSS lookup table via AdminQ:

```c
// ice_set_rss_lut() -> ice_aq_set_rss_lut()
params.vsi_handle = vsi->idx;
params.lut_size = lut_size;
params.lut_type = vsi->rss_lut_type;
params.lut = lut;
status = ice_aq_set_rss_lut(hw, &params);
``` [2](#4-1) [3](#4-2) 

The AdminQ command structure includes:
- VSI handle/ID
- LUT type (VSI/PF/Global)
- LUT size (128/512/2048 entries)
- LUT data array

### 3. RSS Key Programming (AdminQ 0x0B02)

The RSS hash key is programmed separately:

```c
// ice_set_rss_key() -> ice_aq_set_rss_key()
status = ice_aq_set_rss_key(hw, vsi->idx, 
    (struct ice_aqc_get_set_rss_keys *)seed);
``` [4](#4-3) [5](#4-4) 

The key is 52 bytes (13 x 32-bit words) for extended RSS.

### 4. RSS Flow Field Configuration

The driver configures which packet fields participate in RSS hashing:

```c
// ice_vsi_set_rss_flow_fld() iterates through default configurations
for (i = 0; i < ARRAY_SIZE(default_rss_cfgs); i++) {
    status = ice_add_rss_cfg(hw, vsi, cfg);
}
``` [6](#4-5) [7](#4-6) 

Default configurations include IPv4/IPv6 TCP/UDP/SCTP with appropriate header fields.

## Complete Setup Flow

1. **VSI Creation**: Allocate RX queues and set RSS context in VSI context
2. **Queue Mapping**: Program VSI queue map in hardware
3. **RSS LUT**: Fill LUT with queue indices and program via AdminQ 0x0B03
4. **RSS Key**: Set Toeplitz key via AdminQ 0x0B02
5. **Flow Fields**: Configure hash input sets for different protocols

## Key Implementation Details

- LUT size is determined by VSI type: PF uses 512 entries, VF uses 128 entries
- RSS size is limited by `min(allocated_queues, rss_table_size)`
- The LUT uses round-robin distribution by default: `lut[i] = i % rss_size` [8](#4-7) 
- Queue count is based on CPU count when RSS is enabled [9](#4-8) 

## Notes

- All AdminQ commands require proper VSI handle validation
- The driver uses `ice_aq_send_cmd()` to send AdminQ commands to firmware
- RSS configuration happens during VSI setup in `ice_vsi_cfg_def()` [10](#4-9)

### Citations

**File:** drivers/net/ethernet/intel/ice/ice_lib.c (L191-210)
```c
			vsi->alloc_txq = ice_get_txq_count(pf);
		}

		pf->num_lan_tx = vsi->alloc_txq;

		/* only 1 Rx queue unless RSS is enabled */
		if (!test_bit(ICE_FLAG_RSS_ENA, pf->flags)) {
			vsi->alloc_rxq = 1;
		} else {
			if (vsi->req_rxq) {
				vsi->alloc_rxq = vsi->req_rxq;
				vsi->num_rxq = vsi->req_rxq;
			} else {
				vsi->alloc_rxq = ice_get_rxq_count(pf);
			}
		}

		pf->num_lan_rx = vsi->alloc_rxq;

		vsi->num_q_vectors = max(vsi->alloc_rxq, vsi->alloc_txq);
```

**File:** drivers/net/ethernet/intel/ice/ice_lib.c (L1154-1186)
```c
static void ice_set_rss_vsi_ctx(struct ice_vsi_ctx *ctxt, struct ice_vsi *vsi)
{
	u8 lut_type, hash_type;
	struct device *dev;
	struct ice_pf *pf;

	pf = vsi->back;
	dev = ice_pf_to_dev(pf);

	switch (vsi->type) {
	case ICE_VSI_CHNL:
	case ICE_VSI_PF:
		/* PF VSI will inherit RSS instance of PF */
		lut_type = ICE_AQ_VSI_Q_OPT_RSS_LUT_PF;
		break;
	case ICE_VSI_VF:
	case ICE_VSI_SF:
		/* VF VSI will gets a small RSS table which is a VSI LUT type */
		lut_type = ICE_AQ_VSI_Q_OPT_RSS_LUT_VSI;
		break;
	default:
		dev_dbg(dev, "Unsupported VSI type %s\n",
			ice_vsi_type_str(vsi->type));
		return;
	}

	hash_type = ICE_AQ_VSI_Q_OPT_RSS_HASH_TPLZ;
	vsi->rss_hfunc = hash_type;

	ctxt->info.q_opt_rss =
		FIELD_PREP(ICE_AQ_VSI_Q_OPT_RSS_LUT_M, lut_type) |
		FIELD_PREP(ICE_AQ_VSI_Q_OPT_RSS_HASH_M, hash_type);
}
```

**File:** drivers/net/ethernet/intel/ice/ice_lib.c (L1590-1606)
```c
static const struct ice_rss_hash_cfg default_rss_cfgs[] = {
	/* configure RSS for IPv4 with input set IP src/dst */
	{ICE_FLOW_SEG_HDR_IPV4, ICE_FLOW_HASH_IPV4, ICE_RSS_ANY_HEADERS, false},
	/* configure RSS for IPv6 with input set IPv6 src/dst */
	{ICE_FLOW_SEG_HDR_IPV6, ICE_FLOW_HASH_IPV6, ICE_RSS_ANY_HEADERS, false},
	/* configure RSS for tcp4 with input set IP src/dst, TCP src/dst */
	{ICE_FLOW_SEG_HDR_TCP | ICE_FLOW_SEG_HDR_IPV4,
				ICE_HASH_TCP_IPV4,  ICE_RSS_ANY_HEADERS, false},
	/* configure RSS for udp4 with input set IP src/dst, UDP src/dst */
	{ICE_FLOW_SEG_HDR_UDP | ICE_FLOW_SEG_HDR_IPV4,
				ICE_HASH_UDP_IPV4,  ICE_RSS_ANY_HEADERS, false},
	/* configure RSS for sctp4 with input set IP src/dst - only support
	 * RSS on SCTPv4 on outer headers (non-tunneled)
	 */
	{ICE_FLOW_SEG_HDR_SCTP | ICE_FLOW_SEG_HDR_IPV4,
		ICE_HASH_SCTP_IPV4, ICE_RSS_OUTER_HEADERS, false},
	/* configure RSS for gtpc4 with input set IPv4 src/dst */
```

**File:** drivers/net/ethernet/intel/ice/ice_lib.c (L1670-1694)
```c
static void ice_vsi_set_rss_flow_fld(struct ice_vsi *vsi)
{
	u16 vsi_num = vsi->vsi_num;
	struct ice_pf *pf = vsi->back;
	struct ice_hw *hw = &pf->hw;
	struct device *dev;
	int status;
	u32 i;

	dev = ice_pf_to_dev(pf);
	if (ice_is_safe_mode(pf)) {
		dev_dbg(dev, "Advanced RSS disabled. Package download failed, vsi num = %d\n",
			vsi_num);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(default_rss_cfgs); i++) {
		const struct ice_rss_hash_cfg *cfg = &default_rss_cfgs[i];

		status = ice_add_rss_cfg(hw, vsi, cfg);
		if (status)
			dev_dbg(dev, "ice_add_rss_cfg failed, addl_hdrs = %x, hash_flds = %llx, hdr_type = %d, symm = %d\n",
				cfg->addl_hdrs, cfg->hash_flds,
				cfg->hdr_type, cfg->symm);
	}
}
```

**File:** drivers/net/ethernet/intel/ice/ice_lib.c (L2385-2388)
```c
			if (test_bit(ICE_FLAG_RSS_ENA, pf->flags)) {
				ice_vsi_cfg_rss_lut_key(vsi);
				ice_vsi_set_rss_flow_fld(vsi);
			}
```

**File:** drivers/net/ethernet/intel/ice/ice_main.c (L3644-3650)
```c
void ice_fill_rss_lut(u8 *lut, u16 rss_table_size, u16 rss_size)
{
	u16 i;

	for (i = 0; i < rss_table_size; i++)
		lut[i] = i % rss_size;
}
```

**File:** drivers/net/ethernet/intel/ice/ice_main.c (L7892-7912)
```c
int ice_set_rss_lut(struct ice_vsi *vsi, u8 *lut, u16 lut_size)
{
	struct ice_aq_get_set_rss_lut_params params = {};
	struct ice_hw *hw = &vsi->back->hw;
	int status;

	if (!lut)
		return -EINVAL;

	params.vsi_handle = vsi->idx;
	params.lut_size = lut_size;
	params.lut_type = vsi->rss_lut_type;
	params.lut = lut;

	status = ice_aq_set_rss_lut(hw, &params);
	if (status)
		dev_err(ice_pf_to_dev(vsi->back), "Cannot set RSS lut, err %d aq_err %s\n",
			status, libie_aq_str(hw->adminq.sq_last_status));

	return status;
}
```

**File:** drivers/net/ethernet/intel/ice/ice_main.c (L7921-7935)
```c
int ice_set_rss_key(struct ice_vsi *vsi, u8 *seed)
{
	struct ice_hw *hw = &vsi->back->hw;
	int status;

	if (!seed)
		return -EINVAL;

	status = ice_aq_set_rss_key(hw, vsi->idx, (struct ice_aqc_get_set_rss_keys *)seed);
	if (status)
		dev_err(ice_pf_to_dev(vsi->back), "Cannot set RSS key, err %d aq_err %s\n",
			status, libie_aq_str(hw->adminq.sq_last_status));

	return status;
}
```

**File:** drivers/net/ethernet/intel/ice/ice_common.c (L4580-4584)
```c
int
ice_aq_set_rss_lut(struct ice_hw *hw, struct ice_aq_get_set_rss_lut_params *set_params)
{
	return __ice_aq_get_set_rss_lut(hw, set_params, true);
}
```

**File:** drivers/net/ethernet/intel/ice/ice_common.c (L4643-4652)
```c
int
ice_aq_set_rss_key(struct ice_hw *hw, u16 vsi_handle,
		   struct ice_aqc_get_set_rss_keys *keys)
{
	if (!ice_is_vsi_valid(hw, vsi_handle) || !keys)
		return -EINVAL;

	return __ice_aq_get_set_rss_key(hw, ice_get_hw_vsi_num(hw, vsi_handle),
					keys, true);
}
```

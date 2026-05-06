# RSS Setup on Intel E810

> Source-driven guide to the RSS path implemented by the current working `my_ice_rust` codebase.
>
> Scope: this document describes the exact mechanism used by this userspace driver. It does not try to document every possible E810 RSS configuration. Existing documents under `docs/` may be stale; this report was derived from the current source, primarily `src/driver.rs :: IceDriver::setup_rx_queues_and_rss`, `src/aq_commands.rs`, `src/rx.rs`, `src/descriptor.rs`, and `src/admin_queue.rs`.

## 1. Executive Summary

On this hardware, the working RSS path is not just "set a key and a LUT".

This codebase does five separate things:

1. It allocates and enables multiple Rx queues.
2. It programs the RSS Toeplitz key.
3. It programs both the per-VSI RSS LUT and the PF RSS LUT.
4. It updates the VSI context so firmware knows that this VSI uses RSS and which contiguous Rx queue block belongs to it.
5. It moves the VSI into an existing RSS VSIG in the RSS XLT2 table, so the VSI inherits flow profiles that were already installed by firmware/DDP/another driver.

Upstream `ice` drivers do the same first four categories, but they usually solve step 5 differently: they create or associate RSS flow profiles directly through the RSS flow-management layer (`ice_add_rss_cfg()` and related helpers), which in turn manages VSIG membership and XLT2 updates internally. This userspace driver does not implement that layer; it piggybacks on a VSIG that already exists.

In this implementation, steps 1-4 were not enough by themselves. RSS only started producing non-zero `rss_hash` values after step 5.

## 2. What This Guide Does and Does Not Cover

This guide covers:

- The AQ commands used by the working code.
- The exact buffer layouts and flag usage the code sends to firmware.
- The Rx queue prerequisites needed before RSS metadata appears in Rx descriptors.
- The XLT2 / VSIG reuse strategy used by this driver.
- The diagnostics the code uses to verify that RSS actually turned on.

This guide does not cover:

- How to author a new DDP package.
- How to create new RSS flow profiles from scratch in the full upstream style.
- How to build a brand-new VSIG/profile chain without reusing one that already exists.
- Any Tx-only setup such as `qparent_teid` discovery or `ADD_TXQS`; those are not part of RSS itself.

That distinction matters: the current driver gets working RSS by reusing an existing RSS-capable VSIG. It does not synthesize one from first principles.

## 3. Terms Used in This Document

| Term | Meaning in this implementation |
|---|---|
| PF | PCIe Physical Function. This code also interprets the top 3 bits of a VSIG value as a PF number: `(vsig >> 13) & 0x7`. |
| VSI | Virtual Station Interface. The RSS key, LUT selection, and queue mapping are programmed per VSI. |
| VSIG | VSI Group. The code treats this as the object that owns a set of RSS flow profiles. A VSI enters a VSIG through the RSS XLT2 table. |
| XLT2 | The RSS-related package section that this driver reads/writes with `UPLOAD_SECTION` / `UPDATE_PKG`. The code uses section ID `43` and treats it as a table of 768 `u16` VSIG entries. |
| VSI LUT | Per-VSI RSS indirection table. This code uses 64 entries. |
| PF LUT | PF-wide RSS indirection table. This code uses 2048 entries. |
| DDP package | Firmware package that provides parsing/profile data. This driver treats an active DDP package as a practical prerequisite for RSS hash computation. |

## 4. The Hardware Model This Code Assumes

The working code behaves as if E810 RSS has three layers:

1. Key + indirection table.
2. VSI context enabling.
3. Flow-profile association.

The source makes the following working assumptions:

- The RSS key and LUT tell hardware how to hash and how to map hash output to queue indices.
- The VSI context tells firmware which queue block belongs to the VSI and which LUT type/hash algorithm to use.
- Flow profiles are not attached directly to the VSI. Instead, the VSI must land in a VSIG that already has RSS-capable profiles.

The current code treats step 3 as mandatory in practice.

## 5. Inputs You Need Before You Touch RSS

Before the RSS AQ sequence begins, this driver already has:

- A working Admin Queue transport (`src/admin_queue.rs :: aq_send_cmd`).
- A discovered default VSI number from `GET_SW_CFG` (`src/aq_commands.rs :: aq_get_default_vsi_and_lport`).
- A logical port (`lport`) from `GET_SW_CFG`.
- A contiguous block of hardware Rx queue IDs from `PFLAN_RX_QALLOC` (`src/driver.rs :: IceDriver::get_rx_queue_ids`).
- DMA-backed Rx descriptor rings and packet buffers.

Important clarifications:

- `vsi_num` is required for the RSS AQ commands.
- `lport` is not an argument to the RSS AQ commands in this code. It matters for switch rules and other setup, not for `SET_RSS_KEY`, `SET_RSS_LUT`, `UPDATE_VSI`, or the XLT2 commands.
- `qparent_teid` is not part of RSS. It is needed for Tx queue creation.

## 6. Admin Queue API Conventions

Every AQ command in this driver uses the same transport pattern.

### 6.1 Descriptor Rules

- Start from a 32-byte direct AQ descriptor.
- Set `SI` by default. `IceAqDesc::new_direct()` does this.
- If the command has an indirect buffer, pass that buffer to `aq_send_cmd()`.
- Set `RD` only when firmware is expected to read the indirect buffer from host memory.
- `aq_send_cmd()` adds `BUF` automatically when a buffer is present.
- `aq_send_cmd()` also adds `LB` automatically when the indirect buffer is larger than 512 bytes.

The relevant AQ flag bits are:

| Flag | Meaning |
|---|---|
| `ICE_AQ_FLAG_SI` | Synchronous / immediate command |
| `ICE_AQ_FLAG_RD` | Firmware reads the indirect buffer from host memory |
| `ICE_AQ_FLAG_BUF` | Indirect buffer is present |
| `ICE_AQ_FLAG_LB` | Indirect buffer is larger than 512 bytes |

### 6.2 Generic Pseudocode

```text
desc = new_direct_aq_desc(opcode)

if firmware_reads_buffer:
    desc.flags |= RD

prepare_command_specific_params(desc)
attach_indirect_buffer_to_transport(desc, buffer)

// the transport layer must ensure firmware sees:
//   - BUF when a buffer exists
//   - LB when buffer_length > 512
//   - datalen
//   - buffer DMA address

submit_to_ATQ(desc)
poll_ATQH_until_firmware_consumes_it()
check_desc.retval == 0
copy_back_response_buffer_if_any()
```

### 6.3 Example Rust Snippet

```rust
let mut desc = IceAqDesc::new_direct(ICE_AQC_OPC_SET_RSS_KEY);

{
    let flags = u16::from_le(desc.flags) | ICE_AQ_FLAG_RD;
    desc.flags = flags.to_le();

    let rss = desc.params_as_mut::<IceAqcGetSetRssKey>();
    rss.vsi_id = (vsi_num | ICE_AQC_RSS_VSI_VALID).to_le();
}

aq_send_cmd(bar0, atq, &mut desc, Some(&mut key_buf))?;
```

## 7. Queue-Side Prerequisites Before RSS Becomes Visible

The driver enables all Rx queues before it sends the RSS AQ commands.

That queue setup is not just plumbing. It affects whether RSS metadata appears in writeback descriptors.

### 7.1 Allocate a Contiguous Rx Queue Block

`src/driver.rs :: IceDriver::get_rx_queue_ids` reads `PFLAN_RX_QALLOC` and extracts:

- `first_q`
- `last_q`
- `VALID` bit

The driver then takes a contiguous range starting at `first_q`.

This matters because the LUT entries programmed later are not absolute hardware queue IDs. They are relative queue indices inside a contiguous VSI queue block.

### 7.2 Program Each Rx Queue

`src/rx.rs :: setup_and_enable_rxq` does the following for every Rx queue:

1. Zero the descriptor ring and Rx buffers.
2. Pre-populate every descriptor in read format with a packet-buffer IOVA.
3. Build an `IceRlanCtx` and pack it with `set_ctx_bits()`.
4. Write the packed context to `QRX_CONTEXT`.
5. Program `QRXFLXP_CNTXT`.
6. Enable the queue through `QRX_CTRL`.
7. Set `QRX_TAIL = DESC_COUNT - 1`.

### 7.3 Exact Rx Context Values Used by This Driver

The code uses these values in `IceRlanCtx`:

| Field | Value |
|---|---|
| `base` | `desc_iova >> 7` |
| `qlen` | `2048` |
| `dbuf` | `ICE_RX_BUF_SIZE >> 7`, so `2048 >> 7 = 16` |
| `dtype` | `ICE_RX_DTYPE_NO_SPLIT` (`0`) |
| `dsize` | `1` for 32-byte flex descriptors |
| `crcstrip` | `1` |
| `l2tsel` | `1` |
| `showiv` | `1` |
| `rxmax` | `2048` |
| `lrxqthresh` | `1` |
| `prefena` | `1` |

Relevant Rust snippet:

```rust
let rlan = IceRlanCtx {
    head: 0,
    cpuid: 0,
    base: ring.desc_iova >> 7,
    qlen: ICE_RX_DESC_COUNT,
    dbuf: ICE_RX_BUF_SIZE >> ICE_RLAN_CTX_DBUF_S as u16,
    hbuf: 0,
    dtype: ICE_RX_DTYPE_NO_SPLIT,
    dsize: 1,
    crcstrip: 1,
    l2tsel: 1,
    hsplit_0: 0,
    hsplit_1: 0,
    showiv: 1,
    rxmax: ICE_RX_BUF_SIZE as u32,
    tphrdesc_ena: 0,
    tphwdesc_ena: 0,
    tphdata_ena: 0,
    tphhead_ena: 0,
    lrxqthresh: 1,
    prefena: 1,
};
```

### 7.4 Flex Descriptor Profile Selection

`setup_and_enable_rxq()` also programs `QRXFLXP_CNTXT` to:

- `RXDID = 2` (`ICE_RXDID_FLEX_NIC`)
- `PRIO = 3`

In source comments, this is treated as required for normal NIC flex writeback and RSS metadata.

Pseudocode:

```text
reg = read32(QRXFLXP_CNTXT[q])
reg.RXDID = 2
reg.PRIO  = 3
write32(QRXFLXP_CNTXT[q], reg)
```

If you skip this, the rest of RSS may be programmed correctly and you still may not see the expected hash/writeback behavior.

## 8. Exact RSS Setup Sequence Used by This Code

The source entry point is `src/driver.rs :: IceDriver::setup_rx_queues_and_rss`.

For `num_rxq == 1`, the function skips all RSS AQ programming.

For `num_rxq > 1`, the code executes this sequence:

| Step | API | Opcode | Required by working code? | Purpose |
|---|---|---|---|---|
| 1 | `SET_RSS_KEY` | `0x0B02` | Yes | Program 52-byte Toeplitz key |
| 2 | `GET_RSS_LUT` (VSI) | `0x0B05` | No, diagnostic | Read existing 64-entry VSI LUT |
| 3 | `GET_RSS_LUT` (PF) | `0x0B05` | No, diagnostic | Read existing 2048-entry PF LUT |
| 4 | `SET_RSS_LUT` (VSI) | `0x0B03` | Yes in this implementation | Program 64-entry VSI LUT |
| 5 | `SET_RSS_LUT` (PF) | `0x0B03` | Yes in this implementation | Program 2048-entry PF LUT |
| 6 | `GET_VSI_PARAMS` | `0x0212` | No, diagnostic | Read VSI context before update |
| 7 | `UPDATE_VSI` | `0x0211` | Yes | Enable RSS and map the Rx queue block |
| 8 | `GET_VSI_PARAMS` | `0x0212` | No, diagnostic | Verify VSI context after update |
| 9 | `GET_PKG_INFO_LIST` | `0x0C43` | No, diagnostic but practically important | Check whether any DDP package is active |
| 10 | `UPLOAD_SECTION` | `0x0C41` | Yes in this implementation | Read RSS XLT2 table |
| 11 | `REQUEST_RES` | `0x0008` | Yes for write path | Acquire change lock |
| 12 | `UPDATE_PKG` | `0x0C42` | Yes in this implementation | Write one XLT2 entry: move our VSI into target VSIG |
| 13 | `RELEASE_RES` | `0x0009` | Yes for write path | Release change lock |
| 14 | `UPLOAD_SECTION` | `0x0C41` | No, diagnostic | Verify new XLT2 mapping |

## 9. Step-by-Step Details

### 9.1 Program the RSS Key with `SET_RSS_KEY`

Source: `src/aq_commands.rs :: aq_set_rss_key`

API usage:

- Opcode: `0x0B02`
- Param struct: `IceAqcGetSetRssKey`
- Indirect buffer size: `52` bytes (`ICE_RSS_KEY_SIZE`)
- Direction: host to firmware, so `RD` is set
- Required field: `vsi_id = vsi_num | ICE_AQC_RSS_VSI_VALID`

This driver uses `ICE_DFLT_RSS_KEY`, a 52-byte default Toeplitz key.

Pseudocode:

```text
desc = AQ(opcode = SET_RSS_KEY)
desc.flags |= RD
desc.params.vsi_id = vsi_num | VALID_BIT
buffer = 52-byte RSS key
send(desc, buffer)
```

Relevant Rust snippet:

```rust
let rss = desc.params_as_mut::<IceAqcGetSetRssKey>();
rss.vsi_id = (vsi_num | ICE_AQC_RSS_VSI_VALID).to_le();
```

### 9.2 Program the LUTs

Source: `src/aq_commands.rs :: aq_set_rss_lut` and `aq_set_rss_lut_pf`

This code programs two LUTs, not one:

- A 64-entry VSI LUT.
- A 2048-entry PF LUT.

Every entry is a relative queue index in the range `0 .. num_rxq-1`.

Important: the entries are not hardware queue IDs.

The absolute queue base is carried separately in `UPDATE_VSI`.

#### 9.2.1 Build the LUT Contents

The code fills both tables round-robin:

```text
for i in 0 .. lut_size-1:
    lut[i] = i mod num_rxq
```

That means:

- LUT entry `0` maps to relative queue `0`
- LUT entry `1` maps to relative queue `1`
- ...
- LUT entry `num_rxq` wraps back to relative queue `0`

#### 9.2.2 VSI LUT API Usage

- Opcode: `0x0B03`
- Param struct: `IceAqcGetSetRssLut`
- `vsi_id = vsi_num | VALID`
- `lut_params = TYPE_VSI | (size_code << 2)`
- Direction: host to firmware, so `RD` is set

For the 64-entry table used here:

- type = `ICE_AQC_RSS_LUT_TYPE_VSI = 0`
- size code = `ICE_AQC_RSS_LUT_SIZE_SMALL = 0`

#### 9.2.3 PF LUT API Usage

- Opcode: `0x0B03`
- Param struct: `IceAqcGetSetRssLut`
- `vsi_id = vsi_num | VALID`
- Direction: host to firmware, so `RD` is set
- Indirect buffer length must be exactly `2048`

The working code uses this exact encoding for the PF LUT flags field:

```rust
let flags_val: u16 = ICE_AQC_RSS_LUT_TYPE_PF | (1u16 << 3);
```

That is:

- LUT type = PF (`1`)
- 2K-size bit = bit `3`

So the final value sent by this code is `9`.

This is an important detail. The generic LUT size constants in `src/descriptor.rs` only describe the 64/128/512 cases directly. The 2048-entry PF LUT uses a raw extra size bit.

This is also consistent with upstream `ice`:

- `ice_adminq_cmd.h` defines `ICE_AQ_VSI_Q_OPT_RSS_LUT_PF = 0x2` and distinct LUT sizes `ICE_LUT_VSI_SIZE = 64` and `ICE_LUT_PF_SIZE = 2048`.
- `ice_vsi_set_rss_params()` / `ice_set_rss_vsi_ctx()` select PF LUT for PF VSIs and VSI LUT for VF/SF VSIs.
- `ice_set_rss_lut()` programs only `vsi->rss_lut_type`, not both LUT types.

So one unresolved question from the first version of this document can now be narrowed substantially:

- For a PF VSI, once `q_opt_rss` selects PF LUT, the standard `ice` drivers treat the PF LUT as the operative table.
- The extra VSI LUT write in this repo is best understood as a defensive extra step, not as part of the standard PF-VSI path.

### 9.3 Read Back the LUTs with `GET_RSS_LUT`

Source: `src/aq_commands.rs :: aq_get_rss_lut`

This command is diagnostic in the current driver, but it is extremely useful.

The code reads:

- VSI LUT before programming
- PF LUT before programming
- PF LUT again after programming

The driver does that because stale PF LUT contents caused a real bug: only queues `0-3` received traffic until the PF LUT was explicitly rewritten.

API usage:

- Opcode: `0x0B05`
- Param struct: `IceAqcGetSetRssLut`
- Direction: firmware to host, so the wrapper sets `BUF` but not `RD`

`lut_type` encodings used by the source:

| LUT kind | Encoding used by source | Buffer length |
|---|---|---|
| VSI | `0 | (0 << 2)` | `64` |
| PF | `1 | (1 << 3)` | `2048` |
| Global | `2 | (1 << 2)` | `512` |

### 9.4 Enable RSS in the VSI with `UPDATE_VSI`

Source: `src/aq_commands.rs :: aq_update_vsi_rss`

This is the point where the driver tells firmware:

- which absolute Rx queue block belongs to the VSI
- which LUT type to use
- which hash algorithm to use

API usage:

- Opcode: `0x0211`
- Param struct: `IceAqcUpdateVsi`
- Indirect buffer size: `128` bytes
- Direction: host to firmware, so `RD` is set
- Required field: `vsi_num = vsi_num | ICE_AQ_VSI_IS_VALID`

The driver sends a fresh zeroed 128-byte `ice_aqc_vsi_props` buffer and marks only two sections valid:

- `RXQ_MAP`
- `Q_OPT`

The exact bytes it writes are:

| Byte offset | Field | Value written by this code |
|---|---|---|
| `0` | `valid_sections` | `ICE_AQ_VSI_PROP_RXQ_MAP_VALID | ICE_AQ_VSI_PROP_Q_OPT_VALID` = `0x00c0` |
| `28` | `mapping_flags` | `0` |
| `30` | `q_mapping[0]` | `first_rxq` |
| `32` | `q_mapping[1]` | `num_rxq` |
| `62` | `tc_mapping[0]` | `(0 << 0) | (pow << 11)` |
| `78` | `q_opt_rss` | `0x02` |
| `79` | `q_opt_tc` | `0` |
| `80` | `q_opt_flags` | `0` |

`pow` is computed as `ceil(log2(num_rxq))`.

This is also how upstream `ice` computes it:

- Linux uses `order_base_2(qcount)`.
- FreeBSD uses `flsl(num_rx_queues - 1)`.

The associated upstream header definitions make the intended split explicit:

- `q_mapping[]` entries are queue identifiers.
- `tc_mapping[tc]` packs `offset` in bits `[10:0]` and `pow` in bits `[14:11]`.

In other words, the standard driver contract is:

- `q_mapping[0]` gives the first queue for the VSI or TC mapping style in use.
- `q_mapping[1]` carries the exact queue count for contiguous mapping.
- `tc_mapping[0]` tells firmware the traffic-class queue span as an offset plus a power-of-two range.

That does not fully reveal the hardware's internal tie-break behavior for non-power-of-two counts, but it does resolve the biggest ambiguity: our code is not inventing an unusual encoding. It matches the standard driver model.

Pseudocode:

```text
props = zeroed_128_bytes()

props.valid_sections = RXQ_MAP_VALID | Q_OPT_VALID

props.mapping_flags = 0
props.q_mapping[0] = first_rxq      // absolute hardware queue id
props.q_mapping[1] = num_rxq        // queue count

pow = ceil_log2(num_rxq)
props.tc_mapping[0] = (0 << 0) | (pow << 11)

props.q_opt_rss   = 0x02            // PF LUT, Toeplitz selector = 0
props.q_opt_tc    = 0
props.q_opt_flags = 0

send UPDATE_VSI(vsi_num | VALID, props)
```

Relevant Rust snippet:

```rust
let valid = ICE_AQ_VSI_PROP_RXQ_MAP_VALID | ICE_AQ_VSI_PROP_Q_OPT_VALID;
props[VSI_PROPS_VALID_SECTIONS_OFF..VSI_PROPS_VALID_SECTIONS_OFF + 2]
    .copy_from_slice(&valid.to_le_bytes());

props[VSI_PROPS_Q_MAPPING_OFF..VSI_PROPS_Q_MAPPING_OFF + 2]
    .copy_from_slice(&first_rxq.to_le_bytes());
props[VSI_PROPS_Q_MAPPING_OFF + 2..VSI_PROPS_Q_MAPPING_OFF + 4]
    .copy_from_slice(&num_rxq.to_le_bytes());

let q_opt_rss: u8 = ICE_AQ_VSI_Q_OPT_RSS_LUT_PF
    | (ICE_AQ_VSI_Q_OPT_RSS_HASH_TPLZ << ICE_AQ_VSI_Q_OPT_RSS_HASH_S);
props[VSI_PROPS_Q_OPT_RSS_OFF] = q_opt_rss;
```

Important implementation detail:

- The source does not do a read-modify-write of the full VSI properties block.
- It writes a zeroed 128-byte buffer and relies on `valid_sections` to scope what firmware updates.

That is an implementation choice, not a universal requirement. Another driver could choose to read the current VSI props first and only patch the relevant fields.

### 9.5 Check DDP Package State with `GET_PKG_INFO_LIST`

Source: `src/aq_commands.rs :: aq_get_pkg_info_list`

This is diagnostic in the working code, but the driver treats it as a strong clue about whether RSS hashing can work at all.

API usage:

- Opcode: `0x0C43`
- Response buffer: 4096 bytes
- Response format:
  - first 4 bytes = `count` (`u32`, little-endian)
  - then `count` instances of `IceAqcGetPkgInfo` (40 bytes each)

The code warns if:

- no DDP packages are present
- packages are present but none are active

The driver does not abort RSS setup here. It logs a warning and continues.

### 9.6 Read the RSS XLT2 Table with `UPLOAD_SECTION`

Source: `src/aq_commands.rs :: aq_read_xlt2_rss`

This is the most unusual part of the RSS path.

The driver reads a package section with:

- Opcode: `0x0C41`
- Section ID: `ICE_SID_XLT2_RSS = 43`
- Entry count: `ICE_XLT2_CNT = 768`

The request buffer format used by this driver is:

```text
u16 section_count = 1
u16 data_end      = 12 + (4 + 768 * 2)

u32 section_type  = 43
u16 section_off   = 12
u16 section_size  = 4 + 768 * 2

u16 xlt2_count    = 768
u16 xlt2_offset   = 0
u16 xlt2_values[768] = zeroed on request, filled by firmware on response
```

Important AQ nuance:

- Even though this is logically a read, the driver sets `RD`.
- Why: firmware still has to read the request structure from the indirect buffer.

After the response comes back, the code parses 768 `u16` VSIG values and keeps only the non-zero ones.

For each non-zero entry it logs:

- VSI index
- VSIG value
- `vsig_idx = vsig & 0x1fff`
- `pf_num = (vsig >> 13) & 0x7`

### 9.7 Choose a Target VSIG

Source: `src/aq_commands.rs :: aq_associate_vsi_with_rss_profiles`

The driver does not create a new RSS profile chain.

Instead, it searches the XLT2 table for a VSIG that is already used by other VSIs and moves its own VSI into that VSIG.

The exact selection algorithm is:

1. Read the full XLT2 table.
2. Record `our_current_vsig` if our VSI already appears in the non-zero entries.
3. Count how many VSIs use each non-zero VSIG.
4. Keep only VSIGs that are used by at least one other VSI.
5. Sort candidates by this key:
   - prefer `pf_num != 0`
   - prefer larger `other_vsi_count`
   - prefer a VSIG that is not our current VSIG
   - prefer smaller numeric VSIG
6. Take the first candidate.
7. If we are already in that VSIG, stop.
8. Otherwise update XLT2 so our VSI points to that VSIG.

Pseudocode:

```text
entries = read_xlt2_non_default_entries()
our_current_vsig = entries.get(our_vsi)

vsig_counts = histogram(entry.vsig for entry in entries)

candidates = []
for each vsig in vsig_counts:
    other_users = count(vsig)
    if our_current_vsig == vsig:
        other_users -= 1
    if other_users == 0:
        continue

    pf_num = (vsig >> 13) & 0x7
    candidates.append((vsig, other_users, pf_num, vsig == our_current_vsig))

sort candidates by:
    pf_num != 0 first,
    other_users descending,
    current_vsig last,
    vsig ascending

target_vsig = candidates[0]
```

Why PF preference exists in this code:

- The source comments say that in the author's environment, a non-zero-PF VSIG from the sibling PF under the kernel `ice` driver was the reliable RSS-capable choice.
- That is a heuristic from this implementation, not a hardware rule proven by this repo.

Upstream reference point:

- The full `ice` flow-management layer stores VSIG values with the same bit layout used by this repo: bits `[12:0]` are VSIG index and bits `[15:13]` are PF number.
- However, the upstream sources do not establish a rule that `pf_num != 0` is inherently better for RSS.

So this question is partially resolved:

- The PF bits in VSIG are real and well-defined.
- Preferring non-zero PF remains an environment-specific selection heuristic, not a generally proven requirement.

### 9.8 Acquire the Change Lock with `REQUEST_RES`

Source: `src/aq_commands.rs :: aq_acquire_change_lock`

Before writing XLT2, the driver takes a firmware-global change lock:

- Opcode: `0x0008`
- Param struct: `IceAqcReqRes`
- `res_id = ICE_CHANGE_LOCK_RES_ID = 3`
- `access_type = ICE_RES_WRITE = 2`
- `res_number = 0`
- `timeout = 1000` ms

The wrapper retries every 10 ms if firmware returns `retval = 0x1000` (`EBUSY` in the code's interpretation).

Pseudocode:

```text
repeat up to 100 times:
    REQUEST_RES(res_id=3, access_type=2, res_number=0, timeout=1000)
    if success: break
    if retval == 0x1000: sleep 10 ms and retry
    else: fail
```

### 9.9 Move the VSI into the Chosen VSIG with `UPDATE_PKG`

Source: `src/aq_commands.rs :: aq_write_xlt2_rss`

The driver writes exactly one XLT2 entry:

- Opcode: `0x0C42`
- Param struct: `IceAqcDownloadPkg`
- `flags = ICE_AQC_DOWNLOAD_PKG_LAST_BUF`
- Section ID: `43`

The exact request buffer used by the source is:

```text
u16 section_count = 1
u16 data_end      = 18

u32 section_type  = 43
u16 section_off   = 12
u16 section_size  = 6

u16 xlt2_count    = 1
u16 xlt2_offset   = our_vsi
u16 xlt2_value[0] = target_vsig
```

This literally means: "for VSI index `our_vsi`, write VSIG value `target_vsig` into the RSS XLT2 section".

What the full upstream driver does instead:

- `ice_add_rss_cfg()` constructs RSS flow-profile definitions from requested packet headers and hash fields.
- `ice_add_rss_cfg_sync()` searches for an existing matching profile, disassociates conflicting ones if needed, creates a new profile if necessary, and associates the VSI to that profile.
- That association path eventually calls `ice_add_prof_id_flow()`, which moves the VSI into an existing VSIG or creates a new VSIG as needed, then updates hardware.

So the upstream standalone path is not "write a raw XLT2 entry".

It is:

1. Define the desired RSS input set as flow/profile data.
2. Add or reuse a matching profile.
3. Let the flow-management layer create or reuse a VSIG.
4. Let that layer update XLT2.

### 9.10 Release the Change Lock with `RELEASE_RES`

Source: `src/aq_commands.rs :: aq_release_change_lock`

API usage:

- Opcode: `0x0009`
- Param struct: `IceAqcReqRes`
- `res_id = 3`
- `res_number = 0`

The wrapper always attempts release, even if the write failed.

## 10. Verification Checklist

The working code verifies RSS in four different ways.

### 10.1 Verify the PF LUT Really Changed

Use `GET_RSS_LUT` after programming the PF LUT.

Expected result:

- Entries cover every relative queue index `0 .. num_rxq-1`.
- Distribution counts are close to uniform.

This was added because stale PF LUT contents previously limited RSS to only 4 queues.

### 10.2 Verify the VSI Context

Use `GET_VSI_PARAMS` before and after `UPDATE_VSI`.

The source logs:

- `valid_sections`
- `mapping_flags`
- `q_mapping[0]`
- `q_mapping[1]`
- `tc_mapping[0]`
- `q_opt_rss`
- `q_opt_tc`
- `q_opt_flags`

After `UPDATE_VSI`, this driver expects:

- `q_mapping[0] = first_rxq`
- `q_mapping[1] = num_rxq`
- `q_opt_rss = 0x02`

### 10.3 Verify XLT2 Membership

Re-read the XLT2 section after the `UPDATE_PKG` write.

Expected result:

- Your VSI appears in the non-default entries.
- Its VSIG equals the target VSIG you just wrote.

### 10.4 Verify Packet-Side RSS Metadata

The source has a runtime debug helper in `src/driver.rs :: print_first_rx_packet`.

It checks:

- `rss_hash` from the Rx flex writeback descriptor
- `status_error0` bit 12, which this code treats as `rss_valid`

In the working path, the useful signs are:

- `rss_hash` is non-zero for traffic that should hash
- `rss_valid` is set
- packets from different flows arrive on different queues

One important caveat from the source:

- The debug helper computes `lut_idx = rss_hash % 64` and then `expected_q = lut_idx % num_pairs`.
- That matches a 64-entry model and is useful as a quick sanity check, but it is not a precise verifier for the 2048-entry PF LUT path.

## 11. What This Implementation Proves vs What It Infers

The source is authoritative for what the driver does, but not every hardware statement is equally proven.

### 11.1 Proven by the Working Code

- Multiple Rx queues must be enabled before RSS is programmed.
- `SET_RSS_KEY`, `SET_RSS_LUT`, `UPDATE_VSI`, and XLT2 manipulation are all part of the working path.
- PF LUT contents matter in practice. Stale PF LUT state caused a real distribution bug.
- Moving the VSI into a shared, non-default VSIG was necessary in this environment for non-zero RSS hashes.

### 11.2 Inferred or Environment-Specific

- The driver assumes that a shared, non-default VSIG is a good proxy for "this VSIG has useful RSS flow profiles".
- The driver prefers `pf_num != 0` VSIGs because that worked in the author's setup. This is a heuristic, not a documented hardware law in this repo.
- The driver programs both VSI and PF LUTs. Upstream `ice` indicates that PF VSIs standardly use the PF LUT when `q_opt_rss = LUT_PF`, so the extra VSI LUT write here should be treated as defensive rather than standard.
- `tc_mapping[0]` uses `ceil(log2(num_rxq))` while `q_mapping[1]` uses the exact queue count. Upstream drivers use the same encoding, so this is standard driver behavior, but the precise firmware behavior for non-power-of-two counts is still not spelled out in the references collected here.
- The driver does not inspect the actual content of the chosen profile chain. It only proves that reusing the chosen VSIG made RSS work in this environment.

### 11.3 Resolved by Upstream References

The following questions from the first version of this report are now substantially resolved:

1. PF LUT vs VSI LUT:
   For PF VSIs, upstream `ice` explicitly selects `ICE_AQ_VSI_Q_OPT_RSS_LUT_PF` and programs the PF-sized LUT for that VSI type. The standard path does not program both LUT types.
2. `q_mapping[1]` vs `tc_mapping[0]`:
   Upstream Linux and FreeBSD both program these exactly the same way as this repo: exact queue count in `q_mapping[1]`, power-of-two span in `tc_mapping[0]`.
3. Standalone VSIG/profile creation:
   Upstream does not manually poke XLT2 for normal RSS setup. It builds RSS flow profiles through `ice_add_rss_cfg()`, which then associates the VSI with a matching or newly created VSIG and updates XLT2 internally.
4. Inherited profile contents:
   The standard PF defaults include at least IPv4, IPv6, TCPv4, UDPv4, TCPv6, UDPv6, and additional protocol families depending on package/support. Our piggyback path still does not inspect which subset the chosen VSIG actually contains.

## 12. Failure Modes and What They Usually Mean

| Symptom | Likely cause in the current design |
|---|---|
| `rss_hash == 0`, all traffic on queue 0 | No active DDP package, no usable VSIG in XLT2, or VSI never moved into an RSS-capable VSIG |
| RSS reaches only queues 0-3 | PF LUT was never programmed, or old PF LUT contents are still active |
| `GET_RSS_LUT` VSI looks correct, but traffic distribution is still wrong | You may be updating the wrong table; the current code expects PF LUT selection in `UPDATE_VSI` |
| Packets arrive, but writeback does not look like NIC flex RSS writeback | `QRXFLXP_CNTXT` may not have `RXDID=2`, `PRIO=3` |
| No packets at all | Usually not an RSS bug; check Rx queue enable, switch rules, and packet delivery first |

## 13. Minimal Rust-to-Pseudocode Map

If you are reimplementing this in another language, these are the source functions to mirror:

| Rust source | Reimplementation responsibility |
|---|---|
| `src/admin_queue.rs :: aq_send_cmd` | AQ transport, indirect buffer setup, ATQ submission, polling, retval handling |
| `src/driver.rs :: IceDriver::get_rx_queue_ids` | Discover contiguous hardware Rx queue IDs |
| `src/rx.rs :: setup_and_enable_rxq` | Program Rx queue context, flex descriptor profile, queue enable, initial tail |
| `src/aq_commands.rs :: aq_set_rss_key` | `SET_RSS_KEY` wrapper |
| `src/aq_commands.rs :: aq_set_rss_lut` | `SET_RSS_LUT` for VSI LUT |
| `src/aq_commands.rs :: aq_set_rss_lut_pf` | `SET_RSS_LUT` for PF LUT |
| `src/aq_commands.rs :: aq_get_rss_lut` | LUT readback diagnostic |
| `src/aq_commands.rs :: aq_update_vsi_rss` | `UPDATE_VSI` buffer construction |
| `src/aq_commands.rs :: aq_get_pkg_info_list` | DDP package diagnostic |
| `src/aq_commands.rs :: aq_read_xlt2_rss` | `UPLOAD_SECTION` request/response parsing for XLT2 |
| `src/aq_commands.rs :: aq_acquire_change_lock` | `REQUEST_RES` retry loop |
| `src/aq_commands.rs :: aq_write_xlt2_rss` | `UPDATE_PKG` XLT2 write |
| `src/aq_commands.rs :: aq_release_change_lock` | `RELEASE_RES` |
| `src/aq_commands.rs :: aq_associate_vsi_with_rss_profiles` | Target VSIG selection policy |

## 14. Questions That Still Remain

After comparing against the upstream `ice` sources in `docs/knowledge/code_refs`, these are the questions that still remain genuinely open for this userspace implementation:

1. Is the preference for `pf_num != 0` actually robust across systems, or only a good heuristic for the environment this driver was tested in?
2. What exact RSS profile set is present in the VSIG we piggyback onto at runtime? Our current code does not inspect profile membership before reusing it.
3. For non-power-of-two queue counts, what exact queue-selection behavior does firmware implement when `q_mapping[1]` carries the exact count but `tc_mapping[0]` carries a rounded power-of-two span?
4. If a userspace driver wants to be fully standalone, how much of the upstream `ice_add_rss_cfg()` / flow-profile / VSIG-management stack is the minimal subset that must be ported?

What is no longer unresolved:

1. PF VSIs standardly use the PF LUT when `q_opt_rss` selects PF LUT.
2. The `q_mapping` plus `tc_mapping` encoding used by this repo matches the standard `ice` driver model.
3. A standalone path exists conceptually, but it is the full flow/profile-management path, not just raw XLT2 editing.

If you need a completely standalone userspace RSS bring-up with no dependency on an existing VSIG, this repo still does not contain that path.

## 15. One-Page Implementation Recipe

For convenience, here is the full sequence again in pseudocode.

```text
discover vsi_num via GET_SW_CFG
read contiguous Rx queue block from PFLAN_RX_QALLOC

for each Rx queue:
    zero descs and buffers
    pre-arm every descriptor with pkt_addr
    program Rx queue context
    set QRXFLXP_CNTXT.RXDID = 2
    set QRXFLXP_CNTXT.PRIO  = 3
    enable queue
    set QRX_TAIL = DESC_COUNT - 1

if num_rxq == 1:
    stop; RSS is skipped by this driver

SET_RSS_KEY(vsi_num, 52-byte Toeplitz key)

optional: GET_RSS_LUT(VSI)
optional: GET_RSS_LUT(PF)

vsi_lut = [i mod num_rxq for i in 0..63]
pf_lut  = [i mod num_rxq for i in 0..2047]

SET_RSS_LUT(VSI, vsi_num, vsi_lut)
SET_RSS_LUT(PF,  vsi_num, pf_lut)

optional: GET_VSI_PARAMS(vsi_num)

UPDATE_VSI(
    vsi_num,
    first_rxq = absolute first hardware queue id,
    num_rxq   = queue count,
    q_opt_rss = PF_LUT | TOEPLITZ,
)

optional: GET_VSI_PARAMS(vsi_num)
optional: GET_PKG_INFO_LIST()

xlt2 = UPLOAD_SECTION(section_id = 43)
target_vsig = choose_shared_non_default_vsig(xlt2)

REQUEST_RES(change_lock)
UPDATE_PKG(section_id = 43, offset = our_vsi, value = target_vsig)
RELEASE_RES(change_lock)

optional: UPLOAD_SECTION(section_id = 43) again to verify

verify:
    PF LUT readback covers all queue indices
    VSI context shows q_opt_rss = 0x02
    XLT2 shows our VSI in target VSIG
    Rx writeback shows non-zero rss_hash and rss_valid
```

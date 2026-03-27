# my_ice Driver Workflow Guide

## What this program is

`my_ice` is a small userspace Intel E800-series (`ice`) NIC driver prototype built directly on top of:

- `vfio-pci` for secure userspace PCI access
- BAR0 MMIO register programming
- VFIO DMA mappings for descriptor rings and packet buffers
- Intel ICE Admin Queue (AdminQ) commands for firmware-mediated control-plane operations

It is not linked against DPDK, but it follows the same overall deployment model:

- bind the NIC to `vfio-pci`
- enable IOMMU
- map DMA memory into the device
- program Rx/Tx queues from userspace

The code is concentrated in:

- [`main.c`](/users/manvik12/my_ice/main.c)
- [`ice_min.h`](/users/manvik12/my_ice/ice_min.h)

## High-level architecture

The driver has three layers:

1. VFIO and DMA setup
   - Opens the VFIO container/group/device.
   - Maps BAR0.
   - Allocates DMA-visible memory for AdminQ, Rx rings, Tx rings, and packet buffers.

2. Control plane through AdminQ
   - Queries firmware version.
   - Reads the port MAC address.
   - Discovers the default VSI and physical logical port (`lport`).
   - Discovers the queue scheduler parent TEID needed to create Tx queues.
   - Installs or removes a simple Rx MAC switch rule.

3. Data plane through direct register and descriptor programming
   - Rx path writes queue context into `QRX_CONTEXT`.
   - Tx path creates Tx queue contexts and submits them through `ADD_TXQS`.
   - Packet movement happens through descriptor rings in DMA memory plus doorbells/tails in BAR0.

## Main runtime flow

The top-level flow in `main()` is:

1. Parse CLI arguments and choose a mode:
   - `--rx-listen`
   - `--rx-reflect`
   - `--tx-send`
   - `--tx-bench`

2. Allocate software queue metadata:
   - `d.txqs`

3. Initialize VFIO:
   - discover IOMMU group
   - open `/dev/vfio/vfio`
   - open `/dev/vfio/<group>`
   - attach group to container
   - enable `VFIO_TYPE1_IOMMU`
   - obtain the PCI device FD
   - map BAR0

4. Size and allocate DMA memory:
   - AdminQ descriptor rings
   - AdminQ buffers
   - Tx descriptors and Tx packet buffers
   - Rx descriptors and Rx packet buffers

5. Lay out that DMA block into sub-regions and record both:
   - virtual addresses for CPU access
   - IOVAs for device access

6. Initialize the hardware AdminQ rings in BAR0.

7. Run basic control-plane discovery:
   - `GET_VER`
   - `MANAGE_MAC_READ`

8. Enter the selected mode:
   - Rx setup + polling
   - Rx setup + reflect loop
   - Tx queue creation + one-shot send
   - Tx queue creation + threaded benchmark

9. Cleanup:
   - unmap DMA from IOMMU
   - unmap BAR0
   - close VFIO FDs
   - remove temporary hugepage backing file if used

## Core software state

The program keeps almost all runtime state in `struct dev_ctx`.

### `struct dev_ctx`

This is the top-level device object. It owns:

- VFIO FDs:
  - `container_fd`
  - `group_fd`
  - `device_fd`
- BAR0 mapping:
  - `bar0`
  - `bar0_size`
- DMA backing block:
  - `dma.vaddr`
  - `dma.iova`
  - `dma.size`
- AdminQ state:
  - `atq`
  - `arq`
- datapath state:
  - `io`
  - `txqs`

### `struct aq_ring_ctx`

Represents one AdminQ ring:

- `desc`: descriptor ring
- `desc_iova`: device-visible descriptor base
- `buf`: data buffer area, one chunk per descriptor
- `buf_iova`: device-visible buffer base
- `count`: ring size
- `next_to_use`: software producer index

Two instances exist:

- `atq`: Admin Transmit Queue, used to submit firmware commands
- `arq`: Admin Receive Queue, used by firmware to post async/events and indirect data

### `struct io_ring_ctx`

Holds shared datapath identity and Rx state:

- `mac`: LAN MAC address discovered from firmware
- `lport`: physical logical port number
- `vsi_num`: default VSI for the PF
- `rxq_id`: chosen Rx queue from `PFLAN_RX_QALLOC`
- `qparent_teid`: scheduler parent used for Tx queue creation
- `rx_desc`, `rx_desc_iova`
- `rx_bufs`, `rx_bufs_iova`
- `rx_ntc`: next Rx descriptor to check/rearm

### `struct txq_ctx`

Per-Tx-queue software state:

- queue identity:
  - `txq_id`
  - `desc_count`
- ring memory:
  - `tx_desc`, `tx_desc_iova`
  - `tx_pkt_bufs`, `tx_pkt_iova`
- ring indices:
  - `tx_next_to_use`
  - `tx_next_to_clean`
  - `tx_free`
  - `tx_pkts_since_rs`

## VFIO and DMA initialization

## 1. Discover the IOMMU group

`get_iommu_group_id()` reads:

- `/sys/bus/pci/devices/<BDF>/iommu_group`

This matters because VFIO access is granted per IOMMU group, not just per function.

## 2. Open VFIO and validate capabilities

`vfio_init()` performs:

- open `/dev/vfio/vfio`
- verify `VFIO_GET_API_VERSION`
- verify `VFIO_CHECK_EXTENSION(VFIO_TYPE1_IOMMU)`
- open `/dev/vfio/<group_id>`
- verify `VFIO_GROUP_FLAGS_VIABLE`
- attach group to container
- set IOMMU type to `VFIO_TYPE1_IOMMU`
- obtain the device FD with `VFIO_GROUP_GET_DEVICE_FD`

## 3. Enable PCI memory and bus mastering

Still inside `vfio_init()`, the code reads and updates PCI config space:

- offset `PCI_COMMAND_OFF = 0x04`

It sets:

- `PCI_COMMAND_MEM`
- `PCI_COMMAND_MASTER`

This is essential because:

- BAR memory accesses require memory space enabled
- DMA requires bus mastering enabled

## 4. Map BAR0

The driver queries:

- VFIO PCI BAR0 region

Then mmaps it as:

- `d->bar0`

All later MMIO register access happens through:

- `reg_read32()`
- `reg_write32()`

## 5. Allocate and map DMA memory

`dma_map()` allocates one large DMA block, either:

- from anonymous memory
- or from a hugetlbfs-backed file if `--hugepages` is used

Important design choice:

- the IOVA is set equal to the userspace virtual address

```c
d->dma.iova = (uint64_t)(uintptr_t)d->dma.vaddr;
```

That only works because VFIO explicitly maps that virtual range into the IOMMU with `VFIO_IOMMU_MAP_DMA`.

## DMA layout

`layout_dma()` partitions the big DMA block in this order:

1. ATQ descriptors
2. ARQ descriptors
3. ATQ buffers
4. ARQ buffers
5. Tx descriptors for all requested Tx queues
6. Tx packet buffers for all requested Tx queues
7. Rx descriptors
8. Rx packet buffers

Alignment rules:

- descriptors generally aligned to 128 bytes
- AdminQ buffers aligned to 4096 bytes

This function computes both CPU pointers and device IOVAs for every region.

## AdminQ control plane

The Admin Queue is the driver’s firmware mailbox. The code uses it for all higher-level resource discovery and some policy operations.

## AdminQ rings and BAR0 registers

AdminQ BAR0 register families from `ice_min.h`:

- ATQ registers
  - `PF_FW_ATQBAL = 0x00080000`
  - `PF_FW_ATQBAH = 0x00080100`
  - `PF_FW_ATQLEN = 0x00080200`
  - `PF_FW_ATQH   = 0x00080300`
  - `PF_FW_ATQT   = 0x00080400`

- ARQ registers
  - `PF_FW_ARQBAL = 0x00080080`
  - `PF_FW_ARQBAH = 0x00080180`
  - `PF_FW_ARQLEN = 0x00080280`
  - `PF_FW_ARQH   = 0x00080380`
  - `PF_FW_ARQT   = 0x00080480`

Ring sizes:

- `ICE_AQ_NUM_DESC = 64`
- `ICE_AQ_MAX_BUF_LEN = 4096`

## AdminQ hardware init

`adminq_hw_init()` does four things:

1. Clears all AdminQ descriptors and buffers.
2. Prepares every ARQ descriptor with a receive buffer address.
3. Programs ATQ base/length/head/tail registers.
4. Programs ARQ base/length/head/tail registers, then posts all ARQ entries by writing:
   - `PF_FW_ARQT = ICE_AQ_NUM_DESC - 1`

This means:

- ATQ starts empty and software pushes commands into it
- ARQ starts full of posted receive buffers for firmware responses/events

## Command submission model

`aq_send_cmd()` implements synchronous command submission:

1. Check that ATQ is not full by comparing software `next_to_use` to hardware `PF_FW_ATQH`.
2. Copy the command descriptor into the ATQ ring.
3. If a data buffer is provided:
   - copy the payload into the corresponding ATQ buffer slot
   - set `ICE_AQ_FLAG_BUF`
   - set `ICE_AQ_FLAG_LB` if buffer is larger than `ICE_AQ_LG_BUF`
   - fill DMA address fields in the descriptor
4. Advance software tail.
5. Ring the queue by writing `PF_FW_ATQT`.
6. Poll until hardware head catches up.
7. Copy the completion descriptor and any returned buffer contents back to caller memory.
8. Check `retval`.

This is a very simple polling control plane:

- no interrupts
- no separate ARQ event-consumption loop
- synchronous timeout-based completion

## AdminQ commands used by this driver

### `GET_VER` (`0x0001`)

Purpose:

- retrieve firmware and API versioning

Used by:

- `aq_get_fw_ver()`

### `MANAGE_MAC_READ` (`0x0107`)

Purpose:

- retrieve MAC addresses known to firmware

Used by:

- `aq_manage_mac_read()`

What the driver extracts:

- LAN MAC address into `d->io.mac`
- LAN logical port number into `d->io.lport`

### `GET_SW_CFG` (`0x0200`)

Purpose:

- walk switch configuration elements exposed by firmware

Used by:

- `aq_get_default_vsi_and_lport()`

What the driver extracts:

- default PF VSI number
- physical logical port

The code loops until firmware returns `element == 0`, treating the response as a paginated switch configuration walk.

### `GET_DFLT_TOPO` (`0x0400`)

Purpose:

- get default Tx scheduler topology for a port

Used by:

- `aq_get_qparent_teid()`

Why it matters:

- `ADD_TXQS` needs a parent TEID under which new Tx queues will be attached

Selection logic:

- if the last topology element is a leaf, the code uses the previous node as `qparent_teid`
- otherwise it uses the last node directly

Optional debug:

- `--dump-topo` dumps both parsed topology and raw bytes
- `--qparent-teid <hex>` bypasses discovery and forces a chosen TEID

### `ADD_SW_RULES` (`0x02A0`)

Purpose:

- install a switch rule that forwards traffic matching the port MAC to the default VSI

Used by:

- `aq_add_rx_mac_rule()`

Rule style:

- lookup type: Rx
- recipe: MAC
- source: `lport`
- action: forward to `vsi_num`

This is important for the Rx demo because the queue itself is not enough; the switch path must also direct the packet into the right VSI.

### `REMOVE_SW_RULES` (`0x02A2`)

Purpose:

- remove the temporary Rx rule installed for the listen demo

Used by:

- `aq_remove_sw_rule_best_effort()`

### `ADD_TXQS` (`0x0C30`)

Purpose:

- create Tx queues in firmware/scheduler context

Used by:

- `add_tx_queues()`

This is the bridge between:

- direct userspace ring memory
- firmware-managed Tx scheduling tree

## Context encoding model

The hardware queue contexts are not stored in C layout order. They are packed bitfields expected by hardware/firmware.

The code handles that with:

- `struct ice_ctx_ele`
- `ICE_CTX_STORE(...)`
- `set_ctx_bits()`

Two lookup tables describe how software structs map into hardware bit positions:

- `rlan_ctx_info[]` for Rx queue context
- `tlan_ctx_info[]` for Tx queue context

This is one of the most important design points in the driver:

- software builds normal C structs (`struct ice_rlan_ctx`, `struct ice_tlan_ctx`)
- `set_ctx_bits()` repacks them into the exact hardware format

## Data plane register families and offsets

These are the key BAR0 offsets used by the datapath.

### Queue allocation discovery

- `PFLAN_RX_QALLOC = 0x001D2500`
- `PFLAN_TX_QALLOC = 0x001D2580`

Purpose:

- discover which Rx/Tx queues belong to this PF

The code checks the corresponding `VALID` bits and extracts:

- first queue index
- last queue index

### Rx queue programming

- `QRX_CONTEXT(i, q) = 0x00280000 + i*8192 + q*4`
- `QRX_CTRL(q)       = 0x00120000 + q*4`
- `QRX_TAIL(q)       = 0x00290000 + q*4`
- `QRXFLXP_CNTXT(q)  = 0x00480000 + q*4`

Purpose:

- choose Rx descriptor format
- write Rx queue context dwords
- request queue enable
- post receive descriptors

### Tx queue programming and runtime

- `QTX_COMM_DBELL(q) = 0x002C0000 + q*4`
- `QTX_COMM_HEAD(q)  = 0x000E0000 + q*4`

Purpose:

- doorbell hardware after software enqueues Tx descriptors
- read back hardware head to reclaim descriptors

### Per-VSI counters

- Tx bytes:
  - `GLV_GOTCL(vsi)`
  - `GLV_GOTCH(vsi)`
- Rx bytes:
  - `GLV_GORCL(vsi)`
  - `GLV_GORCH(vsi)`

Purpose:

- sanity-check whether Tx/Rx traffic actually moved through the VSI

### MDD / error reporting

- `GL_MDET_TX_TCLAN`
- `GL_MDET_TX_PQM`
- `GL_MDET_RX`
- `PF_MDET_TX_TCLAN`
- `PF_MDET_TX_PQM`
- `PF_MDET_RX`

Purpose:

- dump misbehavior detection registers if Rx fails or descriptors look wrong

## Rx path in detail

The Rx demo is implemented by `run_rx_listen()`.

## Rx initialization sequence

1. Read `PFLAN_RX_QALLOC`.
2. Extract the PF’s first Rx queue and store it in `d->io.rxq_id`.
3. Discover `vsi_num` and `lport` through `GET_SW_CFG`.
4. Program the Rx queue using `setup_and_enable_rxq()`.
5. Install an Rx MAC switch rule forwarding traffic for the NIC MAC into that VSI.
6. Poll descriptors until a packet arrives or timeout expires.
7. Remove the switch rule on exit.

## How Rx descriptors are prepared

For each of `ICE_RX_DESC_COUNT = 128` descriptors:

- `read.pkt_addr` points to one `ICE_RX_BUF_SIZE = 2048` packet buffer
- `read.hdr_addr = 0`

This uses the 32-byte flexible descriptor format:

- `union ice_32b_rx_flex_desc`

## Rx descriptor format selection

The code programs `QRXFLXP_CNTXT(q)`:

- `RXDID = ICE_RXDID_FLEX_NIC`
- priority field set to `0x03`

That tells hardware which completion layout to write into each descriptor.

## Rx queue context fields used

The driver populates `struct ice_rlan_ctx` with:

- `base = rx_desc_iova >> 7`
- `qlen = 128`
- `dbuf = 2048 >> 7`
- `dsize = 1`
- `crcstrip = 1`
- `l2tsel = 1`
- `dtype = ICE_RX_DTYPE_NO_SPLIT`
- `showiv = 1`
- `rxmax = 2048`
- `lrxqthresh = 1`
- `prefena = 1`

Interpretation:

- ring base is supplied in 128-byte units
- buffers are single-buffer, no header split
- L2 selection and stripping are enabled for a simple Ethernet receive path
- prefetching and low threshold are enabled

After packing this context, the driver writes 8 dwords into:

- `QRX_CONTEXT(0..7, q)`

## Rx queue enable handshake

`wait_rxq_ready()` polls `QRX_CTRL(q)` until:

- `QENA_REQ` and `QENA_STAT` match

Then `setup_and_enable_rxq()` sets `QENA_REQ` if needed and waits again until the queue reports enabled.

Finally it posts receive work by writing:

- `QRX_TAIL(q) = ICE_RX_DESC_COUNT - 1`

## Rx polling loop

`poll_one_rx_packet()` scans descriptors starting at `rx_ntc`.

Completion checks:

- `DD` bit must be set
- `EOF` bit must be set
- `RXE` bit must not be set
- packet length must be non-zero

If valid:

- packet bytes are copied from the per-descriptor Rx buffer
- descriptor is re-armed with the same buffer address
- `QRX_TAIL` is advanced for that descriptor index

If invalid:

- the descriptor is rearmed
- the function returns error
- caller prints MDD registers

## Tx path in detail

The Tx datapath appears in two modes:

- `run_tx_send()` for a simple packet generator
- `run_tx_bench()` for sustained throughput testing

## Tx initialization sequence

1. Read `PFLAN_TX_QALLOC`.
2. Extract first and last PF-owned Tx queue indices.
3. Clamp requested `--tx-queues` to available queue count.
4. Assign `txq_id`s sequentially from `first_q`.
5. Discover `vsi_num` and `lport` via `GET_SW_CFG`.
6. Discover `qparent_teid` via `GET_DFLT_TOPO` unless overridden.
7. Submit `ADD_TXQS` to instantiate the queue contexts.
8. Initialize software Tx ring bookkeeping with `tx_ring_init()`.

## Tx queue context fields used

For each queue, `add_tx_queues()` builds `struct ice_tlan_ctx` with:

- `port_num = lport`
- `qlen = tx_desc_count`
- `base = tx_desc_iova >> 7`
- `pf_num = 0`
- `vmvf_type = PF`
- `src_vsi = vsi_num`
- `tso_ena = 1`
- `internal_usage_flag = 1`
- `tso_qnum = txq_id`
- `legacy_int = 1`

Then it packs the context through `tlan_ctx_info[]` into the `ADD_TXQS` payload.

Scheduler metadata attached per queue:

- valid sections: generic + CIR + EIR
- CIR/EIR profile = default profile ID
- CIR/EIR bandwidth allocation = default weight

Result:

- firmware creates queue objects under the chosen scheduler parent
- the returned `q_teid` values are logged

## Tx descriptor format

The Tx descriptor is:

```c
struct ice_tx_desc {
    uint64_t buf_addr;
    uint64_t cmd_type_offset_bsz;
};
```

This driver uses a very small subset of fields:

- `buf_addr`: DMA address of the packet buffer
- `cmd_type_offset_bsz`:
  - descriptor type = data
  - command bits:
    - `EOP`
    - `RS` periodically
  - transmit buffer size = packet length

No offloads are really exercised even though some Tx queue context flags are enabled.

## Tx ring bookkeeping

### `tx_ring_init()`

Initializes each queue:

- clear descriptors
- `tx_next_to_use = 0`
- `tx_next_to_clean = 0`
- `tx_free = desc_count - 1`

One slot is intentionally left unused to distinguish full from empty.

### `tx_update_free()`

Reads hardware consumption from:

- `QTX_COMM_HEAD(q)`

Then computes:

- how many descriptors are still in flight
- how many are free for new work

### `tx_try_enqueue()`

This is the core Tx datapath primitive.

Steps:

1. Check packet length against `ICE_TX_PKT_BUF_SIZE`.
2. Reclaim descriptors if software thinks ring is full.
3. Pick current `tx_next_to_use`.
4. Optionally copy packet bytes into the queue’s packet buffer array.
5. Build descriptor:
   - `buf_addr = tx_pkt_iova + idx * ICE_TX_PKT_BUF_SIZE`
   - type = data
   - command always includes `EOP`
   - every `TX_RS_THRESH = 32` packets, also set `RS`
6. Issue a full memory barrier.
7. Advance software producer index.

The `RS` cadence is important because it helps hardware update head/progress often enough for software reclaim.

### `tx_ring_doorbell()`

Writes:

- `QTX_COMM_DBELL(q) = tx_next_to_use`

This makes hardware consume newly prepared descriptors.

### `tx_wait_drain()`

Polls hardware head until:

- `tx_next_to_clean == tx_next_to_use`

This is used after send loops to let outstanding work finish.

## Operational modes

## `--rx-listen`

Behavior:

- creates and enables one Rx queue
- installs an Rx MAC forwarding rule
- polls for the first packet
- prints L2 header info and payload dump

Use this mode when:

- you want to validate that packets can reach a userspace-programmed Rx queue

## `--rx-reflect`

Behavior:

- creates and enables one Rx queue
- installs an Rx MAC forwarding rule
- creates one Tx queue
- polls for completed Rx descriptors
- copies each packet from Rx DMA to a queue-owned Tx DMA buffer
- rewrites Ethernet MAC addresses:
  - destination MAC = original source MAC
  - source MAC = NIC LAN MAC
- sends the reflected frames back out in small Tx doorbell batches

Use this mode when:

- you want a realistic Rx -> CPU touch -> Tx workload
- you want packets sent back to the original sender with only the MAC addresses changed
- you want a simple baseline before exploring zero-copy buffer loaning

### `--rx-reflect` code map

The key implementation points in `main.c` are:

- `poll_one_rx_desc()`
  - finds a completed Rx descriptor and returns the Rx buffer index and packet length
- `tx_try_enqueue()`
  - copies packet bytes into the queue-owned Tx DMA buffer with:
    - `memcpy(buf, pkt, len)`
  - rewrites only the Ethernet MAC addresses with:
    - `memcpy(buf, dst_mac, ETHER_ADDR_LEN)`
    - `memcpy(buf + ETHER_ADDR_LEN, src_mac, ETHER_ADDR_LEN)`
- `run_rx_reflect()`
  - calls `tx_try_enqueue(d, q, rxpkt, rx_len, true, rxpkt + ETHER_ADDR_LEN, d->io.mac)`
  - this means:
    - copy from the Rx DMA buffer pointed to by `rxpkt`
    - destination MAC becomes the original source MAC
    - source MAC becomes the local NIC MAC

Important clarification:

- the packet payload is not modified
- only the first 12 bytes of the Ethernet header are rewritten

### When the copy happens

The copy to Tx DMA happens per packet, immediately after `run_rx_reflect()` sees a completed Rx descriptor and before the Rx descriptor is rearmed.

So the sequence is:

1. `poll_one_rx_desc()` finds a completed Rx packet
2. `tx_try_enqueue()` copies that packet from Rx DMA to a Tx DMA buffer
3. `tx_try_enqueue()` rewrites the MAC addresses in the copied Tx buffer
4. `rearm_rx_desc()` returns the Rx buffer to the Rx ring

### Reflect batch size

The reflect path uses:

- `const uint16_t reflect_batch = 32;`

inside `run_rx_reflect()`.

That value controls how many packets are copied/enqueued in one inner loop before the code rings the Tx doorbell once.

So for `--rx-reflect`:

- copy/enqueue work is done one packet at a time
- up to 32 packets are accumulated in one batch
- then `tx_ring_doorbell()` is called once for that batch

Yes, in the reflect path the same `reflect_batch` threshold is used for both:

- how many packets are copied into Tx DMA in the inner loop
- how many enqueued packets are allowed before ringing the doorbell

This is separate from `TX_BURST_SIZE = 64`, which is used by `--tx-bench`, not by `--rx-reflect`.

## `--tx-send`

Behavior:

- creates Tx queues
- uses queue 0
- builds a single Ethernet frame template
- sends it `count` times with optional interval

Frame format:

- destination MAC = CLI argument
- source MAC = NIC LAN MAC
- EtherType = `0x88B5`
- payload = CLI string or default `"my_ice-userspace-tx"`
- padded to at least 60 bytes

It also reads the per-VSI Tx byte counter before and after.

## `--tx-bench`

Behavior:

- creates one or more Tx queues
- pre-populates each queue’s packet buffers with the benchmark frame
- spawns one thread per Tx queue
- each thread repeatedly tries to submit bursts of `TX_BURST_SIZE = 64`
- optional CPU pinning with `--pin-cpus`
- prints instantaneous and average Gbps from the VSI byte counter

This is the best mode for stressing the direct userspace Tx path.

## Hugepage support

The driver can run without hugepages because VFIO can map normal anonymous memory.

However, `--hugepages` is supported for more DPDK-like behavior:

- backing file created under a hugetlbfs mount
- default directory: `/mnt/huge`

Flow:

1. `prepare_hugepage_file()` checks the filesystem type is `HUGETLBFS_MAGIC`.
2. It sizes the file to a hugepage multiple.
3. `dma_map()` mmaps that file and registers it with VFIO.

This is mostly useful for:

- stable physically-backed memory behavior
- alignment with common packet-processing deployment practices

## Control plane versus data plane

This split is the key to understanding the driver.

## Control plane

Control-plane operations go through firmware/AdminQ and establish resources or policy:

- firmware/API identity
- MAC discovery
- switch configuration discovery
- scheduler topology discovery
- Tx queue creation
- switch rule add/remove

These are all relatively infrequent and synchronous.

## Data plane

Data-plane operations avoid firmware round trips and touch rings/registers directly:

- Rx descriptors in DMA memory
- Tx descriptors in DMA memory
- Rx queue MMIO context registers
- Tx doorbells and head polling
- Rx tail posting and descriptor polling

This is the high-rate packet path.

## Important offsets and encodings to remember

### IOVA encoding

The hardware context fields store ring bases shifted right by 7:

- Rx: `rlan.base = rx_desc_iova >> 7`
- Tx: `tlan.base = tx_desc_iova >> 7`

That means queue base addresses are encoded in 128-byte units.

### Rx buffer size encoding

`rlan.dbuf` is programmed as:

- `ICE_RX_BUF_SIZE >> ICE_RLAN_CTX_DBUF_S`
- with `ICE_RLAN_CTX_DBUF_S = 7`

So the descriptor buffer size is also encoded in 128-byte units.

### Ring sizes used by default

- AdminQ descriptors: 64
- Rx descriptors: 128
- Tx descriptors: 128 per queue
- Rx buffers: 2048 bytes
- Tx packet buffers: 2048 bytes

These defaults are demo-friendly rather than heavily tuned.

## Limitations and simplifying assumptions

This driver is intentionally minimal. A few important limitations:

- no interrupt handling
- no ARQ event-processing loop
- no robust reset/recovery path
- no advanced Rx steering beyond one MAC rule
- no cleanup for created Tx queue resources through a matching delete flow
- no link-state management
- no VLAN, checksum, RSS, or timestamp feature programming
- no multi-segment packets
- Tx sends from preallocated per-descriptor packet buffers only
- Rx path receives only a single queue and single-buffer packets

The result is a teaching and experimentation driver, not a production PMD.

## How to use the userspace driver

## 1. Build it

The project uses a very small `Makefile`:

```bash
make
```

That compiles:

- `main.c` into `./my_ice`

## 2. Prepare the system

Required prerequisites:

- IOMMU enabled in the kernel
- NIC bound to `vfio-pci`
- hugepages mounted if you want `--hugepages`
- root privileges for VFIO access unless your environment grants them otherwise

Typical boot args:

```bash
intel_iommu=on iommu=pt
```

Typical hugepage mount:

```bash
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

## 3. Bind the NIC to `vfio-pci`

Example:

```bash
sudo dpdk-devbind.py -b vfio-pci 0000:17:00.0
```

If the interface is still active in Linux, bring it down first and make sure it is not carrying your management session.

## 4. Run receive mode

Listen for one packet for up to 30 seconds:

```bash
sudo ./my_ice 0000:17:00.0 --rx-listen
```

Listen for up to 60 seconds:

```bash
sudo ./my_ice 0000:17:00.0 --rx-listen 60
```

Expected behavior:

- prints firmware and API versions
- prints discovered MAC
- sets up Rx queue and MAC steering rule
- prints first received Ethernet frame and payload

## 5. Run reflect mode

Run the reflector on `node_0_new`:

```bash
sudo ./my_ice <BDF> --rx-reflect 60
```

This mode receives packets sent to the DUT MAC, copies them from Rx DMA to Tx DMA, rewrites only the Ethernet source/destination MACs, and sends them back out.

### What to run on `node_1_new`

For a correctness test, send raw L2 frames with source MAC `40:a6:b7:c3:41:20`:

```bash
sudo python3 /users/manvik12/my_ice/send_l2.py \
  -i <node-1-iface> \
  -d <node-0-my_ice-mac> \
  -s 40:a6:b7:c3:41:20 \
  -p hello-reflect \
  -c 20 \
  -t 0.1
```

The reflected frames should come back to `node_1_new` with:

- destination MAC = `40:a6:b7:c3:41:20`
- source MAC = `<node-0-my_ice-mac>`
- unchanged payload

To watch the reflected traffic on `node_1_new`:

```bash
sudo tcpdump -enn -i <node-1-iface> ether dst 40:a6:b7:c3:41:20
```

For higher-rate testing, use your preferred traffic generator on `node_1_new` and send to `<node-0-my_ice-mac>`. If you want the reflected frames to come back to `40:a6:b7:c3:41:20`, make sure the sender uses that source MAC.

## 6. Run simple transmit mode

Send one frame:

```bash
sudo ./my_ice <BDF> --tx-send <dst-mac>
```

Send 20 frames every 100 ms with custom payload:

```bash
sudo ./my_ice <BDF> --tx-send <dst-mac> 20 100 hello
```

Expected behavior:

- discovers VSI and scheduler topology
- creates Tx queue(s)
- sends raw Ethernet frames with EtherType `0x88B5`
- prints byte counter delta from the VSI

## 7. Run throughput mode

Single-queue benchmark:

```bash
sudo ./my_ice <BDF> --tx-bench 10 <dst-mac> 46
```

Multi-queue benchmark with hugepages and CPU pinning:

```bash
sudo ./my_ice <BDF> --tx-bench 15 <dst-mac> 1472 --tx-queues 4 --pin-cpus --hugepages --hugepage-dir /mnt/huge
```

Expected behavior:

- one worker thread per Tx queue
- 1-second interval Gbps reports
- final average Gbps summary

## 8. Useful debug options

Dump the scheduler topology returned by firmware:

```bash
sudo ./my_ice <BDF> --tx-send <dst-mac> --dump-topo
```

Force a known queue parent TEID:

```bash
sudo ./my_ice <BDF> --tx-send <dst-mac> --qparent-teid 0x12345678
```

This is handy if topology discovery works inconsistently on a given firmware revision.

## How to reason about packet flow end to end

For Rx:

1. External packet arrives on the physical port.
2. Switch logic matches the temporary MAC rule.
3. Packet is forwarded into the PF’s VSI.
4. Enabled Rx queue writes a completion into the DMA descriptor ring.
5. Userspace polls the descriptor, copies packet bytes, and rearms the slot.

For Rx reflect:

1. External packet arrives on the physical port.
2. Switch logic matches the temporary MAC rule.
3. Packet is forwarded into the PF's VSI and written into an Rx DMA buffer.
4. Userspace polls the completed Rx descriptor.
5. Userspace copies the packet bytes into a queue-owned Tx DMA buffer.
6. Userspace rewrites:
   - destination MAC = original source MAC
   - source MAC = local NIC MAC
7. Userspace rearms the Rx descriptor.
8. Userspace rings the Tx doorbell after a small batch.
9. Hardware fetches the Tx descriptor and transmits the reflected frame.

For Tx:

1. Userspace copies a frame into a queue-owned DMA buffer.
2. Userspace writes a Tx descriptor pointing to that buffer.
3. Userspace rings the Tx doorbell.
4. Hardware fetches the descriptor and frame over DMA.
5. Scheduler and VSI context route it to the selected port.
6. Userspace later reads hardware head to reclaim descriptors.

## If you want to extend this driver

The most natural next steps are:

- add explicit Tx queue teardown if firmware supports it in your flow
- add RSS or multiple Rx queues
- add a proper ARQ event processing loop
- add loopback commands using the already-defined `SET_MAC_LB` / `SET_PHY_LB` opcodes
- add link and port status reporting
- split control-plane and dataplane code into separate files
- add a packet buffer abstraction instead of raw fixed arrays

## Summary

The shortest mental model is:

- VFIO gives the process safe ownership of the PCI function.
- DMA memory holds AdminQ, Rx rings, Tx rings, and packet buffers.
- AdminQ discovers MAC/VSI/topology and creates Tx queues.
- BAR0 queue registers enable Rx and drive Tx.
- The Rx and Tx fast paths are simple polling loops over DMA descriptors.

That is the core workflow of this userspace ICE prototype.

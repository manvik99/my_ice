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
- map DMA-visible memory into the device
- program Rx/Tx queues directly from userspace

The most important files are:

- [`main.c`](main.c)
- [`ice_min.h`](ice_min.h)
- [`ixy-memory-model.md`](ixy-memory-model.md)
- [`RX_TX_REFLECTOR_DESIGN.md`](RX_TX_REFLECTOR_DESIGN.md)

The current reflector implementation follows the zero-copy ownership model from `ixy-memory-model.md`, not the older copy-first phase described in `RX_TX_REFLECTOR_DESIGN.md`.

## One-Page Code Map

These links are repo-relative so local Markdown viewers can open them. If your viewer ignores the `#L...` suffix, it should still open the file; then jump to the referenced line number manually.

- Process entry and mode dispatch: [`main()`](main.c#L2224)
- VFIO bring-up and BAR0 mapping: [`vfio_init()`](main.c#L475)
- DMA mapping: [`dma_map()`](main.c#L533)
- DMA sub-region layout: [`layout_dma()`](main.c#L561)
- Software ownership arrays and mempool metadata: [`alloc_queue_sw_state()`](main.c#L314)
- Reflect mempool initialization: [`pkt_pool_init()`](main.c#L337)
- AdminQ hardware setup: [`adminq_hw_init()`](main.c#L623)
- Firmware and switch discovery:
  - [`aq_get_fw_ver()`](main.c#L744)
  - [`aq_manage_mac_read()`](main.c#L763)
  - [`aq_get_default_vsi_and_lport()`](main.c#L804)
  - [`aq_get_qparent_teid()`](main.c#L863)
  - [`aq_add_rx_mac_rule()`](main.c#L922)
- Tx queue creation: [`add_tx_queues()`](main.c#L1007)
- Rx queue setup:
  - flat Rx buffer mode: [`setup_and_enable_rxq()`](main.c#L1145)
  - pool-backed Rx mode: [`setup_and_enable_rxq_pool()`](main.c#L1161)
- Tx ring lifecycle:
  - init: [`tx_ring_init()`](main.c#L1181)
  - completion/recycle: [`tx_update_free()`](main.c#L1196)
- Rx polling and rearm:
  - descriptor completion scan: [`poll_one_rx_desc()`](main.c#L1224)
  - repost buffer to hardware: [`rearm_rx_desc()`](main.c#L1363)
- Tx enqueue helpers:
  - queue-owned copy/static-buffer path: [`tx_try_enqueue()`](main.c#L1296)
  - zero-copy pooled-buffer path: [`tx_try_enqueue_pkt_buf()`](main.c#L1326)
- In-place reflect MAC rewrite: [`rewrite_reflect_l2()`](main.c#L1481)
- Runtime modes:
  - listen: [`run_rx_listen()`](main.c#L1490)
  - reflect: [`run_rx_reflect()`](main.c#L1566)
  - single-shot Tx: [`run_tx_send()`](main.c#L1768)
  - Tx benchmark: [`run_tx_bench()`](main.c#L1928)
- Shutdown: [`cleanup()`](main.c#L2132)

## High-Level Architecture

The driver has three layers:

1. VFIO and DMA setup
   - Opens the VFIO container/group/device.
   - Maps BAR0.
   - Maps one large DMA-visible memory block for AdminQ, Rx, Tx, and the reflect mempool.

2. Control plane through AdminQ
   - Queries firmware version.
   - Reads the NIC MAC address.
   - Discovers the default VSI and logical port.
   - Discovers the scheduler parent TEID needed for Tx queue creation.
   - Installs and removes an Rx MAC switch rule.

3. Data plane through direct register and descriptor programming
   - Rx queue contexts are written through `QRX_CONTEXT`.
   - Tx queue contexts are created through `ADD_TXQS`.
   - Rx and Tx datapaths use DMA descriptor rings plus MMIO tails/doorbells.
   - The reflect mode uses an ixy-style memory pool and passes the same DMA buffer from Rx to Tx without copying payload bytes.

## Main Runtime Flow

The top-level flow is implemented in [`main()`](main.c#L2224).

1. Parse CLI arguments and choose one mode:
   - `--rx-listen`
   - `--rx-reflect`
   - `--tx-send`
   - `--tx-bench`

2. Allocate software-side state:
   - `d.txqs` in [`main()`](main.c#L2440)
   - Rx slot ownership array, Tx completion ownership arrays, and mempool free-stack metadata in [`alloc_queue_sw_state()`](main.c#L314)

3. Initialize VFIO and BAR0 in [`vfio_init()`](main.c#L475).

4. Compute the DMA size in [`main()`](main.c#L2455), then map it in [`dma_map()`](main.c#L533).

5. Partition that DMA block into sub-regions in [`layout_dma()`](main.c#L561).

6. Initialize the reflect mempool entries in [`pkt_pool_init()`](main.c#L337).

7. Program AdminQ hardware state in [`adminq_hw_init()`](main.c#L623).

8. Run basic firmware discovery:
   - [`aq_get_fw_ver()`](main.c#L744)
   - [`aq_manage_mac_read()`](main.c#L763)

9. Enter the selected mode:
   - [`run_rx_listen()`](main.c#L1490)
   - [`run_rx_reflect()`](main.c#L1566)
   - [`run_tx_send()`](main.c#L1768)
   - [`run_tx_bench()`](main.c#L1928)

10. Tear down mappings and free software metadata in [`cleanup()`](main.c#L2132).

## Core Software State

Almost all runtime state lives under [`struct dev_ctx`](main.c#L120).

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
- queue bookkeeping:
  - `txq_count`
  - `txq_alloc_count`
  - `tx_desc_count`
  - `txqs`
- AdminQ state:
  - `atq`
  - `arq`
- datapath state:
  - `io`
- reflect mempool state:
  - `reflect_pool`

### `struct pkt_buf`

The pooled packet object for zero-copy reflection lives in [`struct pkt_buf`](main.c#L55).

Each entry carries:

- `buf_addr_iova`: the exact DMA address programmed into hardware descriptors
- `mempool`: a back-pointer to the owning pool
- `mempool_idx`: the stable slot number in that pool
- `size`: current packet length
- `data[]`: packet bytes, aligned so the payload starts on a cache-line boundary

This is the core ixy-style idea: the packet object is self-describing, so completion code can recycle a buffer without guessing where it came from.

### `struct pkt_mempool`

The pool metadata lives in [`struct pkt_mempool`](main.c#L64).

Important fields:

- `base`, `base_iova`: start of the DMA-backed pool region
- `entry_size`: fixed stride between packet objects
- `num_entries`: total number of packet buffers
- `free_stack_top` and `free_stack[]`: a simple LIFO free-stack allocator

The pool size formula is implemented in [`reflect_pool_entry_count()`](main.c#L299):

- `ICE_RX_DESC_COUNT`
- `+ tx_desc_count`
- `+ TX_BURST_SIZE` extra slack

### `struct io_ring_ctx`

Shared datapath identity and Rx state lives in [`struct io_ring_ctx`](main.c#L91).

Important fields:

- identity:
  - `mac`
  - `lport`
  - `vsi_num`
  - `rxq_id`
  - `qparent_teid`
- Rx ring memory:
  - `rx_desc`, `rx_desc_iova`
  - `rx_bufs`, `rx_bufs_iova`
- pooled Rx ownership:
  - `rx_pkt_bufs`
- ring progress:
  - `rx_ntc`

`rx_bufs` is used by the simple listen path.

`rx_pkt_bufs` is the critical zero-copy side array for reflect mode: descriptor slot `i` maps to the current pooled `pkt_buf*` owned by that Rx descriptor.

### `struct txq_ctx`

Per-Tx-queue state lives in [`struct txq_ctx`](main.c#L106).

Important fields:

- queue identity:
  - `txq_id`
  - `desc_count`
- ring memory:
  - `tx_desc`, `tx_desc_iova`
  - `tx_pkt_bufs`, `tx_pkt_iova`
- software indices:
  - `tx_next_to_use`
  - `tx_next_to_clean`
  - `tx_free`
  - `tx_pkts_since_rs`
- Tx ownership side array:
  - `tx_pkt_buf_refs`

`tx_pkt_bufs` is used by `--tx-send`, `--tx-bench`, and the legacy queue-owned path in [`tx_try_enqueue()`](main.c#L1296).

`tx_pkt_buf_refs` is what makes zero-copy reflection safe: when Tx borrows an Rx buffer, software remembers which `pkt_buf*` was queued in each descriptor slot, and [`tx_update_free()`](main.c#L1196) returns it to the pool only after hardware has completed that descriptor.

## VFIO, DMA, and Pool Initialization

### Discover and open VFIO

[`vfio_init()`](main.c#L475) performs:

- IOMMU group discovery
- container and group opens
- `VFIO_TYPE1_IOMMU` enablement
- BAR0 region discovery and `mmap()`
- PCI command register update to enable:
  - memory decoding
  - bus mastering

All direct register access after that goes through:

- `reg_read32()`
- `reg_write32()`

### Map DMA memory

[`dma_map()`](main.c#L533) allocates one large memory block, either from:

- anonymous memory
- or hugetlbfs if `--hugepages` is enabled

The key design choice is:

- `d->dma.iova = (uint64_t)(uintptr_t)d->dma.vaddr`

That works because VFIO explicitly maps that virtual range into the IOMMU with `VFIO_IOMMU_MAP_DMA`.

### Partition the DMA block

[`layout_dma()`](main.c#L561) partitions the single DMA mapping in this order:

1. ATQ descriptors
2. ARQ descriptors
3. ATQ buffers
4. ARQ buffers
5. Tx descriptors for all allocated Tx queues
6. Tx packet buffers for all allocated Tx queues
7. Rx descriptors
8. flat Rx packet buffers for listen mode
9. pooled packet buffer region for zero-copy reflect mode

The reflect mempool DMA region is placed at:

- [`layout_dma()` line 593](main.c#L593)

### Allocate software ownership metadata

[`alloc_queue_sw_state()`](main.c#L314) creates:

- `d->io.rx_pkt_bufs`
- `q->tx_pkt_buf_refs` for every allocated Tx queue
- `d->reflect_pool.free_stack`

These arrays are normal heap allocations. They are not DMA-visible. They exist purely so software can map descriptor slots back to `pkt_buf*`.

### Initialize the reflect mempool

[`pkt_pool_init()`](main.c#L337) walks the DMA pool region and initializes each packet object:

- computes the per-entry DMA address that points at `pkt_buf.data`
- stores `mempool` and `mempool_idx`
- zeroes `size`
- populates the free stack

Allocation and free are intentionally simple:

- allocate: [`pkt_buf_alloc()`](main.c#L356)
- free: [`pkt_buf_free()`](main.c#L371)

There is no refcounting. Ownership is single-owner at every step.

### How the mempool allocator actually works

The allocator is intentionally tiny and ixy-like.

Allocation path:

- [`pkt_buf_alloc()`](main.c#L356) decrements `free_stack_top`
- it reads one stable slot index from `free_stack[]`
- it converts that index into a `pkt_buf*` with [`pkt_pool_get_entry()`](main.c#L309)
- it resets `buf->size = 0`

Free path:

- [`pkt_buf_free()`](main.c#L371) looks at `buf->mempool`
- it pushes `buf->mempool_idx` back onto that pool’s `free_stack[]`
- it increments `free_stack_top`

Why this design matters:

- the buffer always knows which pool it came from
- the Tx completion path does not need to guess where to return it
- the recycle logic works even though the buffer was received on Rx and freed later from Tx completion

## AdminQ Control Plane

AdminQ hardware setup is done in [`adminq_hw_init()`](main.c#L623).

The main commands this driver uses are:

- [`aq_get_fw_ver()`](main.c#L744)
  - firmware and API version discovery
- [`aq_manage_mac_read()`](main.c#L763)
  - MAC address discovery
- [`aq_get_default_vsi_and_lport()`](main.c#L804)
  - default VSI and logical port discovery
- [`aq_get_qparent_teid()`](main.c#L863)
  - Tx scheduler parent discovery
- [`aq_add_rx_mac_rule()`](main.c#L922)
  - installs an Rx MAC steering rule to forward frames for the port MAC into the default VSI
- [`add_tx_queues()`](main.c#L1007)
  - creates Tx queues in firmware/scheduler space

This split is important:

- AdminQ is the control plane
- descriptor rings and MMIO tails/doorbells are the data plane

## Rx Queue Model

There are two Rx queue setup paths:

- flat-buffer Rx for listen mode: [`setup_and_enable_rxq()`](main.c#L1145)
- pool-backed Rx for reflect mode: [`setup_and_enable_rxq_pool()`](main.c#L1161)

Both paths share the same hardware queue enable sequence through [`enable_rxq()`](main.c#L1092):

- program `QRXFLXP_CNTXT`
- pack and write `QRX_CONTEXT`
- request queue enable with `QRX_CTRL`
- post the ring with `QRX_TAIL = ICE_RX_DESC_COUNT - 1`

### Listen mode Rx

The listen path keeps the older simple model:

- each descriptor points to one fixed `rx_bufs` slice
- [`poll_one_rx_packet()`](main.c#L1383) copies bytes out to a stack buffer
- [`rearm_rx_desc()`](main.c#L1363) reposts the same Rx DMA buffer

### Reflect mode Rx

The reflect path uses the ixy-style pool-backed model:

- each Rx descriptor is armed with a pooled `pkt_buf`
- `rx_pkt_bufs[idx]` tracks which pooled buffer belongs to slot `idx`
- on completion, software allocates a replacement buffer before giving the completed one to Tx

The descriptor completion scan itself is still done by [`poll_one_rx_desc()`](main.c#L1224):

- `DD` must be set
- `EOF` must be set
- `RXE` must be clear
- `pkt_len` must be non-zero

## Tx Queue Model

Tx queue creation is done through [`add_tx_queues()`](main.c#L1007), and software ring state is reset in [`tx_ring_init()`](main.c#L1181).

There are two Tx enqueue paths:

- queue-owned Tx buffers: [`tx_try_enqueue()`](main.c#L1296)
- zero-copy pooled buffers: [`tx_try_enqueue_pkt_buf()`](main.c#L1326)

Both enqueue styles share the same descriptor construction logic through:

- [`tx_try_reserve_slot()`](main.c#L1256)
- [`tx_prepare_desc()`](main.c#L1276)
- [`tx_commit_slot()`](main.c#L1268)
- [`tx_ring_doorbell()`](main.c#L1343)

Tx completion and recycle happens in [`tx_update_free()`](main.c#L1196):

- read `QTX_COMM_HEAD(q)`
- walk every completed slot from `tx_next_to_clean` to hardware head
- if that slot carried a pooled `pkt_buf*`, return it to the mempool with [`pkt_buf_free()`](main.c#L371)
- recompute `tx_free`

That is the key lifetime rule:

- a pooled buffer is not reusable when Tx accepts it
- it becomes reusable only when Tx completion advances past its descriptor slot

## Zero-Copy Reflect Memory Lifecycle

This section is the end-to-end ownership model for the current `--rx-reflect` implementation.

### 1. Pool metadata is allocated

Software metadata arrays are created in:

- [`alloc_queue_sw_state()`](main.c#L314)

The DMA pool region itself is reserved in:

- [`layout_dma()`](main.c#L593)

### 2. Pool entries are initialized

Each pooled packet object is initialized in:

- [`pkt_pool_init()`](main.c#L337)

At this point every buffer is:

- free
- self-describing
- known by stable index

### 3. RX ring ownership begins

Reflect mode arms the Rx ring with pooled buffers in:

- [`setup_and_enable_rxq_pool()`](main.c#L1161)

For each descriptor slot:

- allocate one `pkt_buf`
- store it in `rx_pkt_bufs[idx]`
- write `read.pkt_addr = buf->buf_addr_iova`

Ownership after this step:

- the pool no longer owns those buffers
- the Rx ring owns them on behalf of the NIC

### 4. Hardware receives into a pooled Rx buffer

When a packet arrives, hardware DMA-writes directly into the `pkt_buf.data` region for that descriptor.

Software detects completion in:

- [`poll_one_rx_desc()`](main.c#L1224)

No packet payload copy occurs here.

### 5. Software swaps in a fresh Rx buffer before handing off the old one

This is the most important zero-copy handoff step, implemented in:

- [`run_rx_reflect()`](main.c#L1691)
- [`run_rx_reflect()`](main.c#L1702)
- [`rearm_rx_desc()`](main.c#L1363)

The sequence is:

1. allocate a replacement buffer from the pool
2. remember the completed buffer in `rx_buf`
3. install the replacement into `rx_pkt_bufs[rx_idx]`
4. repost that replacement back to hardware with `rearm_rx_desc()`

After that repost:

- the completed `rx_buf` is no longer attached to the Rx ring
- the NIC can keep receiving with the replacement buffer
- software is free to edit and transmit `rx_buf`

### 6. The completed buffer is modified in place

The reflect logic rewrites only L2 addresses in:

- [`rewrite_reflect_l2()`](main.c#L1481)

The payload stays in the same DMA buffer.

The exact MAC updates happen at:

- [`main.c:1485`](main.c#L1485)
  - copy the original source MAC into a temporary stack buffer
- [`main.c:1486`](main.c#L1486)
  - write the reflected destination MAC from that original source MAC
- [`main.c:1487`](main.c#L1487)
  - write the reflected source MAC from the local NIC MAC

At the reflect call site, the rewrite happens here:

- [`run_rx_reflect()` call at `main.c:1706`](main.c#L1706)

### 7. The same buffer is queued directly to Tx

The completed Rx buffer is submitted to Tx in:

- [`tx_try_enqueue_pkt_buf()`](main.c#L1326)
- [`run_rx_reflect()`](main.c#L1710)

The Tx descriptor gets:

- `buf_addr = buf->buf_addr_iova`

and the software side array gets:

- `tx_pkt_buf_refs[idx] = buf`

This is the actual zero-copy handoff.

The exact Tx handoff happens immediately after the MAC rewrite:

- MAC rewrite call: [`main.c:1706`](main.c#L1706)
- zero-copy Tx handoff call: [`main.c:1710`](main.c#L1710)

Inside [`tx_try_enqueue_pkt_buf()`](main.c#L1326), the transfer into the Tx ring is:

- [`main.c:1338`](main.c#L1338)
  - program the Tx descriptor with `buf->buf_addr_iova`
- [`main.c:1339`](main.c#L1339)
  - store the same `pkt_buf*` in `tx_pkt_buf_refs[idx]` so completion can recycle it later

So the hot-path order is:

1. receive into pooled Rx buffer
2. allocate replacement Rx buffer
3. repost replacement to Rx
4. rewrite MACs in the completed buffer at [`main.c:1706`](main.c#L1706)
5. hand the same buffer to Tx at [`main.c:1710`](main.c#L1710)
6. later recycle that same buffer from Tx completion in [`tx_update_free()`](main.c#L1196)

### 8. Hardware transmits from that same DMA buffer

Once [`tx_ring_doorbell()`](main.c#L1343) rings the queue, the NIC DMA-reads from the exact same memory region that Rx originally filled.

No payload copy happens between Rx and Tx.

### 9. TX completion returns the buffer to the pool

On later progress, [`tx_update_free()`](main.c#L1196) walks completed Tx slots and frees pooled buffers with:

- [`pkt_buf_free()`](main.c#L371)

Ownership path for a reflected frame is therefore:

- free in pool
- armed on Rx
- filled by hardware
- detached from Rx after replacement install
- queued on Tx
- reclaimed on Tx completion
- free in pool again

### 10. What is still not zero-copy

The queue-owned Tx helper is still used by:

- `--tx-send`
- `--tx-bench`

That path lives in [`tx_try_enqueue()`](main.c#L1296) and still transmits from `q->tx_pkt_bufs`.

So the zero-copy guarantee currently applies to:

- `--rx-reflect`

not to the generic Tx generators.

## Operational Modes

### `--rx-listen`

Implementation:

- [`run_rx_listen()`](main.c#L1490)

Behavior:

- creates and enables one Rx queue
- installs an Rx MAC forwarding rule
- waits for the first packet
- copies the packet into a local buffer and dumps the payload

Use this mode when:

- you want to validate that packets reach a userspace-programmed Rx queue

### `--rx-reflect`

Implementation:

- [`run_rx_reflect()`](main.c#L1566)

Behavior:

- creates and enables one pooled Rx queue
- installs an Rx MAC forwarding rule
- creates one Tx queue
- polls for completed Rx descriptors
- swaps in a fresh Rx pool buffer
- rewrites only Ethernet source and destination MACs in place
- queues the same DMA buffer directly to Tx
- reclaims buffers only after Tx completion

This is now a zero-copy reflect path.

### Interpreting a tcpdump capture

If you see a trace like this:

- inbound:
  - `40:a6:b7:c3:41:20 > 40:a6:b7:c2:d4:88`
  - length `24`
  - payload prefix `mai_aur_tu`
- outbound:
  - `40:a6:b7:c2:d4:88 > 40:a6:b7:c3:41:20`
  - length `60`
  - same payload prefix `mai_aur_tu`

then the reflector is behaving correctly.

What that means:

- destination and source MACs were swapped as expected
- EtherType stayed the same
- the payload prefix survived reflection
- the transmitted frame was padded to the Ethernet minimum frame size

The `24 -> 60` jump is normal for short Ethernet frames:

- the logical payload is still the short packet you sent
- hardware pads transmit frames shorter than 60 bytes without FCS
- tcpdump on the receiving side shows the padded frame length

In other words, that capture is a success, not a bug.

### `--tx-send`

Implementation:

- [`run_tx_send()`](main.c#L1768)

Behavior:

- creates Tx queue(s)
- builds one Ethernet frame template
- sends it `count` times with optional interval

This path uses queue-owned Tx packet buffers through [`tx_try_enqueue()`](main.c#L1296).

### `--tx-bench`

Implementation:

- [`run_tx_bench()`](main.c#L1928)

Behavior:

- creates one or more Tx queues
- pre-populates each queue’s Tx packet buffers
- spawns one worker thread per active Tx queue
- repeatedly submits bursts of `TX_BURST_SIZE = 64`
- optionally pins threads with `--pin-cpus`

This is the best mode for stressing the direct userspace Tx datapath.

## Build and Run Checklist

### 1. Build

```bash
make
```

### 2. Prepare the system

You need:

- IOMMU enabled
- the NIC bound to `vfio-pci`
- root privileges or equivalent VFIO access
- a hugetlbfs mount only if you want `--hugepages`

Typical boot args:

```bash
intel_iommu=on iommu=pt
```

Typical hugepage mount:

```bash
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

### 3. Bind the NIC to `vfio-pci`

```bash
sudo dpdk-devbind.py -b vfio-pci 0000:17:00.0
```

### 4. Run listen mode

```bash
sudo ./my_ice 0000:17:00.0 --rx-listen 60
```

### 5. Run reflect mode

On the DUT:

```bash
sudo ./my_ice <BDF> --rx-reflect 60
```

On the traffic source:

```bash
sudo python3 ./send_l2.py \
  -i <node-1-iface> \
  -d <node-0-my_ice-mac> \
  -s 40:a6:b7:c3:41:20 \
  -p hello-reflect \
  -c 20 \
  -t 0.1
```

To watch the reflected traffic:

```bash
sudo tcpdump -enn -i <node-1-iface> ether proto 0x88b5
```

### 6. Run simple transmit mode

```bash
sudo ./my_ice <BDF> --tx-send <dst-mac> 20 100 hello
```

### 7. Run throughput mode

```bash
sudo ./my_ice <BDF> --tx-bench 15 <dst-mac> 1472 --tx-queues 4 --pin-cpus --hugepages --hugepage-dir /mnt/huge
```

## Mental Model to Keep in Your Head

The easiest way to reason about this driver is:

- AdminQ discovers and creates things.
- BAR0 registers enable and ring things.
- Descriptor rings tell hardware where buffers live.
- Side arrays tell software which packet object each descriptor slot owns.
- In `--rx-reflect`, the same pooled packet buffer moves through:
  - pool
  - Rx ring
  - reflect logic
  - Tx ring
  - pool again

If you keep that ownership chain in mind while reading the code, the rest of the datapath becomes much easier to follow.

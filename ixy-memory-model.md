# ixy Memory Model and Packet Buffer Lifecycle

This document explains how the current C implementation of `ixy` manages DMA memory, packet buffers, descriptor rings, and zero-copy forwarding.

The emphasis is on the physical NIC path in the `ixgbe` driver, because that is the closest match to building a userspace driver for an Intel NIC such as the E810. The generic buffer allocator lives in `src/memory.c`; the concrete RX/TX lifecycle described here is implemented in `src/driver/ixgbe.c`; the reflect example comes from `src/app/ixy-fwd.c`.

## Scope and Mental Model

`ixy` uses three different kinds of memory objects:

1. Descriptor ring memory
   RX and TX rings are separate DMA allocations, created once per queue and kept for the lifetime of the process.
2. Packet-buffer pool memory
   A mempool is another long-lived DMA allocation that is subdivided into many fixed-size packet buffers.
3. Software bookkeeping memory
   Queue structs, mempool metadata, and software-only arrays such as `virtual_addresses[]` live in normal process memory.

The design is intentionally simple:

- Packets are stored in fixed-size DMA-capable buffers.
- RX descriptors always point at buffers that are already armed for the NIC.
- When RX completes, the driver swaps in a fresh buffer and hands the completed one to the application.
- The application can either free the buffer or pass the same pointer to TX.
- TX completion is asynchronous, so buffers are not returned to the pool until the NIC reports completion.

The result is a zero-copy data path for forwarding: the packet payload is never copied between RX and TX. Only pointer ownership changes.

## Core Data Structures

### `struct pkt_buf`

`pkt_buf` is the fundamental packet object:

```c
struct pkt_buf {
	uintptr_t buf_addr_phy;
	struct mempool* mempool;
	uint32_t mempool_idx;
	uint32_t size;
	uint8_t head_room[40];
	uint8_t data[] __attribute__((aligned(64)));
};
```

Important properties:

- `buf_addr_phy` is the address the NIC must DMA to or from.
- `mempool` points back to the owning pool.
- `mempool_idx` is the stable slot number inside that pool.
- `size` is the current packet length in bytes.
- `head_room` reserves 40 bytes immediately before `data`.
- `data` starts at offset 64, so the metadata plus headroom occupy exactly one cache line.

Two practical consequences matter a lot for a driver design:

- The packet metadata and the DMA payload storage live in the same allocation.
- The DMA address used in descriptors is not the start of `pkt_buf`; it is `buf_addr_phy + offsetof(struct pkt_buf, data)`.

### `struct mempool`

The mempool is a fixed-size allocator for `pkt_buf`s:

```c
struct mempool {
	void* base_addr;
	uint32_t buf_size;
	uint32_t num_entries;
	uint32_t free_stack_top;
	uint32_t free_stack[];
};
```

This is a simple LIFO free-stack allocator:

- `base_addr` points at the start of the DMA-backed packet-buffer area.
- `buf_size` is the fixed stride between buffers.
- `free_stack[]` stores free entry indices, not pointers.
- `free_stack_top` is the number of currently free buffers.

There is no per-buffer allocation metadata beyond what is embedded in each `pkt_buf`.

### DMA allocation wrapper

`memory_allocate_dma()` returns:

```c
struct dma_memory {
	void* virt;
	uintptr_t phy;
};
```

`virt` is the CPU-visible virtual address.

`phy` is the address that the device should use:

- Without VFIO/IOMMU, it is the physical address.
- With VFIO, it is an IOVA created by `vfio_map_dma()`.

For the rest of the driver, both cases are treated the same: descriptor programming always uses `phy`.

### RX/TX queue side arrays

Both ixgbe RX and TX queues maintain a `virtual_addresses[]` array indexed by descriptor slot.

That array is essential because descriptors only carry a DMA address. They do not give software a direct way to recover the original `pkt_buf*`.

So ixy keeps two parallel views:

- hardware-visible descriptor ring: DMA addresses and device control fields
- software-visible side array: the `pkt_buf*` currently associated with each ring slot

That mapping is what makes later handoff and free operations possible.

## DMA Addressing Model

### Without VFIO/IOMMU

`memory_allocate_dma()` allocates hugepage-backed memory from `/mnt/huge`, locks it with `mlock()`, and translates virtual addresses to physical addresses through `/proc/self/pagemap`.

Important implications:

- Descriptor rings requested as `require_contiguous = true` must fit in a single 2 MiB huge page.
- Mempools are allowed to span multiple huge pages.
- Because the whole pool is not assumed physically contiguous, `memory_allocate_mempool()` computes `buf_addr_phy` for each individual buffer.

That per-buffer physical address is why descriptors can point at any packet buffer even if the pool spans multiple huge pages.

### With VFIO/IOMMU

When VFIO is active:

- memory is still allocated with huge pages
- `vfio_map_dma()` creates an IOVA mapping
- ixy intentionally chooses `iova == vaddr`

That means:

- `dma_memory.phy` is actually an IOVA
- each `pkt_buf->buf_addr_phy` is just the virtual address cast to an integer
- descriptors still use `buf_addr_phy + data_offset`, but now that value is an IOVA, not a host physical address

One important design detail is the global VFIO container shared across NICs. This lets every NIC in the process DMA to memory allocated for every other NIC, including mempools. That is the mechanism that makes cross-port zero-copy forwarding work cleanly under VFIO.

## How ixy Creates and Initializes a Memory Pool

`memory_allocate_mempool(num_entries, entry_size)` builds a packet-buffer pool in five steps.

### 1. Choose the buffer size

If `entry_size == 0`, ixy uses `2048`.

In the ixgbe path, RX mempools use `PKT_BUF_ENTRY_SIZE = 2048`.

Because `pkt_buf` metadata consumes 64 bytes, this leaves:

- 64 bytes metadata plus headroom
- 1984 bytes of actual packet storage from `data[]`

That is enough for standard Ethernet frames, but not for jumbo frames.

### 2. Validate layout constraints

In non-VFIO mode, `entry_size` must divide the 2 MiB huge page size exactly.

The reason is simple: ixy computes buffer addresses as:

```text
base_addr + index * buf_size
```

This matters because a `pkt_buf` is handed to hardware using one DMA base address plus an offset into `data[]`.

Without VFIO, ixy does not assume consecutive huge pages are physically contiguous. So each fixed-size entry must fit entirely within one huge page. If an entry crossed a hugepage boundary, one `buf_addr_phy` would no longer describe the whole DMA region safely.

### 3. Allocate mempool metadata and the backing DMA region

The `struct mempool` itself is allocated with `malloc()`.

The actual packet-buffer storage is allocated as one DMA-backed region:

```text
num_entries * entry_size
```

That DMA region becomes `mempool->base_addr`.

### 4. Initialize the free stack

The free stack is filled with every entry index from `0` to `num_entries - 1`.

At this moment:

- all buffers are free
- `free_stack_top == num_entries`

### 5. Initialize each `pkt_buf`

For every slot:

- compute the virtual address of the buffer as `base_addr + i * entry_size`
- compute or assign the device-visible DMA address
- store `mempool_idx = i`
- store `mempool = mempool`
- initialize `size = 0`

From that point on, every buffer is self-describing:

- it knows where it came from
- it knows how to return itself
- it already carries the address the NIC should DMA against

## How Packet Buffers Are Allocated from the Pool

Allocation is intentionally minimal.

### Batch allocation

`pkt_buf_alloc_batch()`:

- checks whether enough free entries exist
- if not, returns only the number currently available
- pops indices from the top of `free_stack`
- converts each index to a `pkt_buf*`

There is no buffer initialization on allocation:

- the payload is not cleared
- the old `size` is not reset here
- the caller is expected to overwrite what matters

That behavior is intentional and visible in `ixy-pktgen`, which pre-initializes packet templates, frees them, and later reallocates the same buffers with their contents still present.

### Single-buffer allocation

`pkt_buf_alloc()` is just a 1-buffer wrapper around the batch allocator.

### Free

`pkt_buf_free()` does not inspect the pointer value at all. It simply does:

- `mempool = buf->mempool`
- push `buf->mempool_idx` back onto that pool's free stack

This is a crucial property:

- a buffer always returns to its original mempool
- the free path does not care which device or queue used it most recently

That is exactly what enables forwarding a buffer received from one NIC and later freeing it from another NIC's TX completion path.

## Relationship Between Mempool, Descriptors, and DMA Packet Data

The layout looks like this:

```text
mempool DMA region
  -> pkt_buf[0]
       -> metadata (buf_addr_phy, mempool, mempool_idx, size, head_room)
       -> data[...]
  -> pkt_buf[1]
       -> metadata
       -> data[...]
  -> ...

RX descriptor ring DMA region
  -> descriptor slot i contains DMA address of pkt_buf.data
  -> software side array slot i contains pkt_buf*

TX descriptor ring DMA region
  -> descriptor slot i contains DMA address of pkt_buf.data
  -> software side array slot i contains pkt_buf*
```

Key point:

- the descriptor points at the packet payload
- the side array points at the full software object

Hardware never sees the software pointer.
Software never relies on recovering a pointer from the DMA address.

## RX Queue Initialization

The ixgbe RX path uses two separate initialization stages.

### Stage 1: allocate the RX descriptor ring

`init_rx()` allocates a DMA region for the descriptor ring, programs the NIC registers with its base address and length, and stores the CPU pointer in `queue->descriptors`.

This ring allocation is not part of the mempool.

### Stage 2: pre-fill every RX descriptor with a packet buffer

`start_rx_queue()` then creates one mempool per RX queue and allocates one `pkt_buf` per descriptor slot.

For each descriptor slot:

1. allocate a buffer from the queue's mempool
2. program `rxd->read.pkt_addr` with `buf->buf_addr_phy + offsetof(struct pkt_buf, data)`
3. set `rxd->read.hdr_addr = 0`
4. store `queue->virtual_addresses[i] = buf`

After all descriptors are armed, the driver enables the queue and sets `RDT` so the hardware can start receiving into the full ring.

At this point, ownership is:

- mempool no longer owns these buffers
- RX descriptors own them on behalf of the NIC
- the application has not seen them yet

## What Happens When Packets Are Received

`ixgbe_rx_batch()` implements the receive fast path.

The driver loops starting at `queue->rx_index` and checks each descriptor's DD bit.

For each completed descriptor:

### 1. Confirm the packet is single-buffer

If EOP is not set, ixy aborts with an error.

So the current design assumes:

- one packet per descriptor
- one descriptor per packet
- one fixed-size buffer per packet

### 2. Recover the completed packet buffer

The driver looks up:

```text
buf = queue->virtual_addresses[rx_index]
```

That `buf` is the software object whose `data[]` region the NIC just filled.

The descriptor is copied locally and:

```text
buf->size = desc.wb.upper.length
```

No packet bytes are copied.

### 3. Allocate a replacement buffer immediately

Before giving the completed packet to the application, the driver allocates a fresh buffer from the same mempool.

This replacement becomes the new RX target for that descriptor slot.

### 4. Re-arm the descriptor

The driver writes:

- new packet DMA address into `desc_ptr->read.pkt_addr`
- `hdr_addr = 0` to clear flags
- `queue->virtual_addresses[rx_index] = new_buf`

Now the ring slot once again has a valid buffer for future DMA.

### 5. Hand the completed buffer to the application

The old buffer pointer is stored in the caller-provided `bufs[]` array.

Ownership has now moved from the RX ring to the application.

### 6. Publish replenished descriptors back to hardware

After processing a batch, the driver updates `RDT` with the last replenished slot index.

That tells hardware those descriptor entries are ready again.

### Why this is zero-copy

The data flow is:

- NIC DMA writes directly into `pkt_buf.data`
- driver swaps descriptor ownership by pointer
- application receives the same `pkt_buf*`

There is no copy from descriptor memory into a second packet buffer and no copy from RX storage into application storage.

## Ownership Model on RX

The RX-side ownership transitions are:

1. Free in mempool
2. Allocated and attached to an RX descriptor
3. Owned by the NIC for DMA fill
4. RX completion observed by driver
5. Descriptor immediately re-armed with a different fresh buffer
6. Completed original buffer returned to application

The important invariant is:

- once `ixgbe_rx_batch()` returns a `pkt_buf*`, that buffer is no longer attached to the RX ring

So the application is free to:

- inspect it
- modify it
- transmit it
- or free it

without racing the RX hardware for that same buffer.

## How RX-to-TX Reflect Works

The reflect path in `src/app/ixy-fwd.c` is the cleanest example of ixy's ownership model.

The application:

1. calls `ixy_rx_batch()` and receives an array of `pkt_buf*`
2. optionally edits packet contents in place
3. passes the same pointers to `ixy_tx_batch()`
4. frees only the tail that could not be queued

No packet payload copy happens between RX and TX.

### What is actually reused

The following things are reused directly:

- the same `pkt_buf` object
- the same DMA-backed packet data in `data[]`
- the same `buf_addr_phy`

The only thing that changes is which ring slot currently references that buffer.

### Why cross-device forwarding works

Suppose port A receives into a mempool owned by A's RX queue, and the application forwards that buffer out port B.

That still works because:

- the buffer's `buf_addr_phy` is globally valid for DMA
- port B's TX descriptor can point at that same address
- after TX completion, the buffer returns to its original mempool through `buf->mempool`

So the TX device does not need to allocate from its own mempool for forwarding.

This is an important design choice:

- packet ownership is attached to the buffer object
- not to the current device
- not to the current queue

## What Happens When TX Is Requested

`ixgbe_tx_batch()` has two logically separate phases:

1. cleanup of previously completed transmissions
2. enqueue of new transmissions

### Phase 1: clean completed TX descriptors

TX is asynchronous. Once the application hands a buffer to TX, the NIC may still be reading from it later.

So the driver must not free a buffer at enqueue time.

Instead, on each `ixgbe_tx_batch()` call, the driver checks whether previously queued descriptors have completed.

The bookkeeping uses:

- `tx_index`: where the next packet will be inserted
- `clean_index`: the next descriptor slot that still needs cleanup

ixy cleans in batches of `TX_CLEAN_BATCH = 32`.

For each cleanup attempt:

1. compute how many outstanding descriptors exist between `clean_index` and `tx_index`
2. if fewer than 32 are outstanding, stop cleaning
3. look at the last descriptor in that 32-descriptor block
4. if that descriptor has DD set, free all 32 associated `pkt_buf*`
5. advance `clean_index`

Why checking only the last descriptor is enough:

- TX completion is ordered
- if the last descriptor in a block is done, all earlier descriptors in that block are also done

The free operation is:

```text
buf = queue->virtual_addresses[i]
pkt_buf_free(buf)
```

So completed transmitted buffers go back to their original mempool.

### Phase 2: enqueue new TX work

For each caller-provided `pkt_buf*`:

1. check whether the ring has space
2. store the software pointer in `queue->virtual_addresses[tx_index]`
3. fill the TX descriptor with `buffer_addr = buf->buf_addr_phy + data_offset`
4. program length and command flags
5. advance `tx_index`

After filling descriptors, the driver writes `TDT`.

That is the point where ownership transfers to the NIC.

From that moment until cleanup:

- the application must not free the buffer
- the application must not modify the payload
- the buffer is considered in flight

## How DMA Memory Becomes Reusable After Transmission

DMA memory does not become reusable just because `ixy_tx_batch()` accepted the pointer.

The actual sequence is:

1. application passes `pkt_buf*` to TX
2. TX descriptor points at that buffer's `data[]`
3. NIC DMA-reads the packet payload asynchronously
4. NIC writes DD status into the TX descriptor when done
5. a later `ixgbe_tx_batch()` call observes DD
6. the driver calls `pkt_buf_free(buf)`
7. the mempool free stack receives the buffer index again
8. a future allocation can return the same buffer

So reuse is software-driven, not hardware-driven.

The NIC never "returns" buffers directly. It only reports descriptor completion. The driver interprets that completion and recycles the software object.

## Ownership Model Across the Full Lifecycle

This is the full state machine for a forwarded packet buffer:

1. `Free`
   The buffer index is on `mempool->free_stack`.
2. `Armed on RX`
   An RX descriptor points at the buffer's `data[]`. The RX queue side array points at the `pkt_buf*`.
3. `Received`
   The NIC has DMA-written packet bytes into the buffer and marked the descriptor done.
4. `Application-owned`
   `ixgbe_rx_batch()` swaps in a replacement buffer and returns the completed original buffer pointer.
5. `Queued on TX`
   `ixgbe_tx_batch()` places that same pointer into a TX ring entry and advances `TDT`.
6. `In flight`
   The NIC may still read from the buffer.
7. `TX completed`
   The NIC sets DD on the descriptor.
8. `Freed back to original pool`
   A later TX cleanup batch calls `pkt_buf_free()` using the stored pointer.
9. `Reusable`
   A future allocator pop can hand the same buffer out again.

For a packet that is not forwarded:

- RX-to-application is the same
- after processing, the application directly calls `pkt_buf_free()`
- the TX stages are skipped

For a packet that TX could not enqueue:

- `ixy_tx_batch()` returns fewer than requested
- the unsent suffix remains application-owned
- the application must either retry or free those buffers itself

`ixy-fwd` chooses to free them immediately to avoid queueing latency.

## Important Invariants, Constraints, and Assumptions

### Fixed-size buffers

Each mempool is fixed-stride. There is no variable-length allocation.

Implication:

- the driver is simple
- but large packets require larger entries or multi-buffer support

### Single-segment packets only

RX aborts if EOP is not set.

Implication:

- no scatter RX
- no chained mbufs
- no jumbo-frame support in the current ixgbe path

### `2048`-byte entry size is tuned for normal MTU

With 64 bytes consumed before `data[]`, only 1984 bytes remain for packet payload.

That is fine for standard Ethernet frames and some metadata headroom, but it is not a generic solution for larger frames.

### No reference counting

A `pkt_buf` has exactly one logical owner at a time:

- mempool
- RX ring/NIC
- application
- TX ring/NIC

There is no cloning, shared ownership, or refcount-based lifetime.

### Free returns to the buffer's home pool

`pkt_buf_free()` always uses `buf->mempool`.

Implication:

- forwarding across ports works naturally
- but the mempool must outlive every outstanding reference to its buffers

### Mempools are not thread-safe

The free stack is just a plain stack in shared memory with no locking.

Implication:

- a pool can only be used safely by one thread
- the current design assumes a packet is allocated and freed on the same thread
- multi-stage cross-thread pipelines need a different pool design or an explicit thread-safe handoff structure

### Queue sizes must be powers of two

The ring wrap helper relies on:

```text
(index + 1) & (ring_size - 1)
```

So ring sizes must be powers of two.

### TX reclamation is lazy and batched

Completed TX buffers are reclaimed only:

- when `ixgbe_tx_batch()` is called again
- and only in 32-descriptor batches

Implications:

- stopping TX with fewer than 32 outstanding descriptors can delay reclamation
- short bursts may leave buffers in flight longer than strictly necessary
- this is a deliberate simplicity/performance tradeoff

### RX refill must succeed

On RX completion, ixy immediately allocates a replacement buffer.

If that allocation fails, the driver aborts.

Implication:

- mempools must be sized for the worst case
- enough free buffers must exist for all RX descriptors plus all buffers temporarily held by the application or TX path

That is why `start_rx_queue()` allocates at least `max(NUM_RX_QUEUE_ENTRIES + NUM_TX_QUEUE_ENTRIES, MIN_MEMPOOL_ENTRIES)` buffers.

### Descriptor rings and packet memory are long-lived

ixy allocates DMA memory once at initialization and keeps it for the process lifetime.

There is no per-packet DMA map/unmap.

Implication:

- fast steady-state operation
- simple ownership tracking
- process lifetime effectively becomes DMA-region lifetime

### Buffers are reused in place

Freeing a buffer does not scrub payload bytes or reset all metadata.

Implication:

- callers must set `size` and any packet contents they care about before transmit
- reuse can preserve packet templates, which `ixy-pktgen` intentionally exploits

## Zero-Copy Behavior Summary

The main zero-copy behaviors in ixy are:

- RX delivers the same DMA-filled `pkt_buf` to the application.
- Reflect forwarding passes the same `pkt_buf*` from RX to TX.
- TX descriptors point directly at that original packet data.
- TX completion returns the same buffer object to the mempool.

What is not zero-copy:

- descriptor metadata is still rewritten on RX refill and TX enqueue
- the application may still copy payload data if it wants to
- `ixy-pcap` copies packet bytes into a file before freeing the buffer

But the core forwarding path is zero-copy with respect to packet payload memory.

## Design Lessons for a `my_ice` / Intel E810 Userspace Driver

The exact descriptor formats on E810 will be different, but the reusable ideas from ixy are:

### 1. Keep packet objects self-describing

Each packet buffer should carry:

- device-visible DMA address or IOVA
- pool back-pointer
- pool slot/index
- current data length

That makes recycling independent of which queue currently holds the buffer.

### 2. Separate hardware ring state from software ownership state

Do not rely on descriptors alone to recover software objects.

Keep a side array from descriptor slot to packet-buffer pointer. That is the simplest way to:

- hand completed RX buffers to software
- free completed TX buffers later
- preserve zero-copy forwarding

### 3. Refill RX immediately before returning packets

The ixy RX pattern is excellent for a polling driver:

- consume completed RX descriptor
- grab its software buffer pointer
- install a fresh replacement
- only then return the completed buffer upward

That cleanly separates NIC-owned memory from application-owned memory.

### 4. Free after TX completion, not after TX enqueue

This is the most important lifecycle rule in a userspace NIC driver.

The transmit API may have accepted the packet, but the NIC can still be DMA-reading from it later.

Your E810 driver will need a completion path that delays recycling until hardware completion is visible.

### 5. Preserve the ownership model if you want zero-copy reflect

For zero-copy forwarding, the buffer must move through these owners:

```text
RX ring -> application -> TX ring -> mempool
```

Do not copy packet data unless your hardware model forces it.

### 6. Revisit the limitations deliberately

For an E810-class driver, you will likely want to improve on ixy in at least these areas:

- multi-segment packet support
- jumbo frames
- thread-safe mempools or per-core pools
- more explicit TX completion handling
- better buffer accounting for long-lived outstanding packets
- explicit cleanup/destruction of DMA mappings

If you keep ixy's ownership rules while upgrading those capabilities, the design will still stay understandable.

## Short Implementation Checklist

If you want to reproduce the ixy model in `my_ice`, the minimal rules are:

1. Allocate long-lived DMA descriptor rings per queue.
2. Allocate long-lived DMA packet-buffer pools with fixed-size entries.
3. Embed DMA address, pool pointer, and pool index inside each packet object.
4. Keep a software pointer array parallel to each ring.
5. On RX completion, swap in a fresh buffer before returning the old one.
6. On reflect, pass the same buffer pointer into TX.
7. Treat TX enqueue as ownership transfer to hardware.
8. Recycle buffers only after TX completion is observed.
9. Return buffers to their original pool, not to the current device.
10. Preserve enough free buffers to cover RX, TX, and application-held packets.

That is the essence of ixy's packet-buffer lifecycle.

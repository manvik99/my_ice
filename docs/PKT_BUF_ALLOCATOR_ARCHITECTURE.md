# Packet Buffer Allocator Architecture

This document explains the packet-buffer allocator used by `--rx-reflect`.

It is specifically about the software ring buffer used to hand out fresh
`pkt_buf` objects and reclaim them later after TX completion. This is not the
hardware RX or TX descriptor ring. It is a separate software allocator that
tracks which pooled packet buffers are free.

If you want the full RX/TX reflect datapath around this allocator, also read:

- [`RX_REFLECT_BATCHING_DEEP_DIVE.md`](RX_REFLECT_BATCHING_DEEP_DIVE.md)
- [`DRIVER_WORKFLOW.md`](DRIVER_WORKFLOW.md)

## 1. Why This Allocator Exists

Reflect mode needs a fast way to do two things:

1. give RX a fresh packet buffer when software detaches a completed one
2. reclaim detached packet buffers only after TX hardware has finished with them

This means a buffer can move through several owners:

- free pool
- RX ring
- software
- TX ring
- free pool again

The allocator is the mechanism that tracks the `free pool` part of that cycle.

It must be:

- simple
- deterministic
- O(1)
- cheap enough to use in the packet hot path

## 2. What Object Is Actually Being Allocated

The allocator manages `struct pkt_buf`, defined in [`main.c`](../main.c#L57).

Important fields:

- `buf_addr_iova`
  - the IOVA of the `data` payload region
  - this is what RX and TX descriptors use
- `mempool`
  - back-pointer to the owning pool
- `mempool_idx`
  - stable slot number inside the pool
- `size`
  - logical packet length currently stored in `data`
- `data[]`
  - packet bytes

The allocator does not move packet payloads around. It only hands out and
reclaims ownership of these fixed packet-buffer objects.

## 3. The Main Data Structure

The allocator state lives in `struct pkt_mempool`, defined in
[`main.c`](../main.c#L66).

Fields:

- `base`
  - start of the DMA-backed pool memory
- `base_iova`
  - IOVA of that pool memory
- `entry_size`
  - fixed byte stride between pool entries
- `num_entries`
  - number of `pkt_buf` entries in the pool
- `free_head`
  - next free index to allocate
- `free_tail`
  - next slot where a returned index is appended
- `free_count`
  - number of entries currently free
- `free_ring`
  - circular queue of free `mempool_idx` values

The important thing to notice is:

- `free_ring` stores indices, not packet bytes and not pointers

So this allocator is really:

- a circular queue of stable packet-buffer indices

## 4. Two Different Kinds of Rings

This distinction matters a lot because the word "ring" appears several times in
the driver.

### 4.1 Hardware Descriptor Rings

The NIC has:

- an RX descriptor ring
- a TX descriptor ring

Those rings are DMA-visible and consumed by hardware.

### 4.2 Software Allocator Ring

The allocator also has:

- `free_ring`

That ring is software-only. Hardware never reads it.

It exists only so software can answer:

- which `pkt_buf` objects are free right now?

So when we say "ring buffer" in this document, we mean:

- the software free-index ring inside `struct pkt_mempool`

not the NIC descriptor rings.

## 5. Memory Layout of the Pool

The pool entry stride is defined by:

- [`ICE_PKT_BUF_ENTRY_SIZE`](../main.c#L77)

which is:

```c
ALIGN_UP(offsetof(struct pkt_buf, data) + ICE_PKT_BUF_DATA_SIZE, 64)
```

This means each entry contains:

- `struct pkt_buf` metadata
- the packet `data[]` payload region
- alignment padding up to 64 bytes

That gives each pool entry a fixed addressable slot.

The helper that turns a slot index into an object is:

- [`pkt_pool_get_entry()`](../main.c#L336)

It computes:

```text
base + idx * entry_size
```

So the allocator never asks the heap for individual packet buffers. All packet
buffers live in one pre-reserved DMA region, and the allocator just hands out
indices into that region.

## 6. Pool Sizing Formula

The total number of entries is computed by:

- [`reflect_pool_entry_count()`](../main.c#L326)

Current formula:

```text
ICE_RX_DESC_COUNT + tx_desc_count + ICE_REFLECT_POOL_EXTRA
```

This is designed so the pool has enough buffers to cover:

- all RX descriptors currently armed
- all TX descriptors that may temporarily hold detached buffers
- extra working slack for the reflect path

This sizing logic matters because a zero-copy design needs spare replacement
buffers. If all buffers are already sitting on RX and TX descriptors, RX cannot
be rearmed after a completion.

The total DMA bytes reserved for the pool are computed by:

- [`reflect_pool_dma_bytes()`](../main.c#L331)

which is:

```text
num_entries * ICE_PKT_BUF_ENTRY_SIZE
```

## 7. What the Ring Actually Stores

`free_ring` does not store:

- packet data
- descriptor contents
- ownership flags
- arbitrary pointers

It stores exactly one thing:

- the stable `mempool_idx` of a free entry

This design has several benefits:

- indices are compact
- indices remain valid even if the caller only later converts them to pointers
- `pkt_buf_free()` can recycle in O(1) using `buf->mempool_idx`
- the allocator never needs to search for where a buffer belongs

That last point is especially important. The buffer itself remembers which pool
it came from and which index it occupies.

## 8. Ring State Variables

The ring is defined by three moving pieces:

- `free_head`
- `free_tail`
- `free_count`

### 8.1 `free_head`

This points to the next free index that will be handed out on allocation.

Allocation consumes:

- `free_ring[free_head]`

then advances `free_head` with wraparound.

### 8.2 `free_tail`

This points to the next slot where a recycled index is appended.

Freeing a buffer writes:

- `free_ring[free_tail] = buf->mempool_idx`

then advances `free_tail` with wraparound.

### 8.3 `free_count`

This tracks how many free entries exist right now.

It is used for:

- empty detection on allocation
- full detection on free

This is important because head and tail alone are ambiguous in circular queues:

- `head == tail` could mean empty
- or it could mean full

By carrying `free_count`, the allocator avoids that ambiguity.

## 9. Initialization

Initialization happens in two stages.

### 9.1 Metadata Allocation

[`alloc_queue_sw_state()`](../main.c#L341) allocates:

- `d->io.rx_pkt_bufs`
- `q->tx_pkt_buf_refs`
- `d->reflect_pool.free_ring`

These are normal software metadata arrays, not DMA-visible memory.

### 9.2 Pool Population

[`pkt_pool_init()`](../main.c#L364) initializes the whole pool.

For each entry `i`, it:

1. computes the packet data IOVA
2. stores `buf->mempool = pool`
3. stores `buf->mempool_idx = i`
4. sets `buf->size = 0`
5. writes `free_ring[i] = i`

After the loop it sets:

- `free_head = 0`
- `free_tail = 0`
- `free_count = num_entries`

This means:

- every pool entry is free
- allocation will begin from slot `0`
- the queue is logically full of free entries

## 10. Allocation Path

Single-buffer allocation is implemented by:

- [`pkt_buf_alloc()`](../main.c#L385)

The logic is:

1. if `pool == NULL`, fail
2. if `free_count == 0`, fail
3. read `idx = free_ring[free_head]`
4. advance `free_head`, wrapping to `0` if needed
5. decrement `free_count`
6. convert `idx` to `pkt_buf *` with `pkt_pool_get_entry()`
7. set `buf->size = 0`
8. return the buffer

This is O(1).

There is:

- no search
- no malloc
- no free-list traversal
- no memmove

The allocator only reads one ring slot, updates one index, updates one counter,
and computes one pointer.

## 11. Batch Allocation Path

Burst allocation is implemented by:

- [`pkt_buf_alloc_batch()`](../main.c#L408)

This does the same thing as `pkt_buf_alloc()`, but for up to `num_bufs`
buffers in one call.

Steps:

1. reject invalid arguments
2. compute `avail = min(free_count, num_bufs)`
3. repeat `avail` times:
   - consume `free_ring[free_head]`
   - advance `free_head`
   - decrement `free_count`
   - convert index to pointer
   - reset `buf->size = 0`
4. return the number actually allocated

This is especially useful in reflect mode because the RX loop often wants many
replacement buffers at once.

Instead of:

- 64 repeated allocator calls

it can do:

- one bulk call for up to 64 buffers

That reduces control overhead and keeps the hot path linear.

## 12. Free Path

Recycling is implemented by:

- [`pkt_buf_free()`](../main.c#L434)

The logic is:

1. reject `NULL` or missing-pool inputs
2. if `free_count >= num_entries`, stop
3. set `buf->size = 0`
4. write `buf->mempool_idx` into `free_ring[free_tail]`
5. advance `free_tail` with wraparound
6. increment `free_count`

This is also O(1).

The important point is that freeing uses the buffer's stored identity:

- `buf->mempool`
- `buf->mempool_idx`

So TX completion never needs to search for where a buffer belongs. The buffer
tells us directly.

## 13. Wraparound Behavior

This allocator is circular.

That means:

- when `free_head == num_entries`, it wraps back to `0`
- when `free_tail == num_entries`, it wraps back to `0`

Example with a tiny pool of 8 entries:

Initial state:

```text
free_ring  = [0 1 2 3 4 5 6 7]
free_head  = 0
free_tail  = 0
free_count = 8
```

Allocate 3 buffers:

```text
returned   = [0 1 2]
free_head  = 3
free_tail  = 0
free_count = 5
```

Free buffer `1`, then buffer `2`:

```text
free_ring[0] = 1
free_ring[1] = 2
free_head    = 3
free_tail    = 2
free_count   = 7
```

Keep allocating and freeing long enough and both pointers eventually wrap to the
front of the array again.

That is normal. The queue does not need to be physically compacted.

## 14. Invariants

The allocator relies on several invariants.

### 14.1 Stable Entry Identity

For every valid pool entry:

- `buf->mempool` always points back to the owning pool
- `buf->mempool_idx` never changes

That stable identity is what makes O(1) recycle possible.

### 14.2 `free_count` Bounds

At all times:

```text
0 <= free_count <= num_entries
```

This is enforced by:

- allocation only when `free_count != 0`
- free only when `free_count < num_entries`

### 14.3 No Duplicate Free Ownership

A given `pkt_buf` must not be returned to the pool while still owned by:

- RX
- TX
- some other live software path

That is why reflect mode only calls `pkt_buf_free()` on detached buffers that:

- failed TX enqueue
- or have completed TX and are cleaned by `tx_update_free()`

### 14.4 RX and TX Metadata Must Match Ownership

If RX descriptor slot `i` owns a buffer, then:

- `rx_pkt_bufs[i]` must point to that buffer

If TX descriptor slot `j` owns a pooled buffer, then:

- `tx_pkt_buf_refs[j]` must point to that buffer

The allocator assumes those higher-level ownership structures are correct.

## 15. How the Allocator Fits Into the Reflect Path

The allocator is not used in isolation. It participates in the RX-to-TX
ownership transfer.

### 15.1 Initial RX Arming

[`setup_and_enable_rxq_pool()`](../main.c#L1232) allocates one pooled buffer for
every RX descriptor.

So after bring-up:

- many pool entries are no longer free
- they are owned by RX

### 15.2 On RX Completion

In [`run_rx_reflect()`](../main.c#L1681), software harvests completed RX
descriptors and allocates replacement buffers using:

- [`pkt_buf_alloc_batch()`](../main.c#L408)

Those replacement buffers are installed into RX before the completed buffers are
handed off to TX.

### 15.3 On TX Completion

Later, [`tx_update_free()`](../main.c#L1267) sees completed TX descriptors and
returns their pooled buffers through:

- [`pkt_buf_free()`](../main.c#L434)

That puts those buffers back into the free ring so future RX replacements can
reuse them.

## 16. Ownership Timeline of One Buffer

Take one specific pool entry, say `mempool_idx = 123`.

Its lifecycle may look like:

1. `123` is present in `free_ring`.
2. `pkt_buf_alloc()` hands it out.
3. RX descriptor slot `17` is programmed with `buf_addr_iova`.
4. Hardware receives a packet into that buffer.
5. Software detaches the buffer from RX and installs a replacement.
6. Software rewrites MACs in place.
7. TX descriptor slot `42` is programmed with the same buffer.
8. TX hardware transmits from it.
9. `tx_update_free()` reaches TX slot `42`.
10. `pkt_buf_free()` appends `123` at `free_tail`.
11. The buffer is available for reuse again.

This is the core ownership-transfer loop.

## 17. Why a Circular Queue Instead of a Stack

The previous design used a LIFO free-stack.

The current design uses a circular queue of free indices.

### 17.1 What a Stack Would Mean

A free-stack behaves like:

- allocate from the top
- free back to the top

That works, but it emphasizes most-recently-freed reuse.

### 17.2 What the Circular Queue Means

The circular queue behaves like:

- allocate from the head
- recycle to the tail

This gives a cleaner FIFO-style reuse pattern and makes the state model easy to
describe:

- head is where fresh allocations come from
- tail is where returned buffers go

### 17.3 Why This Design Is Nice Here

Benefits:

- still O(1)
- no data movement
- easy wraparound semantics
- easy to explain in terms of ownership transfer
- works naturally with burst allocation and burst recycle

This does not by itself create the large performance gain. The big throughput
gain came from batching the RX path. But the circular queue is still a clean and
appropriate allocator structure for this design.

## 18. Why This Is Fast

The allocator is fast because it avoids the expensive things a general-purpose
allocator would do:

- no heap interaction per packet
- no metadata search
- no fragmentation handling
- no variable-size allocations
- no pointer chasing through linked lists

Each allocation/free is just:

- one ring-array access
- one pointer/index update
- one counter update

The hot path is predictable and cache-friendly.

## 19. Why This Is Safe for Zero-Copy

Zero-copy is only safe if software never reuses a buffer before hardware is done
with it.

This allocator supports that because buffers are returned to the pool only at
well-defined ownership boundaries:

- RX replacement failure path
- TX completion cleanup path

In other words:

- the allocator never guesses when a buffer is reusable
- it trusts the surrounding ownership logic to return it only when legal

That division of responsibility is important:

- allocator: track free entries cheaply
- datapath logic: decide when ownership really ends

## 20. Concurrency Assumptions

Today, this allocator is intentionally simple and unsynchronized.

There are:

- no locks
- no atomics around `free_head`, `free_tail`, `free_count`
- no per-core sharding

That means the current design assumes allocator operations are externally
serialized by the calling path.

In the current single-queue reflect path, that is fine because the same
execution context performs:

- RX replacement allocation
- TX completion-driven recycle

If the allocator were later shared concurrently across multiple workers, this
exact implementation would need additional synchronization or sharding.

## 21. Failure Modes and Guardrails

The current code includes a few simple guardrails.

### 21.1 Empty Pool

Allocation returns `NULL` or fewer than requested buffers if:

- `free_count == 0`

### 21.2 Over-Free Protection

`pkt_buf_free()` refuses to grow the pool beyond:

- `num_entries`

This is a minimal guard against obvious corruption.

### 21.3 Stable Back-Pointer Requirement

If a caller corrupts:

- `buf->mempool`
- or `buf->mempool_idx`

then recycle correctness is lost.

So those fields are part of the allocator's critical integrity state.

## 22. Pseudocode Summary

### 22.1 Initialization

```text
for i in 0 .. num_entries-1:
  buf = entry(i)
  buf.mempool = pool
  buf.mempool_idx = i
  buf.size = 0
  free_ring[i] = i

free_head = 0
free_tail = 0
free_count = num_entries
```

### 22.2 Allocate One

```text
if free_count == 0:
  return NULL

idx = free_ring[free_head]
free_head = (free_head + 1) mod num_entries
free_count--

buf = entry(idx)
buf.size = 0
return buf
```

### 22.3 Free One

```text
if free_count == num_entries:
  return

buf.size = 0
free_ring[free_tail] = buf.mempool_idx
free_tail = (free_tail + 1) mod num_entries
free_count++
```

## 23. Practical Takeaways

If you want the short version:

- the allocator is a software-only circular queue of free packet-buffer indices
- each packet buffer has a stable identity through `mempool` and `mempool_idx`
- allocation comes from `free_head`
- recycle appends at `free_tail`
- `free_count` removes empty/full ambiguity
- the allocator itself is O(1) and very cheap
- it is a key support structure for zero-copy reflect mode
- it does not move packet payloads, only ownership

## 24. Relationship to the Performance Improvement

It is worth being explicit here.

This allocator architecture is good and appropriate, but it was not the main
reason throughput jumped from roughly `7.8 Mpps` to `37 Mpps`.

The main reason for that jump was:

- batching RX polling
- batching replacement handling
- collapsing many `QRX_TAIL` MMIO writes into one per burst

The allocator helps by making bulk replacement cheap and clean, but the dominant
performance gain came from the reflect-loop batching design around it.

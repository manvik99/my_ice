# RX Reflect Batching Deep Dive

This document explains the current `--rx-reflect` datapath in detail, with special
focus on:

- the packet and ownership data structures
- how the old per-packet reflect loop behaved
- how the new batched reflect loop behaves
- why batching produced a large throughput increase
- what the current numbers mean

The goal is to make the reflect path understandable without having to mentally
reconstruct it from `main.c`.

## 1. Executive Summary

The reflect path is a zero-copy RX-to-TX forwarding loop for one RX queue and one
TX queue. A packet arrives into a DMA buffer owned by the RX ring, software
swaps in a replacement buffer for that RX descriptor, rewrites only the Ethernet
MAC addresses in the completed buffer, and then submits that same completed
buffer directly to TX.

The important recent change is that the reflect loop used to do the RX repost
work one packet at a time, but now does it in bursts.

That means the most important hot-path change was:

- before: roughly one `QRX_TAIL` write per packet
- now: one `QRX_TAIL` write per burst

For a burst size of `64`, the RX-side MMIO pattern changed from approximately:

- `64` RX tail writes + `1` TX doorbell

to:

- `1` RX tail write + `1` TX doorbell

That is why throughput moved from roughly:

- `~7.8 Mpps`
- `~5.25 wire-Gbps` for `60B` frames

to roughly:

- `~37 Mpps`
- `~24.9 wire-Gbps` for the same `60B` frames

The big win came from batching the RX side. TX was already more batched than RX.

## 2. Where the Code Lives

The main pieces of the reflect datapath are:

- pool and packet object definitions in [`main.c`](../main.c#L57)
- reflect mempool initialization in [`main.c`](../main.c#L364)
- pool-backed RX setup in [`main.c`](../main.c#L1232)
- TX completion and recycle in [`main.c`](../main.c#L1267)
- burst RX polling in [`main.c`](../main.c#L1295)
- burst pooled-buffer TX enqueue in [`main.c`](../main.c#L1417)
- burst RX repost in [`main.c`](../main.c#L1443)
- MAC rewrite in [`main.c`](../main.c#L1596)
- main reflect loop in [`main.c`](../main.c#L1681)

For a broader end-to-end workflow, see:

- [`DRIVER_WORKFLOW.md`](DRIVER_WORKFLOW.md)

## 3. What Reflect Mode Is Actually Doing

Reflect mode is not a generic network stack.

It does something much simpler:

1. Receive an Ethernet frame into a DMA buffer.
2. Keep the payload in place.
3. Swap source and destination MAC addresses so the frame goes back toward the sender.
4. Submit the same DMA buffer to TX.
5. Recycle that DMA buffer only after the TX descriptor has completed.

The important property is:

- no payload copy between RX and TX

The buffer changes owners over time, but the packet bytes themselves stay in the
same DMA buffer.

## 4. High-Level Architecture

There are three broad pieces involved:

### 4.1 Control Plane

The control plane is everything needed to make the datapath legal and runnable:

- VFIO setup
- BAR0 mapping
- DMA mapping
- Admin Queue setup
- VSI/lport discovery
- RX MAC rule installation
- TX queue creation

That control work is not the reason for the throughput difference. It happens
once during bring-up.

### 4.2 Datapath Memory

The datapath uses DMA-visible memory for:

- RX descriptor ring
- TX descriptor ring
- queue-owned TX staging buffers used by `--tx-send` and `--tx-bench`
- the reflect mempool used by `--rx-reflect`

It also uses software-only metadata arrays to remember which software object is
associated with each hardware descriptor slot.

### 4.3 The Reflect Hot Loop

Once everything is set up, the performance-critical part is just:

- notice completed RX descriptors
- detach completed buffers from RX
- attach replacement buffers to RX
- edit the detached buffers in place
- submit those detached buffers to TX
- later recycle them from TX completion

That hot loop is where the batching change happened.

## 5. Core Data Structures

This section is the heart of the design.

### 5.1 `struct pkt_buf`

Defined in [`main.c`](../main.c#L57).

This is the software object that represents one DMA-capable packet buffer.

Important fields:

- `buf_addr_iova`
  - IOVA of the `data` region
  - this is the address programmed into RX and TX descriptors
- `mempool`
  - back-pointer to the owning pool
  - used when TX completion returns the buffer
- `mempool_idx`
  - stable index of this entry inside the pool
  - makes recycle O(1)
- `size`
  - logical packet length currently stored in `data`
- `data[]`
  - the actual packet bytes

Conceptually, `pkt_buf` is the unit of zero-copy ownership transfer.

RX owns a `pkt_buf`, then software owns it briefly, then TX owns it, then the
pool owns it again.

### 5.2 `struct pkt_mempool`

Defined in [`main.c`](../main.c#L66).

This is the free-buffer allocator used by reflect mode.

Important fields:

- `base`
  - start of the DMA-backed pool region in virtual memory
- `base_iova`
  - IOVA of that same region
- `entry_size`
  - byte stride from one pool entry to the next
- `num_entries`
  - number of packet buffers in the pool
- `free_head`
  - next free index to hand out
- `free_tail`
  - next slot where a returned index is appended
- `free_count`
  - number of free entries currently available
- `free_ring`
  - circular queue of free `mempool_idx` values

This is a circular free queue, not a LIFO stack.

Allocation:

- read `free_ring[free_head]`
- advance `free_head`
- decrement `free_count`

Free:

- write `buf->mempool_idx` to `free_ring[free_tail]`
- advance `free_tail`
- increment `free_count`

This keeps allocation and recycle O(1) and makes the ownership flow explicit.

### 5.3 `struct io_ring_ctx`

Defined in [`main.c`](../main.c#L91).

This holds shared RX-side state.

Important fields:

- `mac`
  - local NIC MAC address
- `lport`, `vsi_num`, `rxq_id`, `qparent_teid`
  - control-plane identities
- `rx_desc`
  - RX descriptor ring
- `rx_desc_iova`
  - IOVA of that ring
- `rx_bufs`, `rx_bufs_iova`
  - flat RX buffer region used by listen mode
- `rx_pkt_bufs`
  - software-only array mapping RX descriptor slot -> `pkt_buf *`
- `rx_ntc`
  - software next-to-check index for RX completions

The key reflect field is `rx_pkt_bufs[idx]`. It tells software which pooled
buffer is currently attached to RX descriptor slot `idx`.

### 5.4 `struct txq_ctx`

Defined in [`main.c`](../main.c#L110).

This is one TX queue's software and hardware context.

Important fields:

- `txq_id`
  - hardware TX queue number
- `desc_count`
  - TX ring depth
- `tx_desc`, `tx_desc_iova`
  - TX descriptor ring and its IOVA
- `tx_pkt_bufs`, `tx_pkt_iova`
  - queue-owned flat TX staging memory used by non-reflect modes
- `tx_next_to_use`
  - next descriptor slot software will fill
- `tx_next_to_clean`
  - first descriptor slot that may still be owned by hardware
- `tx_free`
  - currently available TX descriptor slots
- `tx_pkts_since_rs`
  - tracks when to request RS status
- `tx_pkt_buf_refs`
  - software-only array mapping TX descriptor slot -> pooled `pkt_buf *`

That last field is what makes zero-copy reflection safe. TX completion uses it
to know exactly which pooled buffer should be returned to the mempool.

### 5.5 `struct dev_ctx`

Defined in [`main.c`](../main.c#L124).

This is the top-level object that ties everything together:

- VFIO fds and BAR mapping
- queue configuration
- DMA block
- AdminQ rings
- RX/TX datapath state
- reflect mempool

Reflect mode operates almost entirely through:

- `d->io`
- `d->txqs[0]`
- `d->reflect_pool`

## 6. DMA and Software Metadata Layout

The physical memory for datapath objects is laid out in [`layout_dma()`](../main.c#L632).

There are two important categories of state:

### 6.1 DMA-Visible State

The NIC can directly DMA to or from:

- RX descriptors
- TX descriptors
- packet buffer memory

### 6.2 Software-Only Metadata

The NIC cannot see:

- `rx_pkt_bufs`
- `tx_pkt_buf_refs`
- `free_ring`

These arrays exist only so software can answer ownership questions quickly:

- which pooled buffer does RX descriptor `i` currently own?
- which pooled buffer does TX descriptor `j` currently owe back to the pool?
- which pooled buffers are free right now?

That separation is important:

- hardware sees only physical addresses in descriptors
- software sees the ownership graph

## 7. Buffer Ownership Model

The ownership model is the conceptual center of the design.

For one reflected frame, the lifecycle is:

1. Free in the mempool.
2. Attached to an RX descriptor.
3. Filled by hardware on packet arrival.
4. Detached from RX when software installs a replacement buffer.
5. Temporarily owned by software while MACs are rewritten.
6. Attached to a TX descriptor.
7. Returned to the mempool when TX completion advances past that descriptor.

In one line:

- free pool -> RX ring -> software -> TX ring -> free pool

The payload does not move across these states.

## 8. Pool Bring-Up and RX Ring Arming

Reflect mode first initializes the mempool in [`pkt_pool_init()`](../main.c#L364).

That function:

- zeros the pool region
- computes each entry's `buf_addr_iova`
- stores `mempool` and `mempool_idx`
- marks every entry free by pushing all indices into `free_ring`

Then [`setup_and_enable_rxq_pool()`](../main.c#L1232) arms the RX ring with
pooled buffers:

- clear RX descriptors
- clear `rx_pkt_bufs`
- allocate one `pkt_buf` per RX descriptor
- store that pointer in `rx_pkt_bufs[idx]`
- program the RX descriptor with `buf->buf_addr_iova`
- enable the RX queue

At that moment:

- those `pkt_buf`s are no longer free
- they are owned by the RX ring on behalf of the NIC

## 9. TX Completion and Recycle

The TX side recycle path is in [`tx_update_free()`](../main.c#L1267).

That function:

1. reads hardware TX head via `QTX_COMM_HEAD`
2. walks from `tx_next_to_clean` up to the hardware head
3. checks `tx_pkt_buf_refs[idx]` for each completed descriptor
4. if a pooled `pkt_buf *` is present, returns it with [`pkt_buf_free()`](../main.c#L434)
5. recomputes `tx_free`

This is the rule that preserves correctness:

- a buffer is not free when TX is given the descriptor
- it becomes free only after hardware has completed that TX slot

That is why zero-copy reflection is safe even though RX and TX are sharing the
same buffer objects over time.

## 10. What the Old Reflect Loop Did

This section describes the old behavior conceptually. The exact old code is gone
now, but the logic was the single-packet version of today's design.

### 10.1 Old Per-Packet Algorithm

For each packet, software effectively did:

```text
loop:
  if tx ring looks full:
    tx_update_free()

  got = poll_one_rx_desc()
  if no packet:
    sleep briefly
    continue

  rx_buf = rx_pkt_bufs[rx_idx]
  replacement = pkt_buf_alloc()

  rx_buf->size = rx_len
  rx_pkt_bufs[rx_idx] = replacement
  rearm_rx_desc(rx_idx)              # writes QRX_TAIL here

  rewrite_reflect_l2(rx_buf)

  enq = tx_try_enqueue_pkt_buf(rx_buf)
  if enough packets accumulated:
    tx_ring_doorbell()
```

The important part is this line:

- `rearm_rx_desc(rx_idx)` happened once per packet

That means for a burst of 64 packets, RX reposting behaved like:

- allocate 64 replacements
- write 64 descriptors
- write `QRX_TAIL` 64 times

### 10.2 Why This Was Expensive

The old path had several sources of overhead:

- one RX completion scan per packet
- one replacement-buffer allocation per packet
- one RX descriptor repost per packet
- one RX tail MMIO write per packet
- more branch and function-call overhead

Of those, the most damaging was:

- one `QRX_TAIL` register write per packet

MMIO writes are expensive compared with regular cacheable memory operations.

So even though the design was zero-copy, it still had heavy control overhead.

### 10.3 What the Old Path Was Already Doing Well

Not everything was bad.

The old path already had:

- zero-copy buffer ownership transfer
- TX completion-based recycle
- burst TX doorbells in the outer loop

This is why the performance gain did not come from inventing zero-copy. That was
already there. The gain came from making the RX side match the burst style more
closely.

## 11. What the New Reflect Loop Does

The current reflect loop is in [`run_rx_reflect()`](../main.c#L1681).

It still reflects one RX queue into one TX queue, but now it processes work in
bursts sized by:

- `TX_BURST_SIZE`

which is currently defined as `64` in [`main.c`](../main.c#L43).

### 11.1 Step 1: Choose a Burst Budget

At the top of each iteration, the loop computes how much work it can safely do.

The budget is bounded by:

- `reflect_batch`
- available TX descriptors (`q->tx_free`)
- available free pool buffers (`d->reflect_pool.free_count`)

That means the loop does not pull more RX packets than it has resources to
replace and transmit.

### 11.2 Step 2: Poll RX in a Burst

[`poll_rx_batch()`](../main.c#L1295) starts at `d->io.rx_ntc` and walks forward
until one of these happens:

- it reaches `max_count`
- it hits a descriptor whose `DD` bit is not set
- it hits an invalid descriptor

For each completed packet, it returns:

- the RX descriptor index
- the packet length

This keeps the loop working on a contiguous ready region rather than repeatedly
searching for a single descriptor.

### 11.3 Step 3: Count How Many Replacements Are Needed

Not every harvested RX descriptor necessarily becomes a reflected packet.

The loop counts only packets with `rx_len >= 14` as valid reflect candidates.

Short packets are:

- counted as `rx_short`
- reposted back to RX
- not sent to TX

This matters because software should only allocate replacement buffers for the
packets that will actually be detached from RX and passed onward.

### 11.4 Step 4: Allocate Replacement Buffers in Bulk

The loop calls [`pkt_buf_alloc_batch()`](../main.c#L408) once for the whole
burst.

Instead of:

- 64 calls to `pkt_buf_alloc()`

it can now do:

- 1 call to `pkt_buf_alloc_batch()` that hands back up to 64 `pkt_buf *`

This is not as important as collapsing MMIO writes, but it still reduces control
overhead and keeps the hot loop more linear.

### 11.5 Step 5: Build the Software View of the Burst

For each completed RX packet:

1. load `rx_buf = d->io.rx_pkt_bufs[rx_idx]`
2. remember that `rx_idx` must be rearmed
3. if the packet is valid:
   - set `rx_buf->size = rx_len`
   - install a replacement buffer into `d->io.rx_pkt_bufs[rx_idx]`
   - rewrite MACs in the completed buffer
   - append the completed buffer to `tx_bufs[]`
   - append the length to `tx_lens[]`

The important detail is that software is preparing two different arrays:

- `rearm_idxs[]`
  - which RX descriptor slots must be reposted
- `tx_bufs[]`
  - which completed packet buffers will be handed to TX

That split makes the ownership transition explicit.

### 11.6 Step 6: Rearm RX in One Burst

[`rearm_rx_desc_batch()`](../main.c#L1443) does the repost work for all
processed descriptors:

- reset descriptor addresses
- zero descriptor read fields
- write `QRX_TAIL` once using the last replenished index
- advance `d->io.rx_ntc`

This is the critical performance improvement.

For a 64-packet burst:

- old path: 64 `QRX_TAIL` writes
- new path: 1 `QRX_TAIL` write

That alone is a dramatic reduction in MMIO traffic.

### 11.7 Step 7: Enqueue TX in a Burst

Once RX has been safely replenished, the detached packet buffers are submitted to
TX using [`tx_try_enqueue_pkt_buf_batch()`](../main.c#L1417).

That helper:

- ensures `q->tx_free` is reasonably up to date
- walks the TX ring from `tx_next_to_use`
- programs each TX descriptor with `buf->buf_addr_iova`
- stores the same `pkt_buf *` into `tx_pkt_buf_refs[idx]`
- updates queue state through `tx_commit_slot()`

This step is the actual zero-copy handoff from software ownership to TX-ring
ownership.

### 11.8 Step 8: Ring TX Once

If any descriptors were enqueued, [`tx_ring_doorbell()`](../main.c#L1494) is
called once for the whole burst.

This is why your `doorbells` counter is useful:

- `tx_pkts / doorbells` is effectively the average TX burst size

In your successful run:

- `1856777038 / 29012171 ~= 64`

That is strong evidence that batching is working as intended.

### 11.9 Step 9: Recycle Later from TX Completion

Nothing about recycle changed conceptually:

- TX completion still returns buffers through `tx_update_free()`
- `pkt_buf_free()` still appends them to the mempool free queue

The batching change did not change ownership correctness. It changed how much
overhead software pays while moving between those ownership states.

## 12. Before vs After

The easiest way to see the change is as a table.

| Aspect | Old path | New path | Why it matters |
| --- | --- | --- | --- |
| RX completion harvest | one packet at a time | up to 64 packets at a time | fewer loop iterations and branches |
| replacement allocation | `pkt_buf_alloc()` per packet | `pkt_buf_alloc_batch()` per burst | lower allocator overhead |
| RX repost | `rearm_rx_desc()` per packet | `rearm_rx_desc_batch()` per burst | much fewer MMIO writes |
| `QRX_TAIL` writes | about 64 per 64 packets | 1 per 64 packets | biggest improvement |
| TX pooled enqueue | logically per packet | explicit burst helper | better linearity and queue usage |
| TX doorbell | burst-oriented already | still burst-oriented | TX was not the main bottleneck |
| ownership model | zero-copy | zero-copy | same correctness model |
| queue count | 1 RX / 1 TX | 1 RX / 1 TX | batching improved single-core efficiency, not parallelism |

## 13. Why the New Path Is Faster

This section is the practical answer to "why did throughput jump so much?"

### 13.1 Reason 1: RX MMIO Writes Collapsed

This is the most important reason.

Before, every packet led to:

- a descriptor repost
- a `QRX_TAIL` register write

Now, one burst leads to:

- many descriptor reposts in memory
- one `QRX_TAIL` register write

Memory writes to descriptors are cheap compared with MMIO writes to device
registers. The new path preserves the cheap work and removes most of the
expensive work.

### 13.2 Reason 2: Fewer Hot-Loop Trips

The old loop repeatedly paid the fixed cost of:

- poll a descriptor
- branch on its state
- handle one packet
- re-enter the loop

The new loop amortizes those fixed costs across an entire burst.

### 13.3 Reason 3: Better Cache and Branch Locality

Burst processing tends to be kinder to the CPU because it:

- walks descriptor arrays linearly
- touches adjacent software metadata slots
- repeats similar work in a tight linear loop

That usually means:

- better data-cache behavior
- fewer unpredictable branches
- less overhead from function boundaries

### 13.4 Reason 4: The Old Bottleneck Was RX Control Overhead, Not Ring Depth

This is important because it explains why increasing descriptor count did not
help much.

Increasing ring depth changes:

- how much burst slack the queue has

It does not change:

- how much software work happens per packet
- how many MMIO writes happen per packet

So ring depth was not the big limiter.

### 13.5 Reason 5: The Circular Free Queue Was Helpful, But Not the Main Win

The circular free queue is a good ownership structure because it is:

- simple
- O(1)
- explicit about allocation from head and recycle to tail

But by itself it does not explain a jump from roughly `7.8 Mpps` to `37 Mpps`.

That jump is mostly explained by:

- batching RX harvest
- batching RX repost
- collapsing `QRX_TAIL` writes

So the allocator change is good engineering, but the batching change is the main
performance story.

## 14. Throughput Formulas and Why the Numbers Look Different

The reporting helpers are:

- [`bytes_ns_to_gbps()`](../main.c#L278)
- [`pkts_ns_to_mpps()`](../main.c#L286)
- [`l2_bytes_to_wire_bytes()`](../main.c#L294)

### 14.1 L2 Gbps

This is based only on bytes in the actual Ethernet frame counted by software:

```text
l2_gbps = (bytes * 8) / seconds / 1e9
```

### 14.2 Wire Gbps

This includes Ethernet wire overhead:

- `4` bytes FCS
- `8` bytes preamble + SFD
- `12` bytes inter-frame gap

That is `24` bytes total per packet.

So:

```text
wire_bytes = l2_bytes + packets * 24
wire_gbps  = (wire_bytes * 8) / seconds / 1e9
```

For `60B` frames:

- L2 size = `60`
- wire size = `84`

So:

```text
wire_gbps = l2_gbps * 84 / 60
```

### 14.3 Example from Your Run

At around:

- `37.05 Mpps`

for `60B` packets:

```text
37.05e6 * 60 * 8  ~= 17.8 Gbps L2
37.05e6 * 84 * 8  ~= 24.9 Gbps wire
```

That is why the on-screen `wire-Gbps` and `*_l2_gbps` numbers are different.

## 15. Why the Big Jump Is Plausible

Your old steady-state result was around:

- `~7.8 Mpps`
- `~5.25 wire-Gbps`

Your new steady-state result is around:

- `~37 Mpps`
- `~24.9 wire-Gbps`

That is about a `4.7x` increase in packet rate.

That makes sense because:

- the old path was still paying a per-packet RX repost/MMIO tax
- the new path amortizes that tax across an entire burst
- the current counters show full TX bursts

The number that particularly supports the new design is:

- `tx_pkts / doorbells ~= 64`

That means the TX side is now operating at the configured burst size almost all
the time.

## 16. What Did Not Change

The following things are still true:

- reflect mode still uses one RX queue and one TX queue
- it is still single-core in the current path
- it still performs one MAC rewrite per valid reflected packet
- it still uses TX completion to decide when a pooled buffer may be freed
- it still does not copy payload bytes between RX and TX

So the new throughput came from better efficiency on the same single-queue model,
not from parallelism.

## 17. What the Current Bottleneck Probably Is

Now that batching improved single-queue efficiency, the next likely bottleneck is
no longer per-packet RX repost overhead.

The current likely bottleneck is:

- one RX queue
- one TX queue
- one worker/core

If offered load is higher than what one queue and one core can drain, the excess
traffic will be dropped before software reflects it.

That is why the next major scaling step is probably:

- multiple RX queues
- hardware steering or RSS
- one worker per queue or queue group

## 18. Why This Design Is Better Than the Previous Approach

This deserves a direct answer.

### 18.1 It Preserves the Good Properties

The new design keeps the good parts of the old one:

- zero-copy RX-to-TX forwarding
- explicit ownership transfer
- TX completion-based recycle
- simple data structures

### 18.2 It Removes an Artificial Bottleneck

The old design had an avoidable software bottleneck:

- reposting RX one packet at a time

That does not improve correctness. It only increases overhead.

The new design removes that avoidable cost without changing the fundamental
ownership model.

### 18.3 It Matches How High-Speed Polling Drivers Usually Work

Efficient userspace NIC drivers tend to:

- harvest bursts
- fill bursts
- doorbell bursts
- recycle in bursts

The new reflect path is much closer to that shape than the old one.

### 18.4 It Scales Better to Higher Rates

A per-packet control loop quickly becomes dominated by:

- MMIO writes
- loop overhead
- branch overhead

A burst-oriented loop has a much better chance of staying useful at higher packet
rates because fixed costs are amortized across many packets.

### 18.5 It Is Still Easy to Reason About

The new design is not a complicated scheduler. It is still easy to explain:

1. collect a burst
2. detach valid packets from RX
3. rearm RX once
4. submit TX once
5. recycle from TX completion later

That makes it a good stepping stone toward future multi-queue work.

## 19. Pseudocode Summary

### 19.1 Previous Design

```text
while running:
  maybe free TX completions

  got one packet from RX
  if none:
    sleep a little
    continue

  allocate one replacement buffer
  swap replacement into RX slot
  write RX tail now

  rewrite MACs in completed buffer
  enqueue completed buffer to TX

  after some packets, ring TX
```

### 19.2 Current Design

```text
while running:
  maybe free TX completions

  budget = min(burst_size, tx_free, pool_free)
  got up to budget packets from RX
  if none:
    sleep a little
    continue

  count valid reflectable packets
  allocate that many replacements in one batch

  for each harvested descriptor:
    if short packet:
      mark RX slot for repost only
    else:
      detach completed buffer
      install replacement
      rewrite MACs in completed buffer
      append completed buffer to TX burst

  repost all RX slots with one RX tail write
  enqueue TX burst
  ring TX once
```

## 20. Practical Takeaways

If you want the short practical version:

- the old reflect path was already zero-copy, but not fully burst-oriented
- the main inefficiency was RX reposting one packet at a time
- the new path batches RX polling, replacement allocation, RX repost, and TX submit
- the biggest concrete improvement is collapsing many `QRX_TAIL` MMIO writes into one
- that is why the single-queue throughput jumped so much

## 21. Future Work

The next logical improvements are:

- add counters for average RX burst size and RX tail writes per second
- add hardware RX-drop visibility
- scale the reflect path to multiple RX/TX queues
- program hardware steering or RSS so traffic actually lands on multiple RX queues

The current document explains how the single-queue zero-copy reflector works
today. Multi-queue scaling will build on these same ownership rules.

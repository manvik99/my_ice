# RX/TX Reflector Design

## Goal

Build a new runtime mode for `my_ice` that:

- receives Ethernet frames on `node_0_new`
- rewrites only the L2 MAC addresses
- sends the frame back to the node it came from

In short, this is an L2 reflector:

1. packet arrives on Rx
2. userspace inspects the frame
3. source/destination MACs are rewritten
4. frame is transmitted back out on Tx

This document proposes a clear implementation strategy based on the current codebase.

## Why This Mode Makes Sense

The repository already has the pieces needed for a first version:

- Rx queue setup and packet polling in `run_rx_listen()`
- Tx queue setup and single-packet enqueue logic in `run_tx_send()`
- sustained Tx queue operation in `run_tx_bench()`

The missing piece is a combined Rx->Tx fast path.

## Scope

This design is intentionally narrow:

- single port
- single Rx queue
- single Tx queue initially
- polling only
- one packet buffer per descriptor
- no routing, no L3 rewrite, no checksum work, no multi-segment packets

That is enough to simulate a realistic service loop while staying aligned with the current driver.

## Two-Node Topology

Recommended lab setup:

- `node_1_new`: traffic generator and validator
- `node_0_new`: `my_ice` device under test running the reflector

Concrete example for your setup:

- `node_1_new` sends with source MAC `40:a6:b7:c3:41:20`
- `node_1_new` sends to `<node-0-my_ice-mac>`
- `node_0_new` reflects the packet back to destination MAC `40:a6:b7:c3:41:20`

Traffic flow:

1. `node_1_new` sends frames to `node_0_new`'s NIC MAC
2. `node_0_new` receives the frames into Rx DMA buffers
3. `node_0_new` rewrites MAC addresses and transmits them back
4. `node_1_new` receives the reflected frames and verifies them

This setup is realistic enough to exercise:

- Rx DMA
- descriptor polling
- CPU touch of packet headers and payload
- Tx descriptor production
- Tx DMA
- doorbell behavior

## Core Design Decision

### Recommended phase 1: copy-based reflection

The first implementation should copy packet bytes from the Rx buffer into a Tx buffer, then transmit from the Tx buffer.

This is the recommended first step because it matches the current code structure and keeps buffer ownership simple.

Flow per packet:

1. poll one completed Rx descriptor
2. read packet bytes from the Rx DMA buffer
3. copy the packet into a Tx DMA buffer
4. rewrite:
   - `tx.dst_mac = rx.src_mac`
   - `tx.src_mac = local_nic_mac`
5. build a Tx descriptor for that Tx buffer
6. rearm the Rx descriptor
7. ring the Tx doorbell after a small batch

### Why not pointer handoff first

The tempting idea is:

- do not copy packet data
- point the Tx descriptor directly at the Rx DMA buffer
- ring Tx later in batch

That can be made to work in principle, but it is not the right phase-1 design here.

Reasons:

1. Rx buffer ownership becomes tricky.
   The current Rx path assumes an Rx buffer is immediately returned to the Rx ring after processing. If Tx is still using that same DMA buffer, Rx cannot safely rearm it yet.

2. The current Tx path assumes queue-owned Tx buffers.
   `tx_try_enqueue()` currently computes the DMA address from the Tx ring's own packet buffer array. A zero-copy handoff would require a new enqueue path that accepts arbitrary DMA addresses.

3. Completion tracking becomes more important.
   With pointer handoff, the Rx buffer cannot be reused until Tx completion has been observed. That means adding a buffer-lifecycle system, likely per-descriptor metadata and a deferred recycle path.

4. Backpressure gets harder.
   If Tx stalls, Rx buffers stop being returned quickly. With only `ICE_RX_DESC_COUNT = 128`, the Rx side can run out of posted buffers quickly under load.

5. The copy is not wasted for this workload.
   You explicitly want to touch packet data and bring it into cache while rewriting MAC addresses. A copy-first design naturally exercises the CPU/memory path you care about.

So the design recommendation is:

- phase 1: copy packet bytes from Rx DMA buffer to Tx DMA buffer
- phase 2: optionally add a zero-copy or buffer-loan mode later

## Buffer Ownership Model

### Rx side

Rx buffers remain owned by the Rx ring.

Rules:

- hardware DMA writes incoming packets into Rx DMA buffers
- userspace reads packet bytes from those buffers
- once the frame is copied out, the Rx descriptor is immediately rearmed with the same buffer

This preserves the current `run_rx_listen()` model and minimizes change.

### Tx side

Tx buffers remain owned by the Tx ring.

Rules:

- userspace copies a reflected frame into the next free Tx packet buffer
- userspace writes a Tx descriptor pointing to that Tx buffer
- hardware DMA reads from the Tx buffer when the doorbell is rung
- Tx descriptors are reclaimed using the existing head/clean logic

This preserves the current `tx_try_enqueue()` model.

## Fast Path Strategy

### Packet transform

For each received Ethernet frame:

- reject frames shorter than 14 bytes
- preserve payload exactly
- rewrite only:
  - destination MAC = original source MAC
  - source MAC = local NIC MAC

No other field should change in phase 1.

### Processing loop

The new mode should use a tight polling loop that looks like this:

1. poll Rx descriptors for completed packets
2. for each packet found:
   - validate packet length
   - copy bytes into a Tx buffer
   - rewrite MAC addresses in the copied frame
   - enqueue one Tx descriptor
   - rearm the Rx descriptor
3. after `N` packets, ring the Tx doorbell once
4. periodically reclaim Tx descriptors
5. continue until timeout, signal, or packet budget is reached

### Batch size

Use batching for doorbells, but keep it modest initially.

Recommended initial settings:

- Rx polling batch target: up to 32 packets per loop
- Tx doorbell batch: every 8 to 32 successful enqueues
- reclaim Tx free descriptors whenever free space is low

Why:

- one doorbell per packet is simple but noisier than needed
- very large batches can increase latency and make debugging harder
- a small batch gives a good balance for bring-up

## Recommended Implementation Shape

Add a new mode, for example:

- `--rx-reflect`

Possible CLI shape:

```text
sudo ./my_ice <BDF> --rx-reflect [seconds]
```

Possible later options:

- `--reflect-tx-queue <n>`
- `--reflect-burst <n>`
- `--reflect-max-pkts <n>`
- `--reflect-copy-mode copy|loan`

### Suggested internal functions

Add small helpers instead of putting everything into one large function:

- `run_rx_reflect(struct dev_ctx *d, int timeout_ms)`
- `reflect_one_packet(struct dev_ctx *d, struct txq_ctx *q, uint16_t rx_idx)`
- `tx_try_enqueue_addr(...)` only if a future zero-copy mode is added

Phase 1 does not need `tx_try_enqueue_addr()`.

### Recommended reuse from current code

Reuse as much existing code as possible:

- Rx queue discovery and setup from `run_rx_listen()`
- VSI/lport discovery and MAC rule installation from `run_rx_listen()`
- Tx queue discovery and creation from `run_tx_send()` / `run_tx_bench()`
- Tx ring initialization from `tx_ring_init()`
- Tx descriptor reclaim from `tx_update_free()`
- Tx tail write from `tx_ring_doorbell()`

The main new logic is the reflector loop itself.

## Detailed Packet Lifecycle

### Phase 1 copy-based path

For one reflected packet:

1. hardware writes packet into `rx_buf[idx]`
2. software sees `DD` on Rx descriptor
3. software reads `pkt_len`
4. software reserves one Tx descriptor/buffer
5. software copies `pkt_len` bytes from `rx_buf[idx]` to `tx_buf[tx_idx]`
6. software rewrites the first 12 bytes of Ethernet header in `tx_buf[tx_idx]`
7. software fills Tx descriptor with `tx_buf[tx_idx]` DMA address
8. software rearms `rx_buf[idx]` back to Rx ring
9. after a batch, software rings Tx doorbell
10. hardware DMA-reads `tx_buf[tx_idx]` and transmits it
11. software later observes Tx completion and reclaims that Tx slot

This design has a clean invariant:

- Rx buffers are only for receive
- Tx buffers are only for transmit

## Why This Is the Right First Design

This approach is realistic, not just convenient.

It simulates a common packet-processing service:

- ingress DMA into host memory
- CPU touch and small header rewrite
- egress DMA from host memory

That is already a meaningful workload for:

- PCIe traffic
- cache behavior
- memory bandwidth
- descriptor/ring pressure
- MMIO doorbells

It also gives much cleaner bring-up and debugging than a borrowed-buffer design.

## Zero-Copy Variant for Later

If we later want a more aggressive design, we can add a separate mode that lets Tx borrow Rx buffers temporarily.

That design would require:

1. a Tx enqueue helper that accepts an arbitrary DMA address rather than only a queue-owned Tx buffer
2. metadata linking each outstanding Tx descriptor back to an Rx buffer index
3. delayed Rx rearm only after Tx completion for that borrowed buffer
4. explicit accounting so Rx never runs out of posted buffers

This is feasible, but it is a different ownership model and should not be mixed into the first version.

## Backpressure and Failure Behavior

The reflector must define what happens when Rx is receiving faster than Tx can drain.

### Recommended phase-1 rule

If Tx has no free descriptors:

- reclaim Tx completions
- if still full, stop processing more Rx packets for the moment
- do not consume a new Rx packet unless there is Tx capacity available

Practical interpretation:

- only advance an Rx packet into software processing when a Tx slot is available
- if Tx is full, leave completed Rx descriptors unprocessed temporarily

This is better than copying packets that cannot be transmitted.

### Why this works

The NIC has an Rx ring with a finite number of posted buffers. If software falls behind, eventually Rx drops will happen. That is acceptable for the first version and is easier to reason about than inventing a software holding queue right away.

## Stats and Observability

The new mode should expose simple counters, printed periodically and at exit:

- `rx_pkts`
- `rx_bytes`
- `tx_pkts`
- `tx_bytes`
- `tx_ring_full`
- `rx_short_frames`
- `rx_errors`
- `copy_bytes`
- `doorbells`

Also useful:

- elapsed time
- average reflected pps
- average reflected Gbps

These counters will make it much easier to compare:

- small packets vs large packets
- one queue vs later multi-queue modes
- copy mode vs future zero-copy mode

## Bring-Up Plan

### Step 1: correctness

Goal:

- reflect packets correctly at low rate

Method:

- run reflector on `node_0_new`
- send a few frames from `node_1_new` using `send_l2.py`
- verify returned frames have:
  - dst MAC equal to sender MAC
  - src MAC equal to `node_0_new` NIC MAC
  - unchanged payload

Suggested commands:

On `node_0_new`:

```bash
sudo ./my_ice <BDF> --rx-reflect 60
```

On `node_1_new`:

```bash
sudo python3 /users/manvik12/my_ice/send_l2.py \
  -i <node-1-iface> \
  -d <node-0-my_ice-mac> \
  -s 40:a6:b7:c3:41:20 \
  -p hello-reflect \
  -c 20 \
  -t 0.1
```

Optional capture on `node_1_new`:

```bash
sudo tcpdump -enn -i <node-1-iface> ether dst 40:a6:b7:c3:41:20
```

### Step 2: sustained load

Goal:

- confirm ring behavior under continuous traffic

Method:

- use `pktgen_bench.sh` or another generator on `node_1_new`
- test frame sizes such as:
  - 64B
  - 128B
  - 256B
  - 512B
  - 1518B

### Step 3: tuning

Tune only after phase 1 is stable:

- Tx batch size before doorbell
- descriptor count
- CPU pinning
- hugepages
- optional multi-Tx-queue support

## Non-Goals for Phase 1

Do not include these in the first implementation:

- zero-copy borrowed Rx->Tx buffers
- multiple Rx queues
- RSS
- checksum or TSO offloads
- VLAN edits
- L3/L4 rewriting
- software retry queues
- lock-free cross-thread pipelines

## Final Recommendation

The best way forward is:

1. add a new `--rx-reflect` mode
2. implement it as a copy-based Rx->Tx reflector
3. batch Tx doorbells in small groups
4. keep Rx and Tx DMA buffer ownership separate
5. use this as the baseline realistic workload

Only after that is stable should we consider a second design that loans Rx buffers directly to Tx.

That keeps the first version easy to reason about, faithful to the current codebase, and realistic enough to exercise the hardware/software path you care about.

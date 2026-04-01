# How ixy Measures Packet Throughput

This document explains the throughput measurement path in `ixy`, with `src/app/ixy-fwd.c` as the main example.

The short version is:

- `ixy-fwd` does not run a separate benchmark path apart from forwarding.
- The forwarder pushes real traffic through RX and TX, and a side-band stats path periodically snapshots counters.
- Packet throughput is reported as packets counted over elapsed wall-clock time.
- Bit throughput is derived from byte counters plus a fixed 20-byte per-packet wire-overhead adjustment.
- The exact counter source is driver-specific:
  - `ixgbe` uses NIC hardware registers.
  - `virtio` uses software counters maintained in the driver.

## 1. Direct or indirect measurement?

The answer depends on which metric and which driver you mean.

For `ixy-fwd` on `ixgbe`:

- RX packet throughput is measured directly from NIC packet counters.
- TX packet throughput is measured directly from NIC packet counters.
- RX byte throughput is measured directly from NIC byte counters.
- TX byte throughput is measured directly from NIC byte counters.
- Reported Mbit/s is partly derived, because `ixy` adds a fixed 20 bytes per packet to approximate preamble, SFD, and IFG.

For `virtio`:

- RX and TX counters are not read from hardware statistics registers.
- They are maintained in software inside `virtio_rx_batch()` and `virtio_tx_batch()`.
- The same timing and rate calculation code is reused, but the raw counts are indirect because the driver is counting events in software.

So the framework-level answer is:

- Packet throughput is measured by counting packets over time.
- The timing is direct wall-clock timing.
- The packet and byte counts are driver-specific: direct hardware stats on `ixgbe`, software bookkeeping on `virtio`.
- Bitrate is always a derived metric.

## 2. Where the measurement happens

The measurement path spans three layers.

### Layer A: the app decides when to sample

`src/app/ixy-fwd.c` contains the sampling loop:

1. `main()` initializes two devices with `ixy_init()`.
2. It creates two stats snapshots per device: `stats` and `stats_old`.
3. It records `last_stats_printed = monotonic_time()`.
4. In the forwarding loop, it only checks the clock every 4096 iterations.
5. Once more than one second has elapsed, it reads fresh stats and prints the delta.

Relevant code:

- `src/app/ixy-fwd.c:36-42`
- `src/app/ixy-fwd.c:49-61`

The key logic is:

```c
if ((counter++ & 0xFFF) == 0) {
	uint64_t time = monotonic_time();
	if (time - last_stats_printed > 1000 * 1000 * 1000) {
		ixy_read_stats(dev1, &stats1);
		print_stats_diff(&stats1, &stats1_old, time - last_stats_printed);
		stats1_old = stats1;
		...
		last_stats_printed = time;
	}
}
```

This means:

- there is no per-packet timing,
- there is no separate benchmark thread,
- there is no synthetic "throughput test" path apart from normal forwarding,
- throughput is a periodic delta of counters over elapsed time.

### Layer B: generic stats code converts counts into rates

`src/stats.c` contains the generic timing and math:

- `monotonic_time()` reads `CLOCK_MONOTONIC` and returns nanoseconds.
- `print_stats_diff()` computes packet-rate and bit-rate deltas.
- `stats_init()` zeroes the software snapshot and asks the driver to clear or reset its baseline.

Relevant code:

- `src/stats.c:10-29`
- `src/stats.c:35-38`
- `src/stats.c:42-50`

The formulas are:

```text
Mpps = (delta_packets / 1,000,000) / elapsed_seconds

Mbit/s = ((delta_bytes / elapsed_seconds) * 8) / 1,000,000
         + (Mpps * 20 * 8)
```

Equivalent combined form:

```text
Mbit/s = ((delta_bytes + delta_packets * 20) * 8 / elapsed_seconds) / 1,000,000
```

The added `20` bytes represent:

- 8 bytes of preamble + SFD
- 12 bytes of inter-frame gap

That adjustment is explicitly documented in `src/stats.c:15-18`.

### Layer C: the driver supplies the raw counts

The app never knows how counters are collected. It calls the generic wrapper:

- `src/driver/device.h:45`
- `src/driver/device.h:66-68`

```c
static inline void ixy_read_stats(struct ixy_device* dev, struct device_stats* stats) {
	dev->read_stats(dev, stats);
}
```

Each driver installs its own `read_stats` callback:

- `ixgbe`: `src/driver/ixgbe.c:544-546`
- `virtio`: `src/driver/virtio.c:341-343`

## 3. What metrics ixy collects

The generic `device_stats` structure contains exactly four counters plus a back-pointer to the device:

- `rx_pkts`
- `tx_pkts`
- `rx_bytes`
- `tx_bytes`

See `src/stats.h:9-15`.

From those four counters, `ixy` prints:

- RX Mbit/s
- RX Mpps
- TX Mbit/s
- TX Mpps

It does not print:

- latency,
- jitter,
- queue occupancy,
- dropped-packet rate,
- per-queue rates,
- per-flow rates,
- CPU cycles per packet.

Those would need extra counters or extra instrumentation.

## 4. How packets are counted and timed

## 4.1 Timing

Timing is based on `monotonic_time()` in `src/stats.c:35-38`, which uses:

```c
clock_gettime(CLOCK_MONOTONIC, &timespec);
```

Important details:

- The sampling period is nominally one second.
- It is not exactly one second, because the code only checks the clock every 4096 loop iterations.
- That choice is deliberate to avoid paying the clock-read overhead on every iteration.
- The actual elapsed nanoseconds are passed into `print_stats_diff()`, so the reported rate compensates for sampling jitter.

So the method is:

1. avoid frequent timer reads in the fast path,
2. sample infrequently,
3. use the actual elapsed interval in the rate formula.

## 4.2 Counting on `ixgbe`

`ixgbe_read_stats()` reads these registers:

- `IXGBE_GPRC` at `0x04074`
- `IXGBE_GPTC` at `0x04080`
- `IXGBE_GORCL` and `IXGBE_GORCH` at `0x04088` and `0x0408C`
- `IXGBE_GOTCL` and `IXGBE_GOTCH` at `0x04090` and `0x04094`

See:

- `src/driver/ixgbe.c:635-645`
- `src/driver/ixgbe_type.h:945`
- `src/driver/ixgbe_type.h:948-952`

The code path is:

1. app calls `ixy_read_stats(dev, &stats)`
2. wrapper dispatches to `ixgbe_read_stats()`
3. driver reads hardware registers with `get_reg32()`
4. packet and byte values are added into the caller's cumulative `device_stats`
5. `print_stats_diff()` subtracts the previous snapshot and divides by elapsed time

Notably, `ixgbe_rx_batch()` and `ixgbe_tx_batch()` do not increment software throughput counters in the main stats path. The throughput report comes from the NIC stats registers, not from the ring-processing loop.

That means the normal `ixgbe` forwarding datapath and the measurement datapath are separate:

- datapath: `ixgbe_rx_batch()` and `ixgbe_tx_batch()`
- measurement path: `ixgbe_read_stats()`

### Important `ixgbe` assumption: clear-on-read behavior

`stats_init()` says it "clears the stats on the device" by calling:

```c
ixy_read_stats(dev, NULL);
```

See `src/stats.c:42-50`.

`ixgbe_read_stats()` itself does not explicitly write a reset register. It only reads the counters and optionally adds them to `device_stats`.

From that design, the code is clearly assuming that the `ixgbe` stats registers used here behave like read-and-clear counters or otherwise provide "since last read" semantics.

That assumption matters because `stats1` and `stats_old` are cumulative software snapshots:

- each `ixy_read_stats()` call adds the latest read value into `stats`
- `print_stats_diff()` subtracts old cumulative snapshots

If the hardware counters were plain monotonically increasing counters that do not clear on read, this code would double-count badly. The cumulative software snapshot model only works correctly if each read returns the increment since the previous baseline.

For a new driver, this is one of the first semantics you must verify.

## 4.3 Counting on `virtio`

`virtio` uses a different strategy.

The `virtio_device` struct stores software counters:

- `src/driver/virtio.h:8-18`

The RX path increments them here:

- `src/driver/virtio.c:393-395`

The TX path increments them here:

- `src/driver/virtio.c:460-462`

Then `virtio_read_stats()` copies them into `device_stats` and resets them to zero:

- `src/driver/virtio.c:313-321`

So the raw counting points are:

- RX: when a used RX descriptor is consumed and turned into a returned `pkt_buf`
- TX: when the buffer is queued into the TX virtqueue

This is simpler than the `ixgbe` path, but it changes the meaning:

- `ixgbe` TX stats come from NIC counters,
- `virtio` TX stats are counted in software when the driver queues descriptors,
- therefore `virtio` TX numbers are closer to "packets handed to the device" than "packets definitely observed on the wire."

The `virtio` code also explicitly documents a limitation:

- it is not thread-safe,
- it only really matches the current single-queue setup.

See `src/driver/virtio.c:308-312`.

## 5. How the reflector path relates to the measurement path

The reflect/forward path is in `src/app/ixy-fwd.c:9-23`.

What it actually does:

1. call `ixy_rx_batch()` to receive up to 32 buffers
2. touch byte `data[1]` in each packet
3. call `ixy_tx_batch()` on the same `pkt_buf*`
4. free any tail that could not be queued for TX

This line is the only payload mutation:

```c
bufs[i]->data[1]++;
```

The comment says why:

```c
// touch all packets, otherwise it's a completely unrealistic workload if the packet just stays in L3
```

That payload touch is not a measurement operation.

It only changes the workload so the benchmark is not an unrealistic zero-touch reflector. In other words:

- forwarding exercises RX and TX,
- payload touch exercises cache and write behavior,
- the stats path observes the resulting packet and byte counts,
- the stats path does not inspect or depend on the payload contents.

So the relationship is:

- the forwarder creates the traffic,
- the stats path measures the traffic after the fact.

There is no second, separate throughput datapath.

## 6. End-to-end measurement flow in `ixy-fwd`

This is the full sequence for one reporting interval.

1. `main()` sets up devices and zeroed snapshot structs.
2. `stats_init()` clears the software snapshots and resets the device baseline.
3. The forwarding loop moves real packets from RX to TX.
4. Every 4096 iterations, the app checks `CLOCK_MONOTONIC`.
5. Once more than one second has elapsed, the app calls `ixy_read_stats()`.
6. The driver returns fresh packet and byte counts.
7. `print_stats_diff()` computes deltas from the previous snapshot.
8. `print_stats_diff()` prints RX and TX in Mpps and Mbit/s.
9. The current snapshot becomes the old snapshot for the next interval.

For `ixgbe`, this is a periodic snapshot of hardware counters.

For `virtio`, this is a periodic drain of software counters.

## 7. Assumptions and limitations

The method is simple and cheap, but it has important assumptions.

### 7.1 It measures rates over intervals, not per-packet timestamps

This is throughput sampling, not packet timestamping.

- good for Mpps and Mbit/s,
- useless for latency or jitter.

### 7.2 The interval is approximate

The code checks time only every 4096 loop iterations.

Implications:

- lower timer overhead,
- slightly irregular reporting interval,
- acceptable because the exact elapsed nanoseconds are used in the calculation.

### 7.3 Bitrate is a model, not a perfect physical-layer measurement

`diff_mbit()` adds `20` bytes per packet for preamble/SFD/IFG.

Implications:

- the printed Mbit/s is intended to match link-rate intuition better for small packets,
- it is still an approximation based on the chosen overhead model.

The code does not add any other explicit overhead terms.

### 7.4 The `ixgbe` path assumes specific register semantics

As explained above, the cumulative software snapshot logic assumes the stats reads represent new increments, not the full lifetime total.

If a new NIC exposes monotonically increasing raw counters instead:

- keep the raw previous register values and subtract them, or
- change the stats layer to store raw snapshots directly instead of cumulative read-and-clear sums.

### 7.5 The meaning of "TX throughput" depends on the driver

This is an important limitation if you compare drivers.

- `ixgbe`: TX throughput is driven by NIC counters.
- `virtio`: TX throughput is driven by software enqueue counters.

So `ixy` does not enforce one global semantic definition of TX throughput across all drivers.

### 7.6 The method is aggregate, not per-queue

The generic stats struct is device-wide:

- one RX packet counter,
- one TX packet counter,
- one RX byte counter,
- one TX byte counter.

If you need per-queue measurement, you must extend the stats API.

### 7.7 The method does not directly expose drops

The forwarder drops packets when TX cannot queue them:

- `src/app/ixy-fwd.c:18-21`

Those drops are not printed as a dedicated metric.

However, on `ixgbe` you can infer them indirectly if:

- RX rate is high,
- TX rate is lower,
- and the application is freeing the unsent tail.

That inference is useful, but it is not the same as a true drop counter.

### 7.8 Startup reset is done twice per device in `ixy-fwd`

`ixy-fwd` calls `stats_init()` twice per device:

- once for `stats`
- once for `stats_old`

See `src/app/ixy-fwd.c:37-42`.

That means `ixy_read_stats(dev, NULL)` is also called twice at startup.

In the current code this is harmless because traffic has not started yet. But it is worth knowing if you copy the pattern into a driver where resetting counters is expensive or destructive.

## 8. What is reusable for an E810 `vfio-pci` driver

The reusable part is the architecture, not the exact register list.

You can reuse all of this almost unchanged:

- the `device_stats` struct shape from `src/stats.h`
- the `read_stats` function pointer in `struct ixy_device`
- the app-side snapshot-and-delta pattern in `src/app/ixy-fwd.c`
- `monotonic_time()` for timing
- `diff_mpps()` and `diff_mbit()` style rate calculations
- periodic sampling instead of per-packet timing

In other words, this part is generic:

1. maintain packet and byte counters somewhere,
2. sample them once per interval,
3. divide the deltas by elapsed time,
4. print Mpps and Mbit/s.

## 9. What must change for an E810 VF driver

The parts that must change are all below the `read_stats` callback.

### 9.1 Replace the counter source

You need an E810-specific `read_stats()` implementation.

That function must decide where the authoritative counts come from:

- hardware registers visible to the VF,
- device-specific queue or VSI statistics,
- mailbox/admin-queue fetched statistics,
- or software counters in your own RX/TX path.

This is the main hardware-specific piece.

### 9.2 Match the counter semantics

Before copying the `ixgbe` approach, verify:

- are the counters clear-on-read?
- are they monotonic lifetime counters?
- are they 32-bit or 64-bit?
- are they port-wide, VSI-wide, or queue-wide?
- do they count bytes as seen by software, MAC, or some virtual-function abstraction?

Then choose one of these two models:

Model A: read-and-clear counters

- use the same pattern as `ixgbe`
- `read_stats()` reads the new increments
- `stats_init()` can clear the baseline with `ixy_read_stats(dev, NULL)`

Model B: monotonic counters

- store previous raw values somewhere
- each read computes `delta = raw_now - raw_prev`
- accumulate the delta into `device_stats`
- update `raw_prev`

If your hardware counters can wrap, handle wrap explicitly.

### 9.3 Decide what TX means

You should choose this intentionally for your driver:

- "TX attempted": count when software queues a descriptor
- "TX completed": count when the device reports completion
- "TX on wire": count from hardware transmitted-packet counters

If you want behavior closest to `ixgbe`, prefer hardware TX counters or completion-based accounting, not enqueue-time accounting.

### 9.4 Make it multi-queue safe if needed

The current `virtio` comment is a warning for future driver authors: software counters need proper synchronization.

For an E810 VF driver with multiple queues or threads, you will probably want:

- per-queue counters,
- aggregation at stats-read time,
- or atomic counters if you accept the cost.

That is especially important if your VF driver is more concurrent than `ixy`'s current examples.

### 9.5 Keep the measurement path off the critical path

One of the nice properties of `ixy`'s approach is that the measurement overhead is low:

- no per-packet timestamping,
- no extra memory touch in the measurement path,
- no timer read on every loop iteration.

That design is worth preserving in your E810 driver.

## 10. Recommended E810 implementation strategy

If you want to copy the spirit of `ixy`, the cleanest plan is:

1. keep the app-side logic exactly the same,
2. add `e810_read_stats()` and assign it to `dev->ixy.read_stats`,
3. expose four counters: `rx_pkts`, `tx_pkts`, `rx_bytes`, `tx_bytes`,
4. decide whether they come from hardware stats or software accounting,
5. normalize the semantics so one stats read returns one interval's worth of increments,
6. reuse `print_stats_diff()` unchanged unless you want a different bitrate model.

If hardware stats are cleanly available to the VF, that is the closest match to `ixgbe`.

If they are not, the fallback is:

- count RX in the point where a descriptor becomes a delivered packet,
- count TX at completion or at least at successful queue submission,
- aggregate per queue,
- sample once per second.

## 11. Bottom line

`ixy` does not have a separate throughput-measurement engine hidden somewhere else in the codebase.

Its throughput reporting is a simple side-band sampling system:

- forwarding moves packets,
- drivers maintain or expose counters,
- the app snapshots those counters periodically,
- `stats.c` converts the deltas into Mpps and Mbit/s.

For `ixgbe`, that is a hardware-counter-based measurement path.

For a new E810 `vfio-pci` driver, the reusable idea is the snapshot-plus-delta structure. The hardware-specific work is implementing `read_stats()` with the correct counter source and counter semantics for your VF.

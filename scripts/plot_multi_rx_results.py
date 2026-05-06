#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter


DEFAULT_RESULTS_ROOT = Path("results/multi-rx")
DEFAULT_QUEUE_SET = [1, 2, 4, 8]
PERF_PREFERRED_PLOTS = [
    "tx_wire_gbps",
    "tx_mpps",
    "ipc",
    "cycles_per_rx_pkt",
    "branch_miss_rate",
    "frontend_stall_per_cycle",
    "backend_stall_per_cycle",
    "l1d_load_misses_per_kinst",
    "llc_load_misses_per_kinst",
    "dtlb_load_misses_per_kinst",
    "itlb_load_misses_per_kinst",
    "faults_per_kinst",
]
CROSS_CONFIG_METRICS = [
    "tx_wire_gbps_vs_queue_best",
    "tx_mpps_vs_queue_best",
    "ipc",
    "cycles_per_rx_pkt",
    "instructions_per_rx_pkt",
    "branch_miss_rate",
    "frontend_stall_per_cycle",
    "backend_stall_per_cycle",
    "node_load_miss_rate",
    "l1d_load_misses_per_kinst",
    "l1i_load_misses_per_kinst",
    "llc_load_misses_per_kinst",
    "llc_store_misses_per_kinst",
    "dtlb_load_misses_per_kinst",
    "itlb_load_misses_per_kinst",
    "faults_per_kinst",
    "cs_per_kinst",
    "cpu_migrations_per_kinst",
    "doorbells_per_mpkt",
    "tx_ring_full_per_mpkt",
    "pool_empty_per_mpkt",
    "rx_errors_per_mpkt",
    "rx_short_per_mpkt",
    "zero_copy_share",
]
PERF_METADATA_FIELDS = {
    "timestamp",
    "bdf",
    "duration_s",
    "rx_queues",
    "rx_batch_size",
    "tx_batch_size",
    "repeat_index",
    "status",
    "log_file",
    "perf_file",
    "config_label",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot multi_rx_bench aggregate.csv results into PNG files."
    )
    parser.add_argument(
        "path",
        nargs="?",
        default=str(DEFAULT_RESULTS_ROOT),
        help=(
            "Result directory containing aggregate.csv, or the results root when used "
            "with --all-complete"
        ),
    )
    parser.add_argument(
        "--all-complete",
        action="store_true",
        help="Plot every complete result set under the given results root",
    )
    return parser.parse_args()


def to_int(value: str | None) -> int | None:
    if value is None or value == "":
        return None
    return int(value)


def to_float(value: str | None) -> float | None:
    if value is None or value == "":
        return None
    return float(value)


def to_number(value: str | None) -> float | int | None:
    if value is None:
        return None
    text = value.strip().replace(",", "")
    if not text or text.startswith("<"):
        return None
    try:
        if any(ch in text for ch in ".eE"):
            return float(text)
        return int(text)
    except ValueError:
        return None


def safe_div(numerator: float | int | None, denominator: float | int | None) -> float | None:
    if numerator is None or denominator in (None, 0):
        return None
    return float(numerator) / float(denominator)


def run_key(row: dict) -> tuple:
    return (
        row.get("timestamp"),
        row.get("bdf"),
        row.get("duration_s"),
        row.get("rx_queues"),
        row.get("rx_batch_size"),
        row.get("tx_batch_size"),
        row.get("repeat_index"),
        row.get("log_file"),
    )


def config_label(row: dict) -> str:
    return f"q{row['rx_queues']}-rb{row['rx_batch_size']}-tb{row['tx_batch_size']}"


def config_sort_key(row: dict) -> tuple[int, int, int]:
    return (row["rx_queues"], row["rx_batch_size"], row["tx_batch_size"])


def load_rows(result_dir: Path) -> tuple[list[dict], list[str]]:
    csv_path = result_dir / "aggregate.csv"
    if not csv_path.is_file():
        raise FileNotFoundError(f"missing aggregate.csv in {result_dir}")

    with csv_path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = [name for name in (reader.fieldnames or []) if name]
        rows = []
        for raw in reader:
            if raw is None:
                continue
            row = {k: v for k, v in raw.items() if k}
            if not any((value or "").strip() for value in row.values()):
                continue

            row["rx_queues"] = to_int(row.get("rx_queues"))
            row["rx_batch_size"] = to_int(row.get("rx_batch_size"))
            row["tx_batch_size"] = to_int(row.get("tx_batch_size"))
            row["repeat_index"] = to_int(row.get("repeat_index")) or 1
            row["seconds"] = to_float(row.get("seconds"))
            row["tx_wire_gbps"] = to_float(row.get("tx_wire_gbps"))
            row["rx_wire_gbps"] = to_float(row.get("rx_wire_gbps"))
            row["tx_mpps"] = to_float(row.get("tx_mpps"))
            row["rx_mpps"] = to_float(row.get("rx_mpps"))
            row["tx_l2_gbps"] = to_float(row.get("tx_l2_gbps"))
            row["rx_l2_gbps"] = to_float(row.get("rx_l2_gbps"))
            row["tx_ring_full"] = to_int(row.get("tx_ring_full"))
            row["rx_short"] = to_int(row.get("rx_short"))
            row["rx_errors"] = to_int(row.get("rx_errors"))
            row["pool_empty"] = to_int(row.get("pool_empty"))
            row["doorbells"] = to_int(row.get("doorbells"))
            row["rx_pkts"] = to_int(row.get("rx_pkts"))
            row["rx_bytes"] = to_int(row.get("rx_bytes"))
            row["tx_pkts"] = to_int(row.get("tx_pkts"))
            row["tx_bytes"] = to_int(row.get("tx_bytes"))
            row["zero_copy_pkts"] = to_int(row.get("zero_copy_pkts"))
            row["zero_copy_bytes"] = to_int(row.get("zero_copy_bytes"))
            row["vsi"] = to_int(row.get("vsi"))
            row["gorc_delta"] = to_int(row.get("gorc_delta"))
            row["gotc_delta"] = to_int(row.get("gotc_delta"))
            row.setdefault("status", "ok")
            rows.append(row)

    return rows, fieldnames


def load_perf_rows(result_dir: Path) -> list[dict]:
    perf_path = result_dir / "perf.csv"
    if not perf_path.is_file():
        return []

    with perf_path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        rows = []
        for raw in reader:
            if raw is None:
                continue
            row = {k: v for k, v in raw.items() if k}
            if not any((value or "").strip() for value in row.values()):
                continue

            row["rx_queues"] = to_int(row.get("rx_queues"))
            row["rx_batch_size"] = to_int(row.get("rx_batch_size"))
            row["tx_batch_size"] = to_int(row.get("tx_batch_size"))
            row["repeat_index"] = to_int(row.get("repeat_index")) or 1
            row["value"] = to_number(row.get("value"))
            row["counter_runtime"] = to_number(row.get("counter_runtime"))
            row["running_pct"] = to_number(row.get("running_pct"))
            row["metric_value"] = to_number(row.get("metric_value"))
            row.setdefault("status", "ok")
            rows.append(row)

    return rows


def infer_schema(rows: list[dict], fieldnames: list[str]) -> str:
    has_rb = "rx_batch_size" in fieldnames and any(row.get("rx_batch_size") is not None for row in rows)
    has_tb = "tx_batch_size" in fieldnames and any(row.get("tx_batch_size") is not None for row in rows)

    if not has_rb and not has_tb:
        return "queue_only"
    if has_rb and not has_tb:
        return "queue_rx"
    matched_only = all(
        row.get("rx_batch_size") == row.get("tx_batch_size")
        for row in rows
        if row.get("rx_batch_size") is not None and row.get("tx_batch_size") is not None
    )
    return "matched" if matched_only else "matrix"


def classify_result_dir(result_dir: Path) -> tuple[bool, str]:
    try:
        rows, fieldnames = load_rows(result_dir)
    except FileNotFoundError:
        return False, "missing aggregate.csv"

    if not rows:
        return False, "empty aggregate.csv"

    if any(row.get("status", "ok") != "ok" for row in rows):
        return False, "contains failed rows"

    schema = infer_schema(rows, fieldnames)
    queue_values = sorted({row["rx_queues"] for row in rows if row.get("rx_queues") is not None})

    if schema == "queue_only":
        if queue_values != DEFAULT_QUEUE_SET:
            return False, "legacy queue-only run does not record full default queue set"
        return len(rows) == len(DEFAULT_QUEUE_SET), "queue_only"

    rx_batches = sorted({row["rx_batch_size"] for row in rows if row.get("rx_batch_size") is not None})

    if schema == "queue_rx":
        expected = {(q, rb) for q in queue_values for rb in rx_batches}
        actual = {(row["rx_queues"], row["rx_batch_size"]) for row in rows}
        return actual == expected, "queue_rx"

    tx_batches = sorted({row["tx_batch_size"] for row in rows if row.get("tx_batch_size") is not None})

    if schema == "matched":
        expected = {(q, rb, rb) for q in queue_values for rb in rx_batches}
    else:
        expected = {(q, rb, tb) for q in queue_values for rb in rx_batches for tb in tx_batches}

    actual = {
        (row["rx_queues"], row["rx_batch_size"], row["tx_batch_size"])
        for row in rows
    }
    return actual == expected, schema


def scalar_formatter() -> ScalarFormatter:
    formatter = ScalarFormatter()
    formatter.set_scientific(False)
    return formatter


def unique_sorted(rows: list[dict], key: str) -> list[int]:
    return sorted({row[key] for row in rows if row.get(key) is not None})


def best_by(rows: list[dict], group_key: str) -> dict[int, dict]:
    grouped: dict[int, list[dict]] = defaultdict(list)
    for row in rows:
        grouped[row[group_key]].append(row)
    return {group: max(items, key=lambda row: row["tx_wire_gbps"]) for group, items in grouped.items()}


def representative_rows(rows: list[dict], metric: str = "tx_wire_gbps") -> list[dict]:
    grouped: dict[tuple[int, int, int], list[dict]] = defaultdict(list)
    for row in rows:
        grouped[config_sort_key(row)].append(row)

    representatives = []
    for config in sorted(grouped):
        samples = [row for row in grouped[config] if row.get(metric) is not None]
        if not samples:
            continue
        ordered = sorted(samples, key=lambda row: (row[metric], row.get("repeat_index", 0)))
        representatives.append(ordered[(len(ordered) - 1) // 2])
    return representatives


def finalize_figure(fig: plt.Figure, output_path: Path) -> None:
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def plot_queue_scaling(result_dir: Path, rows: list[dict], schema: str, plot_dir: Path) -> Path:
    rows = representative_rows(rows)
    queues = unique_sorted(rows, "rx_queues")
    fig, ax = plt.subplots(figsize=(7, 4.5))

    if schema == "queue_only":
        ordered = sorted(rows, key=lambda row: row["rx_queues"])
        ax.plot(
            [row["rx_queues"] for row in ordered],
            [row["tx_wire_gbps"] for row in ordered],
            marker="o",
            linewidth=2,
            label="Measured",
        )
    else:
        overall = best_by(rows, "rx_queues")
        ax.plot(
            queues,
            [overall[q]["tx_wire_gbps"] for q in queues],
            marker="o",
            linewidth=2,
            label="Best overall",
        )

        if schema == "matrix":
            matched_rows = [row for row in rows if row["rx_batch_size"] == row["tx_batch_size"]]
            matched = best_by(matched_rows, "rx_queues")
            ax.plot(
                queues,
                [matched[q]["tx_wire_gbps"] for q in queues],
                marker="s",
                linewidth=2,
                label="Best matched",
            )

    ax.set_title(f"{result_dir.name}: TX throughput vs queues")
    ax.set_xlabel("RX queues")
    ax.set_ylabel("TX wire Gbps")
    ax.set_xticks(queues)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    output_path = plot_dir / "queue_scaling.png"
    finalize_figure(fig, output_path)
    return output_path


def plot_matched_throughput(result_dir: Path, rows: list[dict], schema: str, plot_dir: Path) -> Path | None:
    if schema == "queue_only":
        return None

    plot_rows = representative_rows(rows)
    if schema == "matrix":
        plot_rows = [row for row in rows if row["rx_batch_size"] == row["tx_batch_size"]]
        plot_rows = representative_rows(plot_rows)

    batches = unique_sorted(plot_rows, "rx_batch_size")
    queues = unique_sorted(plot_rows, "rx_queues")
    fig, ax = plt.subplots(figsize=(7.5, 4.8))

    for queue in queues:
        ordered = sorted(
            [row for row in plot_rows if row["rx_queues"] == queue],
            key=lambda row: row["rx_batch_size"],
        )
        ax.plot(
            [row["rx_batch_size"] for row in ordered],
            [row["tx_wire_gbps"] for row in ordered],
            marker="o",
            linewidth=2,
            label=f"q={queue}",
        )

    ax.set_title(f"{result_dir.name}: matched batch throughput")
    ax.set_xlabel("Batch size")
    ax.set_ylabel("TX wire Gbps")
    ax.set_xscale("log", base=2)
    ax.set_xticks(batches)
    ax.xaxis.set_major_formatter(scalar_formatter())
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    output_path = plot_dir / "matched_throughput.png"
    finalize_figure(fig, output_path)
    return output_path


def plot_best_over_tx_batch(result_dir: Path, rows: list[dict], plot_dir: Path) -> Path:
    rows = representative_rows(rows)
    batches = unique_sorted(rows, "rx_batch_size")
    queues = unique_sorted(rows, "rx_queues")
    fig, ax = plt.subplots(figsize=(7.5, 4.8))

    for queue in queues:
        best_rows = []
        for batch in batches:
            subset = [
                row for row in rows if row["rx_queues"] == queue and row["rx_batch_size"] == batch
            ]
            if subset:
                best_rows.append(max(subset, key=lambda row: row["tx_wire_gbps"]))
        ax.plot(
            [row["rx_batch_size"] for row in best_rows],
            [row["tx_wire_gbps"] for row in best_rows],
            marker="o",
            linewidth=2,
            label=f"q={queue}",
        )

    ax.set_title(f"{result_dir.name}: best TX throughput for each RX batch")
    ax.set_xlabel("RX batch size")
    ax.set_ylabel("Best TX wire Gbps over TX batch sweep")
    ax.set_xscale("log", base=2)
    ax.set_xticks(batches)
    ax.xaxis.set_major_formatter(scalar_formatter())
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    output_path = plot_dir / "best_over_tx_batch.png"
    finalize_figure(fig, output_path)
    return output_path


def plot_matrix_heatmaps(result_dir: Path, rows: list[dict], plot_dir: Path) -> Path:
    rows = representative_rows(rows)
    rx_batches = unique_sorted(rows, "rx_batch_size")
    tx_batches = unique_sorted(rows, "tx_batch_size")
    queues = unique_sorted(rows, "rx_queues")

    cols = 2 if len(queues) > 1 else 1
    grid_rows = math.ceil(len(queues) / cols)
    fig, axes = plt.subplots(grid_rows, cols, figsize=(6.4 * cols, 4.8 * grid_rows))
    if grid_rows > 1:
        fig.subplots_adjust(hspace=0.32, wspace=0.18, top=0.90)
    else:
        fig.subplots_adjust(wspace=0.18, top=0.88)
    if hasattr(axes, "flat"):
        axes = list(axes.flat)
    else:
        axes = [axes]

    cmap = plt.cm.viridis.copy()
    cmap.set_bad(color="#d9d9d9")
    vmin = min(row["tx_wire_gbps"] for row in rows)
    vmax = max(row["tx_wire_gbps"] for row in rows)
    image = None

    for axis, queue in zip(axes, queues):
        grid = [[math.nan for _ in rx_batches] for _ in tx_batches]
        for row in rows:
            if row["rx_queues"] != queue:
                continue
            tx_index = tx_batches.index(row["tx_batch_size"])
            rx_index = rx_batches.index(row["rx_batch_size"])
            grid[tx_index][rx_index] = row["tx_wire_gbps"]

        image = axis.imshow(grid, origin="lower", aspect="auto", cmap=cmap, vmin=vmin, vmax=vmax)
        axis.set_title(f"q={queue}")
        axis.set_xlabel("RX batch size")
        axis.set_ylabel("TX batch size")
        axis.set_xticks(range(len(rx_batches)), labels=rx_batches, rotation=45)
        axis.set_yticks(range(len(tx_batches)), labels=tx_batches)

    for axis in axes[len(queues):]:
        axis.set_visible(False)

    fig.suptitle(f"{result_dir.name}: TX throughput heatmaps", fontsize=14)
    if image is not None:
        fig.colorbar(image, ax=axes[: len(queues)], label="TX wire Gbps", shrink=0.92)
    output_path = plot_dir / "tx_heatmaps.png"
    finalize_figure(fig, output_path)
    return output_path


def sanitize_metric_name(name: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in name).strip("_").lower()


def quantile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    if len(values) == 1:
        return values[0]

    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[int(position)]
    weight = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * weight


def median_abs_deviation(values: list[float], center: float) -> float | None:
    if not values:
        return None
    deviations = [abs(value - center) for value in values]
    return float(statistics.median(deviations))


def ratio_to_baseline(value: float | None, baseline: float | None) -> float | None:
    if value is None or baseline in (None, 0):
        return None
    return float(value) / float(baseline)


def deviation_direction(value: float | None, baseline: float | None) -> str:
    if value is None or baseline is None:
        return "unknown"
    if value > baseline:
        return "above"
    if value < baseline:
        return "below"
    return "equal"


def severity_score(
    value: float | None,
    modified_z_score: float | None,
    lower_fence: float | None,
    upper_fence: float | None,
    iqr: float | None,
) -> float:
    score = abs(float(modified_z_score)) if modified_z_score is not None else 0.0
    if value is None or lower_fence is None or upper_fence is None or iqr in (None, 0):
        return score
    if value < lower_fence:
        score = max(score, (lower_fence - value) / iqr)
    elif value > upper_fence:
        score = max(score, (value - upper_fence) / iqr)
    return score


def derive_metrics(row: dict) -> dict[str, float | None]:
    instructions = row.get("instructions")
    cycles = row.get("cycles")
    rx_pkts = row.get("rx_pkts")
    tx_pkts = row.get("tx_pkts")
    branches = row.get("branches")
    branch_misses = row.get("branch-misses")
    node_loads = row.get("node-loads")
    node_load_misses = row.get("node-load-misses")

    def per_kinst(event_name: str) -> float | None:
        return safe_div((row.get(event_name) or 0) * 1000.0, instructions)

    def per_mpkt(value: float | int | None, packets: float | int | None) -> float | None:
        return safe_div((value or 0) * 1_000_000.0, packets)

    return {
        "ipc": safe_div(instructions, cycles),
        "branch_miss_rate": safe_div(branch_misses, branches),
        "frontend_stall_per_cycle": safe_div(row.get("stalled-cycles-frontend"), cycles),
        "backend_stall_per_cycle": safe_div(row.get("stalled-cycles-backend"), cycles),
        "node_load_miss_rate": safe_div(node_load_misses, node_loads),
        "l1d_load_misses_per_kinst": per_kinst("L1-dcache-load-misses"),
        "l1i_load_misses_per_kinst": per_kinst("L1-icache-load-misses"),
        "llc_load_misses_per_kinst": per_kinst("LLC-load-misses"),
        "llc_store_misses_per_kinst": per_kinst("LLC-store-misses"),
        "l1d_prefetches_per_kinst": per_kinst("L1-dcache-prefetches"),
        "dtlb_load_misses_per_kinst": per_kinst("dTLB-load-misses"),
        "itlb_load_misses_per_kinst": per_kinst("iTLB-load-misses"),
        "faults_per_kinst": per_kinst("faults"),
        "cs_per_kinst": per_kinst("cs"),
        "cpu_migrations_per_kinst": per_kinst("cpu-migrations"),
        "doorbells_per_mpkt": per_mpkt(row.get("doorbells"), tx_pkts),
        "tx_ring_full_per_mpkt": per_mpkt(row.get("tx_ring_full"), tx_pkts),
        "pool_empty_per_mpkt": per_mpkt(row.get("pool_empty"), rx_pkts),
        "rx_errors_per_mpkt": per_mpkt(row.get("rx_errors"), rx_pkts),
        "rx_short_per_mpkt": per_mpkt(row.get("rx_short"), rx_pkts),
        "zero_copy_share": safe_div(row.get("zero_copy_pkts"), tx_pkts),
        "cycles_per_rx_pkt": safe_div(cycles, rx_pkts),
        "instructions_per_rx_pkt": safe_div(instructions, rx_pkts),
    }


def build_perf_derived_rows(aggregate_rows: list[dict], perf_rows: list[dict]) -> tuple[list[dict], list[str]]:
    aggregate_by_key = {
        run_key(row): row
        for row in aggregate_rows
        if row.get("status", "ok") == "ok"
    }
    perf_by_key: dict[tuple, dict[str, float | int]] = defaultdict(dict)
    raw_events = sorted(
        {
            row["event"]
            for row in perf_rows
            if row.get("status", "ok") == "ok" and row.get("event") and row.get("value") is not None
        }
    )

    for row in perf_rows:
        if row.get("status", "ok") != "ok":
            continue
        event = row.get("event")
        value = row.get("value")
        if not event or value is None:
            continue
        perf_by_key[run_key(row)][event] = value

    derived_rows: list[dict] = []
    for key, aggregate_row in sorted(aggregate_by_key.items(), key=lambda item: config_sort_key(item[1]) + (item[1]["repeat_index"],)):
        perf_map = perf_by_key.get(key)
        if not perf_map:
            continue

        row = {
            "timestamp": aggregate_row.get("timestamp"),
            "bdf": aggregate_row.get("bdf"),
            "duration_s": aggregate_row.get("duration_s"),
            "rx_queues": aggregate_row.get("rx_queues"),
            "rx_batch_size": aggregate_row.get("rx_batch_size"),
            "tx_batch_size": aggregate_row.get("tx_batch_size"),
            "repeat_index": aggregate_row.get("repeat_index"),
            "status": aggregate_row.get("status"),
            "log_file": aggregate_row.get("log_file"),
            "perf_file": next(
                (
                    perf_row.get("perf_file")
                    for perf_row in perf_rows
                    if run_key(perf_row) == key and perf_row.get("perf_file")
                ),
                "",
            ),
            "config_label": config_label(aggregate_row),
            "seconds": aggregate_row.get("seconds"),
            "tx_wire_gbps": aggregate_row.get("tx_wire_gbps"),
            "rx_wire_gbps": aggregate_row.get("rx_wire_gbps"),
            "tx_mpps": aggregate_row.get("tx_mpps"),
            "rx_mpps": aggregate_row.get("rx_mpps"),
            "tx_l2_gbps": aggregate_row.get("tx_l2_gbps"),
            "rx_l2_gbps": aggregate_row.get("rx_l2_gbps"),
            "rx_pkts": aggregate_row.get("rx_pkts"),
            "rx_bytes": aggregate_row.get("rx_bytes"),
            "tx_pkts": aggregate_row.get("tx_pkts"),
            "tx_bytes": aggregate_row.get("tx_bytes"),
            "zero_copy_pkts": aggregate_row.get("zero_copy_pkts"),
            "zero_copy_bytes": aggregate_row.get("zero_copy_bytes"),
            "tx_ring_full": aggregate_row.get("tx_ring_full"),
            "rx_short": aggregate_row.get("rx_short"),
            "rx_errors": aggregate_row.get("rx_errors"),
            "pool_empty": aggregate_row.get("pool_empty"),
            "doorbells": aggregate_row.get("doorbells"),
            "vsi": aggregate_row.get("vsi"),
            "gorc_delta": aggregate_row.get("gorc_delta"),
            "gotc_delta": aggregate_row.get("gotc_delta"),
        }

        for event in raw_events:
            row[event] = perf_map.get(event)

        row.update(derive_metrics(row))
        derived_rows.append(row)

    queue_best_tx_wire_gbps: dict[int, float] = {}
    queue_best_tx_mpps: dict[int, float] = {}
    for row in derived_rows:
        queue = row["rx_queues"]
        tx_wire_gbps = row.get("tx_wire_gbps")
        tx_mpps = row.get("tx_mpps")
        if tx_wire_gbps is not None:
            queue_best_tx_wire_gbps[queue] = max(queue_best_tx_wire_gbps.get(queue, tx_wire_gbps), tx_wire_gbps)
        if tx_mpps is not None:
            queue_best_tx_mpps[queue] = max(queue_best_tx_mpps.get(queue, tx_mpps), tx_mpps)

    for row in derived_rows:
        queue = row["rx_queues"]
        row["tx_wire_gbps_vs_queue_best"] = safe_div(row.get("tx_wire_gbps"), queue_best_tx_wire_gbps.get(queue))
        row["tx_mpps_vs_queue_best"] = safe_div(row.get("tx_mpps"), queue_best_tx_mpps.get(queue))

    return derived_rows, raw_events


def write_csv_rows(path: Path, fieldnames: list[str], rows: list[dict]) -> Path:
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    return path


def numeric_metric_names(rows: list[dict]) -> list[str]:
    metric_names = set()
    for row in rows:
        for key, value in row.items():
            if key in PERF_METADATA_FIELDS:
                continue
            if isinstance(value, (int, float)):
                metric_names.add(key)
    return sorted(metric_names)


def summarize_metric(values: list[float]) -> dict[str, float | int | None]:
    ordered = sorted(values)
    median = float(statistics.median(ordered))
    mad = median_abs_deviation(ordered, median)
    q1 = quantile(ordered, 0.25)
    q3 = quantile(ordered, 0.75)
    iqr = None if q1 is None or q3 is None else q3 - q1
    lower_fence = None if q1 is None or iqr is None else q1 - 1.5 * iqr
    upper_fence = None if q3 is None or iqr is None else q3 + 1.5 * iqr

    iqr_outliers = 0
    modified_z_outliers = 0
    for value in ordered:
        if lower_fence is not None and upper_fence is not None and (value < lower_fence or value > upper_fence):
            iqr_outliers += 1
        if mad not in (None, 0):
            modified_z = 0.6745 * (value - median) / mad
            if abs(modified_z) > 3.5:
                modified_z_outliers += 1

    return {
        "sample_count": len(ordered),
        "min": ordered[0],
        "max": ordered[-1],
        "mean": float(statistics.mean(ordered)),
        "stdev": float(statistics.stdev(ordered)) if len(ordered) > 1 else 0.0,
        "median": median,
        "mad": mad,
        "q1": q1,
        "q3": q3,
        "iqr": iqr,
        "lower_fence": lower_fence,
        "upper_fence": upper_fence,
        "iqr_outlier_count": iqr_outliers,
        "modified_z_outlier_count": modified_z_outliers,
    }


def build_perf_stats(derived_rows: list[dict]) -> tuple[list[dict], list[dict], list[str]]:
    metric_names = numeric_metric_names(derived_rows)
    summary_rows: list[dict] = []
    outlier_rows: list[dict] = []

    grouped: dict[tuple[int, int, int], list[dict]] = defaultdict(list)
    for row in derived_rows:
        grouped[config_sort_key(row)].append(row)

    for config in sorted(grouped):
        rows = sorted(grouped[config], key=lambda row: row["repeat_index"])
        template = rows[0]
        for metric in metric_names:
            samples = [(row, row.get(metric)) for row in rows if isinstance(row.get(metric), (int, float))]
            if not samples:
                continue

            values = [float(value) for _, value in samples]
            stats = summarize_metric(values)
            summary_rows.append(
                {
                    "rx_queues": template["rx_queues"],
                    "rx_batch_size": template["rx_batch_size"],
                    "tx_batch_size": template["tx_batch_size"],
                    "config_label": template["config_label"],
                    "metric": metric,
                    **stats,
                }
            )

            median = stats["median"]
            mad = stats["mad"]
            lower_fence = stats["lower_fence"]
            upper_fence = stats["upper_fence"]
            q1 = stats["q1"]
            q3 = stats["q3"]
            iqr = stats["iqr"]

            for row, value in samples:
                numeric_value = float(value)
                modified_z_score = None
                if mad not in (None, 0):
                    modified_z_score = 0.6745 * (numeric_value - float(median)) / float(mad)
                is_iqr_outlier = (
                    lower_fence is not None
                    and upper_fence is not None
                    and (numeric_value < float(lower_fence) or numeric_value > float(upper_fence))
                )
                is_modified_z_outlier = modified_z_score is not None and abs(modified_z_score) > 3.5
                if not is_iqr_outlier and not is_modified_z_outlier:
                    continue

                outlier_rows.append(
                    {
                        "rx_queues": row["rx_queues"],
                        "rx_batch_size": row["rx_batch_size"],
                        "tx_batch_size": row["tx_batch_size"],
                        "config_label": row["config_label"],
                        "repeat_index": row["repeat_index"],
                        "log_file": row["log_file"],
                        "perf_file": row.get("perf_file", ""),
                        "metric": metric,
                        "value": numeric_value,
                        "median": median,
                        "mad": mad,
                        "modified_z_score": modified_z_score,
                        "q1": q1,
                        "q3": q3,
                        "iqr": iqr,
                        "lower_fence": lower_fence,
                        "upper_fence": upper_fence,
                        "is_iqr_outlier": int(is_iqr_outlier),
                        "is_modified_z_outlier": int(is_modified_z_outlier),
                    }
                )

    return summary_rows, outlier_rows, metric_names


def build_cross_config_stats(derived_rows: list[dict]) -> tuple[list[dict], list[dict], list[dict], list[str]]:
    metric_names = [metric for metric in CROSS_CONFIG_METRICS if any(isinstance(row.get(metric), (int, float)) for row in derived_rows)]
    config_groups: dict[tuple[int, int, int], list[dict]] = defaultdict(list)
    for row in derived_rows:
        config_groups[config_sort_key(row)].append(row)

    config_metric_rows: list[dict] = []
    for config in sorted(config_groups):
        rows = sorted(config_groups[config], key=lambda row: row["repeat_index"])
        template = rows[0]
        for metric in metric_names:
            samples = [float(row[metric]) for row in rows if isinstance(row.get(metric), (int, float))]
            if not samples:
                continue
            config_metric_rows.append(
                {
                    "rx_queues": template["rx_queues"],
                    "rx_batch_size": template["rx_batch_size"],
                    "tx_batch_size": template["tx_batch_size"],
                    "config_label": template["config_label"],
                    "metric": metric,
                    "config_median": float(statistics.median(samples)),
                    "repeat_count": len(samples),
                    "repeat_mean": float(statistics.mean(samples)),
                    "repeat_stdev": float(statistics.stdev(samples)) if len(samples) > 1 else 0.0,
                }
            )

    summary_rows: list[dict] = []
    outlier_rows: list[dict] = []
    grouped: dict[tuple[int, str], list[dict]] = defaultdict(list)
    for row in config_metric_rows:
        grouped[(row["rx_queues"], row["metric"])].append(row)

    for (rx_queues, metric), rows in sorted(grouped.items()):
        values = [float(row["config_median"]) for row in rows]
        stats = summarize_metric(values)
        stats_no_sample_count = {key: value for key, value in stats.items() if key != "sample_count"}
        summary_rows.append(
            {
                "rx_queues": rx_queues,
                "metric": metric,
                "config_count": len(rows),
                "baseline_scope": "same_queue",
                **stats_no_sample_count,
            }
        )

        median = stats["median"]
        mad = stats["mad"]
        lower_fence = stats["lower_fence"]
        upper_fence = stats["upper_fence"]
        q1 = stats["q1"]
        q3 = stats["q3"]
        iqr = stats["iqr"]

        for row in rows:
            value = float(row["config_median"])
            modified_z_score = None
            if mad not in (None, 0):
                modified_z_score = 0.6745 * (value - float(median)) / float(mad)
            is_iqr_outlier = (
                lower_fence is not None
                and upper_fence is not None
                and (value < float(lower_fence) or value > float(upper_fence))
            )
            is_modified_z_outlier = modified_z_score is not None and abs(modified_z_score) > 3.5
            if not is_iqr_outlier and not is_modified_z_outlier:
                continue

            outlier_rows.append(
                {
                    **row,
                    "baseline_scope": "same_queue",
                    "baseline_median": median,
                    "baseline_mad": mad,
                    "baseline_q1": q1,
                    "baseline_q3": q3,
                    "baseline_iqr": iqr,
                    "lower_fence": lower_fence,
                    "upper_fence": upper_fence,
                    "ratio_to_baseline": ratio_to_baseline(value, median),
                    "direction": deviation_direction(value, median),
                    "modified_z_score": modified_z_score,
                    "severity_score": severity_score(value, modified_z_score, lower_fence, upper_fence, iqr),
                    "is_iqr_outlier": int(is_iqr_outlier),
                    "is_modified_z_outlier": int(is_modified_z_outlier),
                }
            )

    return summary_rows, outlier_rows, config_metric_rows, metric_names


def build_outlier_leaderboard(within_config_outliers: list[dict], cross_config_outliers: list[dict]) -> list[dict]:
    leaderboard_rows: list[dict] = []

    for row in within_config_outliers:
        value = float(row["value"])
        baseline = row.get("median")
        modified_z_score = row.get("modified_z_score")
        iqr = row.get("iqr")
        lower_fence = row.get("lower_fence")
        upper_fence = row.get("upper_fence")
        leaderboard_rows.append(
            {
                "scope": "within_config_repeat",
                "baseline_scope": "same_config",
                "baseline_kind": "repeat_median",
                "rx_queues": row["rx_queues"],
                "rx_batch_size": row["rx_batch_size"],
                "tx_batch_size": row["tx_batch_size"],
                "config_label": row["config_label"],
                "repeat_index": row["repeat_index"],
                "metric": row["metric"],
                "value": value,
                "baseline_value": baseline,
                "ratio_to_baseline": ratio_to_baseline(value, baseline),
                "direction": deviation_direction(value, baseline),
                "modified_z_score": modified_z_score,
                "severity_score": severity_score(value, modified_z_score, lower_fence, upper_fence, iqr),
                "log_file": row.get("log_file", ""),
                "perf_file": row.get("perf_file", ""),
                "is_iqr_outlier": row.get("is_iqr_outlier", 0),
                "is_modified_z_outlier": row.get("is_modified_z_outlier", 0),
            }
        )

    for row in cross_config_outliers:
        leaderboard_rows.append(
            {
                "scope": "cross_config_median",
                "baseline_scope": row["baseline_scope"],
                "baseline_kind": "queue_median",
                "rx_queues": row["rx_queues"],
                "rx_batch_size": row["rx_batch_size"],
                "tx_batch_size": row["tx_batch_size"],
                "config_label": row["config_label"],
                "repeat_index": "",
                "metric": row["metric"],
                "value": row["config_median"],
                "baseline_value": row["baseline_median"],
                "ratio_to_baseline": row.get("ratio_to_baseline"),
                "direction": row.get("direction", "unknown"),
                "modified_z_score": row.get("modified_z_score"),
                "severity_score": row.get("severity_score", 0.0),
                "log_file": "",
                "perf_file": "",
                "is_iqr_outlier": row.get("is_iqr_outlier", 0),
                "is_modified_z_outlier": row.get("is_modified_z_outlier", 0),
            }
        )

    leaderboard_rows = [row for row in leaderboard_rows if row.get("severity_score", 0.0) > 0.0]
    leaderboard_rows.sort(
        key=lambda row: (
            -float(row["severity_score"]),
            row["metric"],
            row["config_label"],
            int(row["repeat_index"]) if row["repeat_index"] not in ("", None) else 0,
        )
    )

    for index, row in enumerate(leaderboard_rows, start=1):
        row["rank"] = index

    return leaderboard_rows


def plot_perf_boxplot(result_dir: Path, derived_rows: list[dict], metric: str, plot_dir: Path) -> Path | None:
    grouped: dict[tuple[int, int, int], list[dict]] = defaultdict(list)
    for row in derived_rows:
        value = row.get(metric)
        if isinstance(value, (int, float)):
            grouped[config_sort_key(row)].append(row)

    if not grouped:
        return None

    queue_groups: dict[int, list[tuple[tuple[int, int, int], list[dict]]]] = defaultdict(list)
    for config in sorted(grouped):
        queue_groups[config[0]].append((config, grouped[config]))

    queues = sorted(queue_groups)
    rows_count = len(queues)
    fig_height = max(4.8, 3.6 * rows_count)
    max_group_size = max(len(items) for items in queue_groups.values())
    fig_width = max(8.0, min(24.0, 0.28 * max_group_size + 4.0))
    fig, axes = plt.subplots(rows_count, 1, figsize=(fig_width, fig_height), squeeze=False)
    axes_list = list(axes.flat)

    for axis, queue in zip(axes_list, queues):
        configs = queue_groups[queue]
        data = []
        labels = []
        for config, rows in configs:
            data.append([float(row[metric]) for row in sorted(rows, key=lambda item: item["repeat_index"])])
            labels.append(f"rb{config[1]}\ntb{config[2]}")

        axis.boxplot(data, showfliers=True)

        for index, values in enumerate(data, start=1):
            spread = max(len(values) - 1, 1)
            x_positions = [index + 0.18 * ((i - spread / 2) / spread) for i in range(len(values))]
            axis.scatter(x_positions, values, color="#1f77b4", alpha=0.65, s=18, zorder=3)

        tick_step = max(1, math.ceil(len(labels) / 16))
        tick_positions = list(range(1, len(labels) + 1, tick_step))
        tick_labels = [labels[position - 1] for position in tick_positions]
        axis.set_xticks(tick_positions)
        axis.set_xticklabels(tick_labels)
        axis.set_title(f"q={queue}")
        axis.set_xlabel("Configuration")
        axis.set_ylabel(metric)
        axis.grid(True, axis="y", alpha=0.3)
        axis.tick_params(axis="x", rotation=45, labelsize=8)

    fig.suptitle(f"{result_dir.name}: {metric}", fontsize=14)
    fig.tight_layout(rect=(0.0, 0.0, 1.0, 0.97))
    output_path = plot_dir / f"perf_boxplot_{sanitize_metric_name(metric)}.png"
    finalize_figure(fig, output_path)
    return output_path


def analyze_perf(result_dir: Path, aggregate_rows: list[dict], plot_dir: Path) -> list[Path]:
    perf_rows = load_perf_rows(result_dir)
    if not perf_rows:
        return []

    derived_rows, raw_events = build_perf_derived_rows(aggregate_rows, perf_rows)
    if not derived_rows:
        return []

    outputs: list[Path] = []
    derived_fieldnames = [
        "timestamp",
        "bdf",
        "duration_s",
        "rx_queues",
        "rx_batch_size",
        "tx_batch_size",
        "repeat_index",
        "status",
        "log_file",
        "perf_file",
        "config_label",
        "seconds",
        "tx_wire_gbps",
        "rx_wire_gbps",
        "tx_mpps",
        "rx_mpps",
        "tx_l2_gbps",
        "rx_l2_gbps",
        "rx_pkts",
        "rx_bytes",
        "tx_pkts",
        "tx_bytes",
        "zero_copy_pkts",
        "zero_copy_bytes",
        "tx_ring_full",
        "rx_short",
        "rx_errors",
        "pool_empty",
        "doorbells",
        "vsi",
        "gorc_delta",
        "gotc_delta",
        *raw_events,
        "ipc",
        "branch_miss_rate",
        "frontend_stall_per_cycle",
        "backend_stall_per_cycle",
        "node_load_miss_rate",
        "l1d_load_misses_per_kinst",
        "l1i_load_misses_per_kinst",
        "llc_load_misses_per_kinst",
        "llc_store_misses_per_kinst",
        "l1d_prefetches_per_kinst",
        "dtlb_load_misses_per_kinst",
        "itlb_load_misses_per_kinst",
        "faults_per_kinst",
        "cs_per_kinst",
        "cpu_migrations_per_kinst",
        "doorbells_per_mpkt",
        "tx_ring_full_per_mpkt",
        "pool_empty_per_mpkt",
        "rx_errors_per_mpkt",
        "rx_short_per_mpkt",
        "zero_copy_share",
        "cycles_per_rx_pkt",
        "instructions_per_rx_pkt",
        "tx_wire_gbps_vs_queue_best",
        "tx_mpps_vs_queue_best",
    ]
    outputs.append(write_csv_rows(result_dir / "perf_derived.csv", derived_fieldnames, derived_rows))

    summary_rows, outlier_rows, metric_names = build_perf_stats(derived_rows)
    summary_fieldnames = [
        "rx_queues",
        "rx_batch_size",
        "tx_batch_size",
        "config_label",
        "metric",
        "sample_count",
        "min",
        "max",
        "mean",
        "stdev",
        "median",
        "mad",
        "q1",
        "q3",
        "iqr",
        "lower_fence",
        "upper_fence",
        "iqr_outlier_count",
        "modified_z_outlier_count",
    ]
    outputs.append(write_csv_rows(result_dir / "perf_summary.csv", summary_fieldnames, summary_rows))

    outlier_fieldnames = [
        "rx_queues",
        "rx_batch_size",
        "tx_batch_size",
        "config_label",
        "repeat_index",
        "log_file",
        "perf_file",
        "metric",
        "value",
        "median",
        "mad",
        "modified_z_score",
        "q1",
        "q3",
        "iqr",
        "lower_fence",
        "upper_fence",
        "is_iqr_outlier",
        "is_modified_z_outlier",
    ]
    outputs.append(write_csv_rows(result_dir / "perf_outliers.csv", outlier_fieldnames, outlier_rows))

    cross_summary_rows, cross_outlier_rows, config_metric_rows, _ = build_cross_config_stats(derived_rows)
    cross_summary_fieldnames = [
        "rx_queues",
        "metric",
        "config_count",
        "baseline_scope",
        "min",
        "max",
        "mean",
        "stdev",
        "median",
        "mad",
        "q1",
        "q3",
        "iqr",
        "lower_fence",
        "upper_fence",
        "iqr_outlier_count",
        "modified_z_outlier_count",
    ]
    outputs.append(
        write_csv_rows(result_dir / "perf_cross_config_summary.csv", cross_summary_fieldnames, cross_summary_rows)
    )

    cross_metric_fieldnames = [
        "rx_queues",
        "rx_batch_size",
        "tx_batch_size",
        "config_label",
        "metric",
        "config_median",
        "repeat_count",
        "repeat_mean",
        "repeat_stdev",
    ]
    outputs.append(write_csv_rows(result_dir / "perf_cross_config_metrics.csv", cross_metric_fieldnames, config_metric_rows))

    cross_outlier_fieldnames = [
        "rx_queues",
        "rx_batch_size",
        "tx_batch_size",
        "config_label",
        "metric",
        "config_median",
        "repeat_count",
        "repeat_mean",
        "repeat_stdev",
        "baseline_scope",
        "baseline_median",
        "baseline_mad",
        "baseline_q1",
        "baseline_q3",
        "baseline_iqr",
        "lower_fence",
        "upper_fence",
        "ratio_to_baseline",
        "direction",
        "modified_z_score",
        "severity_score",
        "is_iqr_outlier",
        "is_modified_z_outlier",
    ]
    outputs.append(
        write_csv_rows(result_dir / "perf_cross_config_outliers.csv", cross_outlier_fieldnames, cross_outlier_rows)
    )

    leaderboard_rows = build_outlier_leaderboard(outlier_rows, cross_outlier_rows)
    leaderboard_fieldnames = [
        "rank",
        "scope",
        "baseline_scope",
        "baseline_kind",
        "rx_queues",
        "rx_batch_size",
        "tx_batch_size",
        "config_label",
        "repeat_index",
        "metric",
        "value",
        "baseline_value",
        "ratio_to_baseline",
        "direction",
        "modified_z_score",
        "severity_score",
        "log_file",
        "perf_file",
        "is_iqr_outlier",
        "is_modified_z_outlier",
    ]
    outputs.append(write_csv_rows(result_dir / "perf_outlier_leaderboard.csv", leaderboard_fieldnames, leaderboard_rows))

    plot_metrics = [metric for metric in PERF_PREFERRED_PLOTS if metric in metric_names]
    for metric in plot_metrics:
        output = plot_perf_boxplot(result_dir, derived_rows, metric, plot_dir)
        if output is not None:
            outputs.append(output)

    return outputs


def plot_result_dir(result_dir: Path) -> list[Path]:
    rows, fieldnames = load_rows(result_dir)
    ok_rows = [row for row in rows if row.get("status", "ok") == "ok" and row.get("tx_wire_gbps") is not None]
    if not ok_rows:
        raise RuntimeError(f"no successful rows in {result_dir}")

    schema = infer_schema(ok_rows, fieldnames)
    plot_dir = result_dir / "plots"
    plot_dir.mkdir(exist_ok=True)

    outputs = [plot_queue_scaling(result_dir, ok_rows, schema, plot_dir)]

    matched_plot = plot_matched_throughput(result_dir, ok_rows, schema, plot_dir)
    if matched_plot is not None:
        outputs.append(matched_plot)

    if schema == "matrix":
        outputs.append(plot_best_over_tx_batch(result_dir, ok_rows, plot_dir))
        outputs.append(plot_matrix_heatmaps(result_dir, ok_rows, plot_dir))

    outputs.extend(analyze_perf(result_dir, ok_rows, plot_dir))
    return outputs


def result_dirs_under(root: Path) -> list[Path]:
    return sorted(path for path in root.iterdir() if path.is_dir())


def main() -> int:
    args = parse_args()
    target = Path(args.path)

    if args.all_complete:
        if not target.is_dir():
            raise RuntimeError(f"results root does not exist: {target}")

        plotted = 0
        skipped = 0
        for result_dir in result_dirs_under(target):
            is_complete, reason = classify_result_dir(result_dir)
            if not is_complete:
                print(f"[plot] skipping {result_dir}: {reason}")
                skipped += 1
                continue

            outputs = plot_result_dir(result_dir)
            print(f"[plot] wrote {len(outputs)} artifact(s) for {result_dir}")
            plotted += 1

        if plotted == 0:
            raise RuntimeError(f"no complete result sets found under {target}")

        print(f"[plot] plotted {plotted} complete result set(s); skipped {skipped}")
        return 0

    if not target.is_dir():
        raise RuntimeError(f"result directory does not exist: {target}")

    outputs = plot_result_dir(target)
    for output in outputs:
        print(f"[plot] wrote {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"[plot] error: {exc}", file=sys.stderr)
        raise

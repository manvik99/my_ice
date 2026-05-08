from __future__ import annotations

from pathlib import Path
import math
import re

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from IPython.display import Markdown, display


IMPLEMENTATION_LABELS = {
    "c": "C",
    "rust": "Rust",
}

IMPLEMENTATION_COLORS = {
    "c": "#1f77b4",
    "rust": "#d62728",
}

NON_NUMERIC_SUMMARY_COLS = {
    "timestamp_utc",
    "implementation",
    "run_id",
    "status",
    "bdf",
    "hugepage_dir",
    "stdout_log",
    "stderr_log",
    "perf_file",
}

RATIO_METRICS = [
    ("cycles_per_pkt", "Cycles / pkt"),
    ("instructions_per_pkt", "Instr / pkt"),
    ("llc_load_misses_per_mpkt", "LLC miss / Mpkt"),
    ("dtlb_load_misses_per_mpkt", "dTLB miss / Mpkt"),
    ("itlb_load_misses_per_mpkt", "iTLB miss / Mpkt"),
    ("branch_miss_rate", "Branch miss rate"),
    ("doorbells_per_mpkt", "Doorbells / Mpkt"),
    ("mmio_writes_per_mpkt", "MMIO writes / Mpkt"),
]

HOTSPOT_LINE_RE = re.compile(r"^\s*[0-9]+\.[0-9]+%")


def latest_run_dir(results_root: Path) -> Path:
    run_dirs = sorted([path for path in results_root.iterdir() if path.is_dir()])
    if not run_dirs:
        raise FileNotFoundError(f"No result directories found under {results_root}")
    return run_dirs[-1]


def load_metadata(path: Path) -> dict[str, str]:
    data: dict[str, str] = {}
    if not path.exists():
        return data
    for line in path.read_text().splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        data[key] = value
    return data


def _read_csv(path: Path) -> pd.DataFrame:
    if not path.exists():
        return pd.DataFrame()
    return pd.read_csv(path)


def _coerce_numeric(df: pd.DataFrame) -> pd.DataFrame:
    for col in df.columns:
        if col not in NON_NUMERIC_SUMMARY_COLS:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    return df


def derive_metrics(df: pd.DataFrame) -> pd.DataFrame:
    if df.empty:
        return df

    df = df.copy()
    df["implementation_label"] = df["implementation"].map(IMPLEMENTATION_LABELS).fillna(df["implementation"])
    df["active_run"] = df["tx_pkts"].fillna(0) > 0

    df["instructions_per_pkt"] = np.where(df["tx_pkts"] > 0, df["perf_instructions"] / df["tx_pkts"], np.nan)
    df["cycles_per_pkt"] = np.where(df["tx_pkts"] > 0, df["perf_cycles"] / df["tx_pkts"], np.nan)
    df["branch_miss_rate"] = np.where(df["perf_branches"] > 0, df["perf_branch_misses"] / df["perf_branches"], np.nan)
    df["llc_load_misses_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["perf_llc_load_misses"] / df["tx_pkts"], np.nan)
    df["dtlb_load_misses_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["perf_dtlb_load_misses"] / df["tx_pkts"], np.nan)
    df["itlb_load_misses_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["perf_itlb_load_misses"] / df["tx_pkts"], np.nan)
    df["l1i_load_misses_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["perf_l1_icache_load_misses"] / df["tx_pkts"], np.nan)
    df["l1d_load_misses_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["perf_l1_dcache_load_misses"] / df["tx_pkts"], np.nan)
    df["doorbells_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["doorbells"] / df["tx_pkts"], np.nan)
    df["mmio_writes_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["total_mmio_writes"] / df["tx_pkts"], np.nan)
    df["reclaim_calls_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["reclaim_calls"] / df["tx_pkts"], np.nan)
    df["faults_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["perf_faults"] / df["tx_pkts"], np.nan)
    df["cs_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["perf_cs"] / df["tx_pkts"], np.nan)
    df["cpu_migrations_per_mpkt"] = np.where(df["tx_pkts"] > 0, 1e6 * df["perf_cpu_migrations"] / df["tx_pkts"], np.nan)
    df["stalled_frontend_per_pkt"] = np.where(df["tx_pkts"] > 0, df["perf_stalled_cycles_frontend"] / df["tx_pkts"], np.nan)
    df["stalled_backend_per_pkt"] = np.where(df["tx_pkts"] > 0, df["perf_stalled_cycles_backend"] / df["tx_pkts"], np.nan)
    df["steady_vs_final_tx_wire_delta_gbps"] = df["steady_tx_wire_gbps"] - df["final_tx_wire_gbps"]

    return df


def aggregate_batches(df: pd.DataFrame) -> pd.DataFrame:
    if df.empty:
        return df

    agg = (
        df.groupby(["implementation", "implementation_label", "batch_size"], as_index=False)
        .agg(
            runs=("run_id", "count"),
            active_runs=("active_run", "sum"),
            steady_tx_wire_gbps=("steady_tx_wire_gbps", "mean"),
            final_tx_wire_gbps=("final_tx_wire_gbps", "mean"),
            steady_tx_mpps=("steady_tx_mpps", "mean"),
            cycles_per_pkt=("cycles_per_pkt", "mean"),
            instructions_per_pkt=("instructions_per_pkt", "mean"),
            branch_miss_rate=("branch_miss_rate", "mean"),
            llc_load_misses_per_mpkt=("llc_load_misses_per_mpkt", "mean"),
            dtlb_load_misses_per_mpkt=("dtlb_load_misses_per_mpkt", "mean"),
            itlb_load_misses_per_mpkt=("itlb_load_misses_per_mpkt", "mean"),
            l1i_load_misses_per_mpkt=("l1i_load_misses_per_mpkt", "mean"),
            l1d_load_misses_per_mpkt=("l1d_load_misses_per_mpkt", "mean"),
            doorbells_per_mpkt=("doorbells_per_mpkt", "mean"),
            mmio_writes_per_mpkt=("mmio_writes_per_mpkt", "mean"),
            empty_poll_pct=("empty_poll_pct", "mean"),
            avg_pkts_per_doorbell=("avg_pkts_per_doorbell", "mean"),
            reclaim_calls_per_mpkt=("reclaim_calls_per_mpkt", "mean"),
            faults_per_mpkt=("faults_per_mpkt", "mean"),
            cs_per_mpkt=("cs_per_mpkt", "mean"),
            cpu_migrations_per_mpkt=("cpu_migrations_per_mpkt", "mean"),
            stalled_frontend_per_pkt=("stalled_frontend_per_pkt", "mean"),
            stalled_backend_per_pkt=("stalled_backend_per_pkt", "mean"),
            perf_instructions_running_pct=("perf_instructions_running_pct", "mean"),
            perf_cycles_running_pct=("perf_cycles_running_pct", "mean"),
            perf_llc_load_misses_running_pct=("perf_llc_load_misses_running_pct", "mean"),
            perf_dtlb_load_misses_running_pct=("perf_dtlb_load_misses_running_pct", "mean"),
            perf_branches_running_pct=("perf_branches_running_pct", "mean"),
        )
        .sort_values(["implementation", "batch_size"])
    )

    agg["active_rate"] = agg["active_runs"] / agg["runs"]
    return agg


def choose_focus_batch(agg: pd.DataFrame, metadata: dict[str, str]) -> int | None:
    if agg.empty:
        return None

    perf_record_batches = metadata.get("perf_record_batches", "")
    if perf_record_batches:
        first = perf_record_batches.split(",")[0].strip()
        if first.isdigit():
            batch = int(first)
            if batch in set(agg["batch_size"].dropna().astype(int)):
                return batch

    common_batches = (
        agg.groupby("batch_size")["implementation"].nunique().reset_index(name="implementations")
    )
    both = common_batches.loc[common_batches["implementations"] >= 2, "batch_size"]
    if not both.empty:
        return int(both.max())

    return int(agg["batch_size"].dropna().max())


def comparison_table_for_batch(agg: pd.DataFrame, focus_batch: int) -> pd.DataFrame:
    focus = agg[agg["batch_size"] == focus_batch].copy()
    if focus.empty:
        return focus

    cols = [
        "implementation_label",
        "cycles_per_pkt",
        "instructions_per_pkt",
        "llc_load_misses_per_mpkt",
        "dtlb_load_misses_per_mpkt",
        "itlb_load_misses_per_mpkt",
        "branch_miss_rate",
        "doorbells_per_mpkt",
        "mmio_writes_per_mpkt",
        "empty_poll_pct",
    ]
    return focus[cols].set_index("implementation_label")


def rust_vs_c_lines(agg: pd.DataFrame, focus_batch: int) -> list[str]:
    focus = agg[agg["batch_size"] == focus_batch]
    if set(focus["implementation"]) != {"c", "rust"}:
        return []

    c_row = focus.loc[focus["implementation"] == "c"].iloc[0]
    rust_row = focus.loc[focus["implementation"] == "rust"].iloc[0]
    lines: list[str] = []

    comparisons = [
        ("cycles_per_pkt", "cycles per packet", True),
        ("instructions_per_pkt", "instructions per packet", True),
        ("llc_load_misses_per_mpkt", "LLC load misses per Mpkt", True),
        ("dtlb_load_misses_per_mpkt", "dTLB load misses per Mpkt", True),
        ("itlb_load_misses_per_mpkt", "iTLB load misses per Mpkt", True),
        ("branch_miss_rate", "branch miss rate", True),
        ("doorbells_per_mpkt", "doorbells per Mpkt", True),
        ("mmio_writes_per_mpkt", "MMIO writes per Mpkt", True),
    ]

    for col, label, lower_is_better in comparisons:
        c_value = c_row.get(col)
        rust_value = rust_row.get(col)
        if pd.isna(c_value) or pd.isna(rust_value) or c_value == 0:
            continue

        ratio = rust_value / c_value
        if lower_is_better and ratio >= 1.05:
            lines.append(
                f"- Rust spent **{ratio:.2f}x** the {label} of C at batch **{focus_batch}** "
                f"({rust_value:.3f} vs {c_value:.3f})."
            )
        elif lower_is_better and ratio <= 0.95:
            lines.append(
                f"- Rust improved {label} to **{ratio:.2f}x** of C at batch **{focus_batch}** "
                f"({rust_value:.3f} vs {c_value:.3f})."
            )

    return lines


def low_running_pct_notes(agg: pd.DataFrame) -> list[str]:
    if agg.empty:
        return []

    notes: list[str] = []
    checks = [
        ("perf_instructions_running_pct", "instructions"),
        ("perf_cycles_running_pct", "cycles"),
        ("perf_llc_load_misses_running_pct", "LLC-load-misses"),
        ("perf_dtlb_load_misses_running_pct", "dTLB-load-misses"),
        ("perf_branches_running_pct", "branches"),
    ]

    for col, label in checks:
        if col not in agg:
            continue
        low = agg.loc[agg[col] < 95, ["implementation_label", "batch_size", col]]
        if low.empty:
            continue
        for _, row in low.iterrows():
            notes.append(
                f"- `{label}` ran at **{row[col]:.1f}%** on {row['implementation_label']} batch **{int(row['batch_size'])}**."
            )
    return notes


def hotspot_excerpt(report_path: Path, limit: int = 10) -> list[str]:
    if not report_path.exists():
        return []

    lines: list[str] = []
    for raw_line in report_path.read_text(errors="replace").splitlines():
        if HOTSPOT_LINE_RE.match(raw_line):
            lines.append(raw_line.rstrip())
        if len(lines) >= limit:
            break
    return lines


def _plot_packet_costs(agg: pd.DataFrame) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.5))

    for impl, group in agg.groupby("implementation"):
        color = IMPLEMENTATION_COLORS.get(impl, None)
        label = IMPLEMENTATION_LABELS.get(impl, impl)
        axes[0].plot(group["batch_size"], group["cycles_per_pkt"], marker="o", linewidth=2, color=color, label=label)
        axes[1].plot(group["batch_size"], group["instructions_per_pkt"], marker="o", linewidth=2, color=color, label=label)

    axes[0].set_title("Cycles Per Packet")
    axes[1].set_title("Instructions Per Packet")
    for ax in axes:
        ax.set_xscale("log", base=2)
        ax.set_xticks(sorted(agg["batch_size"].dropna().unique()))
        ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
        ax.set_xlabel("Reflect batch")
        ax.grid(True, alpha=0.3)
        ax.legend()
    axes[0].set_ylabel("cycles / pkt")
    axes[1].set_ylabel("instructions / pkt")
    plt.tight_layout()
    plt.show()


def _plot_memory_pressure(agg: pd.DataFrame) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))
    metrics = [
        ("llc_load_misses_per_mpkt", "LLC Misses / Mpkt"),
        ("dtlb_load_misses_per_mpkt", "dTLB Misses / Mpkt"),
        ("itlb_load_misses_per_mpkt", "iTLB Misses / Mpkt"),
    ]

    for ax, (metric, title) in zip(axes, metrics):
        for impl, group in agg.groupby("implementation"):
            color = IMPLEMENTATION_COLORS.get(impl, None)
            label = IMPLEMENTATION_LABELS.get(impl, impl)
            ax.plot(group["batch_size"], group[metric], marker="o", linewidth=2, color=color, label=label)
        ax.set_title(title)
        ax.set_xscale("log", base=2)
        ax.set_xticks(sorted(agg["batch_size"].dropna().unique()))
        ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
        ax.set_xlabel("Reflect batch")
        ax.grid(True, alpha=0.3)
        ax.legend()

    axes[0].set_ylabel("misses / Mpkt")
    plt.tight_layout()
    plt.show()


def _plot_batching_metrics(agg: pd.DataFrame) -> None:
    metric_cols = ["doorbells_per_mpkt", "mmio_writes_per_mpkt", "empty_poll_pct"]
    if all(agg[col].isna().all() for col in metric_cols if col in agg.columns):
        return

    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))
    titles = [
        ("doorbells_per_mpkt", "Doorbells / Mpkt"),
        ("mmio_writes_per_mpkt", "MMIO Writes / Mpkt"),
        ("empty_poll_pct", "Empty Poll %"),
    ]

    for ax, (metric, title) in zip(axes, titles):
        for impl, group in agg.groupby("implementation"):
            if metric not in group:
                continue
            color = IMPLEMENTATION_COLORS.get(impl, None)
            label = IMPLEMENTATION_LABELS.get(impl, impl)
            ax.plot(group["batch_size"], group[metric], marker="o", linewidth=2, color=color, label=label)
        ax.set_title(title)
        ax.set_xscale("log", base=2)
        ax.set_xticks(sorted(agg["batch_size"].dropna().unique()))
        ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
        ax.set_xlabel("Reflect batch")
        ax.grid(True, alpha=0.3)
        ax.legend()

    axes[0].set_ylabel("cost")
    plt.tight_layout()
    plt.show()


def _plot_ratio_heatmap(agg: pd.DataFrame) -> None:
    focus = agg[agg["implementation"].isin(["c", "rust"])].copy()
    if focus["implementation"].nunique() < 2:
        return

    common = focus.groupby("batch_size")["implementation"].nunique()
    common_batches = common[common >= 2].index.tolist()
    if not common_batches:
        return

    pivot = focus.pivot(index="batch_size", columns="implementation")
    values = []
    labels = []
    for metric, label in RATIO_METRICS:
        if metric not in focus.columns:
            continue
        series_c = pivot[(metric, "c")] if (metric, "c") in pivot else None
        series_r = pivot[(metric, "rust")] if (metric, "rust") in pivot else None
        if series_c is None or series_r is None:
            continue
        ratio = series_r.loc[common_batches] / series_c.loc[common_batches]
        values.append(ratio.to_numpy())
        labels.append(label)

    if not values:
        return

    heat = np.vstack(values)
    fig, ax = plt.subplots(figsize=(1.4 * len(common_batches) + 2, 0.6 * len(labels) + 2))
    im = ax.imshow(heat, aspect="auto", cmap="coolwarm", vmin=0.8, vmax=1.2)
    ax.set_xticks(np.arange(len(common_batches)))
    ax.set_xticklabels(common_batches)
    ax.set_yticks(np.arange(len(labels)))
    ax.set_yticklabels(labels)
    ax.set_xlabel("Reflect batch")
    ax.set_title("Rust / C Cost Ratio (1.0 = parity)")

    for i in range(heat.shape[0]):
        for j in range(heat.shape[1]):
            value = heat[i, j]
            if math.isnan(value):
                text = "n/a"
            else:
                text = f"{value:.2f}"
            ax.text(j, i, text, ha="center", va="center", color="black", fontsize=9)

    fig.colorbar(im, ax=ax, shrink=0.8)
    plt.tight_layout()
    plt.show()


def _perf_record_markdown(metadata: dict[str, str], focus_batch: int | None) -> str:
    bdf = metadata.get("bdf", "<BDF>")
    hugepage_dir = metadata.get("hugepage_dir", "/mnt/huge")
    record_duration = metadata.get("perf_record_duration_seconds", "30")
    batch = focus_batch if focus_batch is not None else metadata.get("perf_record_batches", "64").split(",")[0].strip()
    c_binary = metadata.get("c_binary", "/users/manvik12/my_ice/my_ice")
    rust_binary = metadata.get("rust_binary", "/users/manvik12/my_ice_rust/target/release/my_ice_rust")

    return (
        "## perf stat vs perf record\n"
        "This notebook works from `perf stat`, so it explains **what kind of work** dominated the run. "
        "To learn **which functions** dominated the run, capture samples with something like:\n\n"
        "```bash\n"
        f"sudo perf record -F 999 -g -- {c_binary} {bdf} --rx-reflect {record_duration} --reflect-batch {batch} --hugepages --hugepage-dir {hugepage_dir}\n"
        "sudo perf report\n"
        "```\n\n"
        "For the Rust implementation:\n\n"
        "```bash\n"
        f"sudo perf record -F 999 -g -- {rust_binary} {bdf} --rx-reflect {record_duration} --reflect-batch {batch} --hugepages --hugepage-dir {hugepage_dir}\n"
        "sudo perf report\n"
        "```"
    )


def render_report(run_dir: Path) -> None:
    summary_csv = run_dir / "summary.csv"
    raw_perf_csv = run_dir / "raw_perf.csv"
    metadata_file = run_dir / "metadata.txt"
    perf_record_index_csv = run_dir / "perf_record_index.csv"

    metadata = load_metadata(metadata_file)
    summary_df = _coerce_numeric(_read_csv(summary_csv))
    raw_perf_df = _read_csv(raw_perf_csv)
    perf_record_df = _read_csv(perf_record_index_csv)
    summary_df = derive_metrics(summary_df)
    agg = aggregate_batches(summary_df)
    focus_batch = choose_focus_batch(agg, metadata)

    display(Markdown("## Run Metadata"))
    display(pd.Series(metadata, name="value").to_frame())

    display(Markdown(_perf_record_markdown(metadata, focus_batch)))

    if summary_df.empty:
        display(Markdown("No summary rows were found."))
        return

    display(Markdown("## Per-Run Overview"))
    overview_cols = [
        "run_id",
        "implementation_label",
        "batch_size",
        "status",
        "steady_tx_wire_gbps",
        "cycles_per_pkt",
        "instructions_per_pkt",
        "llc_load_misses_per_mpkt",
        "dtlb_load_misses_per_mpkt",
        "doorbells_per_mpkt",
    ]
    display(
        summary_df[overview_cols].style.format(
            {
                "steady_tx_wire_gbps": "{:.3f}",
                "cycles_per_pkt": "{:.2f}",
                "instructions_per_pkt": "{:.2f}",
                "llc_load_misses_per_mpkt": "{:.2f}",
                "dtlb_load_misses_per_mpkt": "{:.2f}",
                "doorbells_per_mpkt": "{:.2f}",
            }
        )
    )

    display(Markdown("## Batch-Level Perf Summary"))
    summary_cols = [
        "implementation_label",
        "batch_size",
        "active_rate",
        "cycles_per_pkt",
        "instructions_per_pkt",
        "llc_load_misses_per_mpkt",
        "dtlb_load_misses_per_mpkt",
        "itlb_load_misses_per_mpkt",
        "doorbells_per_mpkt",
        "mmio_writes_per_mpkt",
        "perf_instructions_running_pct",
        "perf_cycles_running_pct",
    ]
    display(
        agg[summary_cols].style.format(
            {
                "active_rate": "{:.0%}",
                "cycles_per_pkt": "{:.2f}",
                "instructions_per_pkt": "{:.2f}",
                "llc_load_misses_per_mpkt": "{:.2f}",
                "dtlb_load_misses_per_mpkt": "{:.2f}",
                "itlb_load_misses_per_mpkt": "{:.2f}",
                "doorbells_per_mpkt": "{:.2f}",
                "mmio_writes_per_mpkt": "{:.2f}",
                "perf_instructions_running_pct": "{:.1f}",
                "perf_cycles_running_pct": "{:.1f}",
            }
        )
    )

    if focus_batch is not None:
        display(Markdown(f"## Focus Batch: {focus_batch}"))
        focus_table = comparison_table_for_batch(agg, focus_batch)
        if not focus_table.empty:
            display(
                focus_table.style.format(
                    {
                        "cycles_per_pkt": "{:.2f}",
                        "instructions_per_pkt": "{:.2f}",
                        "llc_load_misses_per_mpkt": "{:.2f}",
                        "dtlb_load_misses_per_mpkt": "{:.2f}",
                        "itlb_load_misses_per_mpkt": "{:.2f}",
                        "branch_miss_rate": "{:.6f}",
                        "doorbells_per_mpkt": "{:.2f}",
                        "mmio_writes_per_mpkt": "{:.2f}",
                        "empty_poll_pct": "{:.2f}",
                    }
                )
            )

    pain_points = rust_vs_c_lines(agg, focus_batch) if focus_batch is not None else []
    if pain_points:
        display(Markdown("## Pain Points\n" + "\n".join(pain_points)))

    running_notes = low_running_pct_notes(agg)
    if running_notes:
        display(
            Markdown(
                "## Confidence Notes\n"
                "Interpret events with low `*_running_pct` carefully: those counters may have been multiplexed by `perf stat`.\n"
                + "\n".join(running_notes[:12])
            )
        )

    display(Markdown("## Packet Cost Plots"))
    _plot_packet_costs(agg)

    display(Markdown("## Memory and TLB Pressure"))
    _plot_memory_pressure(agg)

    display(Markdown("## Batching and MMIO Side Effects"))
    _plot_batching_metrics(agg)

    display(Markdown("## Rust vs C Ratio Heatmap"))
    _plot_ratio_heatmap(agg)

    if not perf_record_df.empty:
        display(Markdown("## Hotspot Samples"))
        perf_record_df = perf_record_df.copy()
        perf_record_df["implementation_label"] = perf_record_df["implementation"].map(IMPLEMENTATION_LABELS).fillna(perf_record_df["implementation"])
        display(perf_record_df[["implementation_label", "batch_size", "status", "perf_report"]])

        for _, row in perf_record_df.iterrows():
            report_path = run_dir / str(row["perf_report"])
            lines = hotspot_excerpt(report_path)
            if not lines:
                continue
            heading = f"### {row['implementation_label']} batch {int(row['batch_size'])}"
            body = "\n".join(lines)
            display(Markdown(f"{heading}\n\n```text\n{body}\n```"))

    if not raw_perf_df.empty:
        display(Markdown("## Raw perf stat Event Count"))
        display(pd.DataFrame({"rows": [len(raw_perf_df)]}, index=["raw_perf.csv"]))

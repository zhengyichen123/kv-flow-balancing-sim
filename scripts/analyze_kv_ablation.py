#!/usr/bin/env python3
"""KV 选路实验的请求级分析器。

数据来源：
  astra-sim/inputs/runs/<batch>/<run>/ns3/output/kv_fct.csv
  outputs/<batch>/<run>.csv

每个运行输出 FCT 均值/P50/P90/P95/P99/最大值、平均 slowdown，
以及 mean TTFT、到达至 KV 开始的均值/P90、端到端平均延迟。延迟单位均为 ms。
FCT 使用单请求整份 KV 的 fct_ns；TTFT 直接使用请求 CSV 的 TTFT。
slowdown = fct_ns / standalone_fct_ns；非正基线不参与均值。
prefill_mean_ms/prefill_p90_ms 表示到达至 KV 开始的时间，包含排队、计算和调度。
分位数采用线性插值，位置为 (样本数 - 1) × 分位比例。
缺失或负数 TTFT 不参与均值。
CSV 每个运行一行；不同批次独立统计，不合并同一 SPS 的结果。

用法：
  python3 scripts/analyze_kv_ablation.py
  python3 scripts/analyze_kv_ablation.py --batch kv261107_0612_s20_nall
  python3 scripts/analyze_kv_ablation.py --scenario symmetric --csv kv_report.csv
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import re
import statistics
import sys

DEFAULT_RUNS_ROOT = "astra-sim/inputs/runs"
DEFAULT_OUTPUTS_ROOT = "outputs"
SCENARIO_LABELS = {"sym": "symmetric", "loc25": "local25g", "ds25": "downstream25g"}
BATCH_RE = re.compile(r"^kv(?P<stamp>\d{6}_\d{4})_s(?P<sps>\d+)_n(?P<n>.+)$")
CSV_FIELDS = [
    "batch", "sps", "scenario", "policy", "fct_mean_ms", "fct_p50_ms",
    "fct_p90_ms", "fct_p95_ms", "fct_p99_ms", "fct_max_ms", "slowdown",
    "ttft_mean_ms", "prefill_mean_ms", "prefill_p90_ms", "latency_mean_ms",
]


def percentile(values, q):
    """与常见统计工具的默认线性插值分位数口径一致。"""
    if not values:
        return float("nan")
    xs = sorted(values)
    position = (len(xs) - 1) * q
    lower = math.floor(position)
    upper = math.ceil(position)
    return xs[lower] + (xs[upper] - xs[lower]) * (position - lower)


def request_stats(run_dir, output_csv):
    with open(os.path.join(run_dir, "kv_fct.csv")) as f:
        kv = list(csv.DictReader(f))
    if not kv:
        raise ValueError("kv_fct.csv 没有已完成请求")
    request_ids = {int(r["request_id"]) for r in kv}
    if len(request_ids) != len(kv):
        raise ValueError("kv_fct.csv 存在重复 request_id，无法按单请求整份 KV 统计")
    fct = [int(r["fct_ns"]) / 1e6 for r in kv]
    slow = [
        int(r["fct_ns"]) / int(r["standalone_fct_ns"])
        for r in kv if int(r["standalone_fct_ns"]) > 0
    ]
    starts = {int(r["request_id"]): int(r["start_ns"]) for r in kv}
    ttft = []
    prefill = []
    latency = []
    if os.path.exists(output_csv):
        with open(output_csv) as f:
            seen = set()
            for r in csv.DictReader(f):
                request_id = int(r["request id"])
                if request_id not in request_ids:
                    continue
                if request_id in seen:
                    raise ValueError("请求 CSV 存在重复 request id")
                seen.add(request_id)
                for column, values in (("TTFT", ttft), ("latency", latency)):
                    raw = r.get(column, "")
                    if raw is None or not raw.strip():
                        continue
                    value = float(raw)
                    if math.isfinite(value) and value >= 0:
                        values.append(value / 1e6)
                arrival = r.get("arrival", "")
                if arrival is not None and arrival.strip() and int(arrival) >= 0:
                    delay = starts[request_id] - int(arrival)
                    if delay >= 0:
                        prefill.append(delay / 1e6)
    return {
        "fct_mean_ms": statistics.mean(fct),
        "fct_p50_ms": percentile(fct, 0.50),
        "fct_p90_ms": percentile(fct, 0.90),
        "fct_p95_ms": percentile(fct, 0.95),
        "fct_p99_ms": percentile(fct, 0.99),
        "fct_max_ms": max(fct),
        "slowdown": statistics.mean(slow) if slow else float("nan"),
        "ttft_mean_ms": statistics.mean(ttft) if ttft else float("nan"),
        "prefill_mean_ms": statistics.mean(prefill) if prefill else float("nan"),
        "prefill_p90_ms": percentile(prefill, 0.90),
        "latency_mean_ms": statistics.mean(latency) if latency else float("nan"),
    }


def parse_run_name(name):
    for code, label in SCENARIO_LABELS.items():
        prefix = code + "_"
        if name.startswith(prefix):
            return label, name[len(prefix):]
    return None, None


def discover_batches(runs_root):
    out = []
    for name in os.listdir(runs_root):
        match = BATCH_RE.match(name)
        if match and os.path.isdir(os.path.join(runs_root, name)):
            out.append((match.group("stamp"), int(match.group("sps")), name))
    return [name for _, _, name in sorted(out)]


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--runs-root", default=DEFAULT_RUNS_ROOT)
    ap.add_argument("--outputs-root", default=DEFAULT_OUTPUTS_ROOT)
    ap.add_argument("--batch", action="append", help="只分析指定批次，可重复")
    ap.add_argument("--scenario", choices=tuple(SCENARIO_LABELS.values()), help="筛选场景")
    ap.add_argument("--policy", help="筛选策略，如 BRBF_V1")
    ap.add_argument("--csv", help="导出请求级汇总 CSV，每个运行一行")
    args = ap.parse_args(argv)
    batches = args.batch or discover_batches(args.runs_root)
    records = []
    for batch in dict.fromkeys(batches):
        batch_dir = os.path.join(args.runs_root, batch)
        if not os.path.isdir(batch_dir):
            print(f"跳过不存在的批次：{batch}", file=sys.stderr)
            continue
        match = BATCH_RE.match(batch)
        sps = int(match.group("sps")) if match else ""
        for run in sorted(os.listdir(batch_dir)):
            scenario, policy = parse_run_name(run)
            if scenario is None:
                continue
            if args.scenario and scenario != args.scenario:
                continue
            if args.policy and policy != args.policy:
                continue
            run_dir = os.path.join(batch_dir, run, "ns3", "output")
            if not os.path.exists(os.path.join(run_dir, "kv_fct.csv")):
                continue
            try:
                stats = request_stats(
                    run_dir, os.path.join(args.outputs_root, batch, run + ".csv"),
                )
            except ValueError as exc:
                print(f"跳过 {batch}/{run}：{exc}", file=sys.stderr)
                continue
            records.append({
                "batch": batch, "sps": sps, "scenario": scenario, "policy": policy, **stats,
            })
    if not records:
        print("没有匹配的已完成请求数据", file=sys.stderr)
        return 1

    print("请求级指标：单请求整份 KV；延迟单位 ms")
    groups = [
        ("请求 KV 网络指标", [
            ("fct_mean_ms", "mean FCT"), ("fct_p50_ms", "P50 FCT"),
            ("fct_p90_ms", "P90 FCT"), ("fct_p95_ms", "P95 FCT"),
            ("fct_p99_ms", "P99 FCT"), ("fct_max_ms", "max FCT"),
            ("slowdown", "slowdown"),
        ]),
        ("请求时间指标（到达→KV 开始包含排队、计算和调度）", [
            ("ttft_mean_ms", "mean TTFT"),
            ("prefill_mean_ms", "到达→KV均值"), ("prefill_p90_ms", "到达→KV P90"),
            ("latency_mean_ms", "端到端均值"),
        ]),
    ]
    for batch in dict.fromkeys(r["batch"] for r in records):
        print(f"\n[{batch}]")
        batch_records = [r for r in records if r["batch"] == batch]
        for title, columns in groups:
            print(f"\n{title}")
            print(
                f"{'SPS':>4} {'场景':<14} {'策略':<13} "
                + " ".join(f"{label:>12}" for _, label in columns)
            )
            for r in batch_records:
                cells = [f"{r[key]:>12.3f}" for key, _ in columns]
                print(
                    f"{r['sps']:>4} {r['scenario']:<14} {r['policy']:<13} "
                    + " ".join(cells)
                )
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
            writer.writeheader()
            for r in records:
                writer.writerow({
                    key: f"{value:.6f}" if isinstance(value, float) else value
                    for key, value in r.items()
                })
        print(f"\n请求级汇总已写入 {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

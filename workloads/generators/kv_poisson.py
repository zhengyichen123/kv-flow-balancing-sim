"""KV-elephant workload generator: Zipf prompt lengths + Poisson arrivals.

Built for the switch-level KV-routing study, and parameterised to mirror the
ARK evaluation (ASPLOS'25, "Avoiding Routing Collisions for KV Cache Transfer
in Disaggregated LLM Inference"):

* prompt length ~ bounded Zipf(--zipf-alpha) over
  [--min-input-toks, --max-input-toks] -- heavy-tailed, so a run mixes a few
  very long prompts (the elephant flows) with many short ones;
* request arrivals ~ Poisson process at --sps requests/sec;
* one request == one prefill->decode KV transfer, whose size is
  input_toks * per_token_kv_bytes.

Emits the same flat JSONL the simulator consumes:

    {"input_toks", "output_toks", "arrival_time_ns",
     ["input_tok_ids", "output_tok_ids"]}

Token-id lists are omitted by default. They are only read when prefix
caching is on, and leaving them out guarantees zero prefix-cache hits -- i.e.
every request transfers its full prompt KV, which is what a KV-routing
experiment wants. Pass --emit-token-ids to include them.

Stdlib only (no numpy), so it runs anywhere.

Invoke via: python -m workloads.generators kv_poisson --help
"""

from __future__ import annotations

import argparse
import bisect
import json
import random
from pathlib import Path


def register_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--num-reqs", type=int, required=True, dest="num_reqs",
                   help="Number of requests to emit.")
    p.add_argument("--sps", type=float, required=True,
                   help="Arrival rate (requests / sec). Poisson-distributed.")
    p.add_argument("--output", required=True,
                   help="Output JSONL path.")
    p.add_argument("--seed", type=int, default=42,
                   help="RNG seed for length sampling and arrivals.")
    p.add_argument("--zipf-alpha", type=float, default=0.7, dest="zipf_alpha",
                   help="Exponent of the bounded Zipf prompt-length "
                        "distribution. Default 0.7 (ARK).")
    p.add_argument("--min-input-toks", type=int, default=1024,
                   dest="min_input_toks",
                   help="Shortest prompt length. Default 1024.")
    p.add_argument("--max-input-toks", type=int, default=53248,
                   dest="max_input_toks",
                   help="Longest prompt length. Default 53248.")
    p.add_argument("--output-toks", type=int, default=128,
                   dest="output_toks",
                   help="Decode length for every request. Default 128.")
    p.add_argument("--first-arrival-sec", type=int, default=0,
                   dest="first_arrival_sec",
                   help="Arrival time (seconds) of the first request. Default 0.")
    p.add_argument("--emit-token-ids", action="store_true", default=False,
                   dest="emit_token_ids",
                   help="Also write input_tok_ids / output_tok_ids (needed "
                        "only when prefix caching should be able to hit). "
                        "Off by default to keep traces small.")


def _build_zipf_cdf(lo: int, hi: int, alpha: float):
    """Cumulative weights of a discrete Zipf(k^-alpha) over [lo, hi]."""
    cum = []
    total = 0.0
    for k in range(lo, hi + 1):
        total += float(k) ** (-alpha)
        cum.append(total)
    return cum, total


def _sample_length(lo: int, cum, total: float, rng: random.Random) -> int:
    r = rng.random() * total
    return lo + bisect.bisect_left(cum, r)


def run(args) -> int:
    if args.sps <= 0:
        raise ValueError("--sps must be > 0")
    if args.max_input_toks < args.min_input_toks:
        raise ValueError("--max-input-toks must be >= --min-input-toks")

    rng = random.Random(args.seed)
    cum, total = _build_zipf_cdf(args.min_input_toks, args.max_input_toks,
                                 args.zipf_alpha)
    lengths = [_sample_length(args.min_input_toks, cum, total, rng)
               for _ in range(args.num_reqs)]

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # Poisson arrivals: exponential inter-arrival at rate `sps`. The first
    # request sits at first_arrival_sec; each subsequent one is a gap later.
    time_ns = int(args.first_arrival_sec) * 1_000_000_000
    with out_path.open("w", encoding="utf-8") as fout:
        for i, length in enumerate(lengths):
            if i > 0:
                time_ns += int(rng.expovariate(args.sps) * 1e9)
            row = {
                "input_toks": int(length),
                "output_toks": int(args.output_toks),
                "arrival_time_ns": int(time_ns),
            }
            if args.emit_token_ids:
                row["input_tok_ids"] = list(range(1, length + 1))
                row["output_tok_ids"] = list(range(1, args.output_toks + 1))
            fout.write(json.dumps(row, ensure_ascii=False) + "\n")

    span_s = time_ns / 1e9 - args.first_arrival_sec
    mean = sum(lengths) / len(lengths)
    rate = args.num_reqs / span_s if span_s > 0 else 0.0
    print(f"Wrote {args.num_reqs} requests -> {out_path}")
    print(f"  prompt toks: min={min(lengths)} mean={mean:.0f} "
          f"max={max(lengths)}")
    print(f"  arrival span: {span_s:.2f} s (effective rate {rate:.1f} req/s)")
    return 0

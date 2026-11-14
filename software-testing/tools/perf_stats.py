#!/usr/bin/env python3
"""Performance testing with statistics for miniLB.

One benchmark run is one number, and on a shared machine the next run can be
10% or more away from it. A single run therefore cannot show that a
requirement is met, or that one configuration is faster than another. This
tool repeats the existing benchmark (benchmarks/run_benchmark.sh, which uses
wrk) N times and reports, per scenario:

    median and mean requests/s, median p99 latency
    coefficient of variation   (how noisy the measurement is)
    95% bootstrap confidence interval for the median

and for two questions the README answers with single numbers:

    scaling   "three backends give about three times the throughput of one"
              (work-3-backends against work-1-backend)
    cache     "with the cache on, hits are faster than proxying"
              (lb-cache against lb-3-backends)

    ratio of medians with a 95% bootstrap confidence interval, and an exact
    Mann-Whitney U test (no assumption that the numbers are normally
    distributed, which throughput samples usually are not).

Targets turn the report into a test. They are requirement Q3, which the
README does not state, so they have no default: choose them and say why.

    --min-scaling 2.5     the lower end of the scaling interval must reach 2.5
    --max-p99-ms 40       the upper end of the p99 interval of lb-3-backends
                          must stay below 40 ms

With a target the exit status is 1 when it is missed.

Usage:
    software-testing/tools/perf_stats.py --runs 8 --duration 5s \\
        --report software-testing/docs/06-performance-report.md

Needs: wrk, redis-server, a Release build in ./build (scripts/build.sh).
Only the Python standard library is used.
"""

import argparse
import itertools
import os
import pathlib
import platform
import random
import re
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCENARIOS = ["direct", "lb-1-backend", "lb-3-backends", "work-1-backend", "work-3-backends", "lb-cache"]


def to_ms(text):
    """wrk prints latencies as 850.00us, 12.34ms or 1.20s."""
    match = re.fullmatch(r"([0-9.]+)(us|ms|s)", text)
    if not match:
        return float("nan")
    value = float(match.group(1))
    return {"us": value / 1000.0, "ms": value, "s": value * 1000.0}[match.group(2)]


def run_once(args):
    """One run of the benchmark script. Returns {scenario: (requests/s, p99 ms, non-2xx)}."""
    env = dict(os.environ, DURATION=args.duration, CONNECTIONS=str(args.connections),
               WRK_THREADS=str(args.wrk_threads), BUILD_DIR=args.build_dir)
    # Run through bash so that it also works when the script's executable
    # bit was lost (for example after a download as a zip file).
    result = subprocess.run(["bash", "benchmarks/run_benchmark.sh"], cwd=ROOT, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        sys.exit("benchmarks/run_benchmark.sh failed:\n" + result.stdout[-2000:])
    rows = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 5 and fields[0] in SCENARIOS:
            try:
                rows[fields[0]] = (float(fields[1]), to_ms(fields[3]), int(fields[4]))
            except ValueError:
                sys.exit(f"could not read the benchmark line: {line}")
    missing = [s for s in SCENARIOS if s not in rows]
    if missing:
        sys.exit(f"the benchmark did not report {missing}:\n" + result.stdout[-2000:])
    return rows


# ------------------------------- statistics ----------------------------------

def bootstrap_ci(sample, statistic, rng, resamples=10000):
    """95% percentile bootstrap interval for statistic(sample)."""
    values = sorted(statistic([rng.choice(sample) for _ in sample]) for _ in range(resamples))
    return values[int(0.025 * resamples)], values[int(0.975 * resamples) - 1]


def ratio_ci(numerator, denominator, rng, resamples=10000):
    """95% bootstrap interval for median(numerator) / median(denominator)."""
    values = []
    for _ in range(resamples):
        top = statistics.median([rng.choice(numerator) for _ in numerator])
        bottom = statistics.median([rng.choice(denominator) for _ in denominator])
        values.append(top / bottom)
    values.sort()
    return values[int(0.025 * resamples)], values[int(0.975 * resamples) - 1]


def mann_whitney_exact(a, b):
    """Exact two-sided Mann-Whitney U test. Returns (U of a, p-value).

    U counts the pairs (x from a, y from b) with x > y. Under the null
    hypothesis that both samples come from the same distribution, every way
    of choosing which len(a) of the pooled values are called "a" is equally
    likely, so the p-value is the share of those choices whose U is at least
    as far from its expected value as the observed one. Exact for the small
    samples used here (16 choose 8 = 12,870 arrangements).
    """
    def u_statistic(first, second):
        return sum((x > y) + 0.5 * (x == y) for x in first for y in second)

    observed = u_statistic(a, b)
    expected = len(a) * len(b) / 2.0
    pooled = a + b
    indices = range(len(pooled))
    extreme = total = 0
    for chosen in itertools.combinations(indices, len(a)):
        picked = set(chosen)
        first = [pooled[i] for i in chosen]
        second = [pooled[i] for i in indices if i not in picked]
        total += 1
        if abs(u_statistic(first, second) - expected) >= abs(observed - expected) - 1e-9:
            extreme += 1
    return observed, extreme / total


# --------------------------------- report ------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=int, default=8)
    parser.add_argument("--duration", default="5s", help="wrk duration per scenario and run")
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--wrk-threads", type=int, default=2)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--seed", type=int, default=1, help="seed of the bootstrap resampling")
    parser.add_argument("--min-scaling", type=float, help="target: lower CI bound of work-3 / work-1")
    parser.add_argument("--max-p99-ms", type=float, help="target: upper CI bound of p99 for lb-3-backends")
    parser.add_argument("--report", help="write a Markdown report to this path (relative to the repository)")
    args = parser.parse_args()
    if args.runs < 4:
        sys.exit("use at least 4 runs; below that the intervals mean nothing")

    runs = []
    for index in range(args.runs):
        runs.append(run_once(args))
        print(f"run {index + 1}/{args.runs}: " +
              "  ".join(f"{s}={runs[-1][s][0]:.0f}/s" for s in SCENARIOS), flush=True)

    rng = random.Random(args.seed)
    throughput = {s: [run[s][0] for run in runs] for s in SCENARIOS}
    p99 = {s: [run[s][1] for run in runs] for s in SCENARIOS}
    errors = {s: sum(run[s][2] for run in runs) for s in SCENARIOS}

    out = ["# Performance report\n"]
    out.append(f"Generated by `software-testing/tools/perf_stats.py --runs {args.runs} --duration {args.duration}"
               f" --connections {args.connections} --wrk-threads {args.wrk_threads}`.\n")
    out.append(f"Machine: {os.cpu_count()} CPU(s), {platform.system()} {platform.release()}, {platform.machine()}."
               " wrk, miniLB, the backends and Redis all share these CPUs, so the absolute numbers"
               " describe this machine only. The comparisons and the size of the noise are the point.\n")

    out.append("## Per scenario\n")
    out.append("| Scenario | Median req/s | 95% CI of the median | Mean | Variation (CV) | Median p99 (ms) | 95% CI of p99 | Non-2xx |")
    out.append("|---|---:|---|---:|---:|---:|---|---:|")
    p99_ci = {}
    for s in SCENARIOS:
        low, high = bootstrap_ci(throughput[s], statistics.median, rng)
        p99_ci[s] = bootstrap_ci(p99[s], statistics.median, rng)
        cv = 100.0 * statistics.stdev(throughput[s]) / statistics.mean(throughput[s])
        out.append(f"| `{s}` | {statistics.median(throughput[s]):,.0f} | {low:,.0f} to {high:,.0f} |"
                   f" {statistics.mean(throughput[s]):,.0f} | {cv:.1f}% |"
                   f" {statistics.median(p99[s]):.2f} | {p99_ci[s][0]:.2f} to {p99_ci[s][1]:.2f} | {errors[s]} |")
    out.append("")

    out.append("## Comparisons\n")
    out.append("| Question | Ratio of medians | 95% CI of the ratio | Mann-Whitney U | p (exact, two-sided) |")
    out.append("|---|---:|---|---:|---:|")
    comparisons = {}
    for label, top, bottom in (("Scaling: 3 backends against 1 on `/work`", "work-3-backends", "work-1-backend"),
                               ("Cache: cache on against cache off on `/`", "lb-cache", "lb-3-backends")):
        ratio = statistics.median(throughput[top]) / statistics.median(throughput[bottom])
        low, high = ratio_ci(throughput[top], throughput[bottom], rng)
        u, p = mann_whitney_exact(throughput[top], throughput[bottom])
        comparisons[top] = (ratio, low, high, p)
        out.append(f"| {label} | {ratio:.2f} | {low:.2f} to {high:.2f} | {u:.0f} of {args.runs * args.runs} | {p:.4f} |")
    out.append("")
    out.append(f"With {args.runs} runs per side the smallest possible two-sided p-value is"
               f" {2.0 / sum(1 for _ in itertools.combinations(range(2 * args.runs), args.runs)):.5f}"
               " (every value of one side above every value of the other).\n")

    failed = False
    out.append("## Targets\n")
    if args.min_scaling is None and args.max_p99_ms is None:
        out.append("No targets were given (`--min-scaling`, `--max-p99-ms`), so this run reports and does not judge."
                   " Requirement Q3 needs a target and a reason for it.\n")
    else:
        out.append("| Target | Measured (bound of the 95% CI) | Result |")
        out.append("|---|---|---|")
        if args.min_scaling is not None:
            low = comparisons["work-3-backends"][1]
            ok = low >= args.min_scaling
            failed |= not ok
            out.append(f"| scaling at least {args.min_scaling} | lower bound {low:.2f} | {'met' if ok else 'MISSED'} |")
        if args.max_p99_ms is not None:
            high = p99_ci["lb-3-backends"][1]
            ok = high <= args.max_p99_ms
            failed |= not ok
            out.append(f"| p99 of `lb-3-backends` at most {args.max_p99_ms} ms | upper bound {high:.2f} ms | {'met' if ok else 'MISSED'} |")
        out.append("")

    out.append("## Raw data (requests/s per run)\n")
    out.append("| Run | " + " | ".join(f"`{s}`" for s in SCENARIOS) + " |")
    out.append("|---|" + "---:|" * len(SCENARIOS))
    for index, run in enumerate(runs, start=1):
        out.append(f"| {index} | " + " | ".join(f"{run[s][0]:,.0f}" for s in SCENARIOS) + " |")
    out.append("")

    report = "\n".join(out)
    if args.report:
        (ROOT / args.report).write_text(report)
        print(f"report written to {args.report}")
    else:
        print(report)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Mutation testing for miniLB, and a seeded-fault check for the CI pipeline.

Mutation testing asks a different question from coverage. Coverage says which
code the tests *ran*; mutation testing says whether the tests would *notice*
if that code were wrong. The tool makes one small change at a time to a source
file (a "mutant"), rebuilds, and runs the tests:

    killed    a test failed (or hung): the suite detects this fault
    survived  every test still passed: either a test is missing, or the mutant
              does not change behaviour ("equivalent"); a person has to decide
    invalid   the mutant did not compile; it is not counted

    mutation score = killed / (killed + survived)

Tests are run in two stages to save time: the unit tests first (fast), and
only if they all pass, the integration tests (start real processes).

Two modes:

    mutation.py run     [--max N] [--seed S] [--files F ...] [--report FILE]
        Generate all possible mutants of the target files, pick N of them at
        random (seeded, so a run can be repeated exactly) and report the score
        and every survivor.

    mutation.py seeded
        Apply a fixed list of hand-picked faults, one per requirement, and
        require the suite to kill every one of them. Exit status 1 if a fault
        gets through. CI runs this to show that the pipeline really does go
        red when the code is broken.

Mutation operators (each replaces one occurrence on one line):

    ROR  relational operator    <  <=  >  >=  ==  !=   (swapped pairwise)
    LCR  logical connector      &&  <->  ||
    LIT  boolean literal        true  <->  false
    AOR  arithmetic operator    +  <->  -
    NEG  negation removed       (!x  ->  (x

Needs: cmake, a C++ compiler and the miniLB build dependencies. No extra
packages.
"""

import argparse
import os
import pathlib
import random
import re
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent

# Files that contain decision logic. Pure plumbing (main.cpp, Logger, EventLoop)
# is left out: mutants there mostly crash at start-up and say little.
DEFAULT_TARGETS = [
    "src/balancer/Backend.cpp",
    "src/balancer/BackendPool.cpp",
    "src/balancer/LeastConnections.cpp",
    "src/balancer/RoundRobin.cpp",
    "src/balancer/WeightedRoundRobin.cpp",
    "src/core/Connection.cpp",
    "src/http/Headers.cpp",
    "src/http/HttpParser.cpp",
    "src/redis/Cache.cpp",
    "src/redis/RateLimiter.cpp",
    "src/util/Config.cpp",
    "src/util/Metrics.cpp",
]

OPERATORS = [
    # (name, regex, replacement). Relational operators are only matched with
    # spaces around them, which is how this code base writes comparisons and
    # which keeps template brackets (vector<int>) and arrows (->) out.
    ("ROR", r" < ", " <= "),
    ("ROR", r" <= ", " < "),
    ("ROR", r" > ", " >= "),
    ("ROR", r" >= ", " > "),
    ("ROR", r" == ", " != "),
    ("ROR", r" != ", " == "),
    ("LCR", r" && ", " || "),
    ("LCR", r" \|\| ", " && "),
    ("LIT", r"\btrue\b", "false"),
    ("LIT", r"\bfalse\b", "true"),
    ("AOR", r" \+ ", " - "),
    ("AOR", r" - ", " + "),
    ("NEG", r"\(!", "("),
]

# The seeded faults: one realistic mistake per requirement (IDs from
# software-testing/docs/01-requirements.md). `find` must occur exactly once.
SEEDED_FAULTS = [
    {
        "id": "S1", "requirement": "F8",
        "what": "rate limiter rejects the request that is exactly on the limit (off by one)",
        "file": "src/redis/RateLimiter.cpp",
        "find": "if (*count > maxRequests_) {",
        "replace": "if (*count >= maxRequests_) {",
    },
    {
        "id": "S2", "requirement": "F4",
        "what": "a backend needs one failure too many before it is removed",
        "file": "src/balancer/Backend.cpp",
        "find": "if (failures < failThreshold) {",
        "replace": "if (failures <= failThreshold) {",
    },
    {
        "id": "S3", "requirement": "F3",
        "what": "weighted round robin gives each backend one ticket too many",
        "file": "src/balancer/WeightedRoundRobin.cpp",
        "find": "if (ticket < weight) {",
        "replace": "if (ticket <= weight) {",
    },
    {
        "id": "S4", "requirement": "F5b",
        "what": "a POST that already reached a backend is sent again to another one",
        "file": "src/core/Connection.cpp",
        "find": "if (!requestSent_) {",
        "replace": "if (true) {",
    },
    {
        "id": "S5", "requirement": "F10",
        "what": "the client address is not appended to an existing X-Forwarded-For",
        "file": "src/core/Connection.cpp",
        "find": 'forwardedFor ? *forwardedFor + ", " + clientIp_ : clientIp_',
        "replace": "forwardedFor ? *forwardedFor : clientIp_",
    },
    {
        "id": "S6", "requirement": "F14, L1",
        "what": "two different Content-Length headers are accepted (request smuggling)",
        "file": "src/http/HttpParser.cpp",
        "find": "if (present && parsed != length) {",
        "replace": "if (false) {",
    },
    {
        "id": "S7", "requirement": "F7, L1",
        "what": "requests with a Cookie are answered from the shared cache",
        "file": "src/redis/Cache.cpp",
        "find": 'request.header("Authorization") == nullptr && request.header("Cookie") == nullptr',
        "replace": 'request.header("Authorization") == nullptr',
    },
    {
        "id": "S8", "requirement": "F13",
        "what": "shutdown waits for slow requests for ever instead of for the grace period",
        "file": "src/core/Server.cpp",
        "find": "if (std::chrono::steady_clock::now() >= drainDeadline_) {",
        "replace": "if (false) {",
    },
    {
        "id": "S9", "requirement": "F11",
        "what": "/stats does not escape quotes, so it can produce invalid JSON",
        "file": "src/util/Metrics.cpp",
        "find": "if (c == '\"' || c == '\\\\') {",
        "replace": "if (false) {",
    },
    {
        "id": "S10", "requirement": "F6",
        "what": "responses that are not 200 are stored in the cache",
        "file": "src/redis/Cache.cpp",
        "find": "if (response.status != 200) {",
        "replace": "if (false) {",
    },
]


# ------------------------------- mutants -------------------------------------

def code_part(line):
    """The part of a source line before a // comment (string literals respected roughly)."""
    in_string = False
    for i, ch in enumerate(line):
        if ch == '"' and (i == 0 or line[i - 1] != "\\"):
            in_string = not in_string
        if not in_string and line.startswith("//", i):
            return line[:i]
    return line


def generate_mutants(path):
    """Every single-change mutant of one file: (file, line no, operator, old line, new line)."""
    mutants = []
    lines = (ROOT / path).read_text().split("\n")
    for number, line in enumerate(lines, start=1):
        code = code_part(line)
        stripped = code.strip()
        if not stripped or stripped.startswith("#") or "spdlog::" in code or "MINILB_ASSERT" in code:
            continue   # nothing to mutate, preprocessor, log output, or a check rather than behaviour
        for name, pattern, replacement in OPERATORS:
            if name == "AOR" and '"' in code:
                continue   # '+' on strings: the mutant would not compile anyway
            for match in re.finditer(pattern, code):
                mutated = line[:match.start()] + replacement + line[match.end():]
                mutants.append((path, number, name, line, mutated))
    return mutants


# ------------------------------ build and run --------------------------------

class Runner:
    def __init__(self, build_dir):
        self.build_dir = ROOT / build_dir
        # A fixed seed for the property-based tests, so that the same mutant
        # gets the same verdict on every run of this tool.
        self.env = dict(os.environ, MINILB_REQUIRE_REDIS="1", RC_PARAMS="seed=1 max_success=300")

    def configure(self):
        subprocess.run(["cmake", "-S", str(ROOT), "-B", str(self.build_dir), "-DCMAKE_BUILD_TYPE=Debug"],
                       check=True, stdout=subprocess.DEVNULL)
        self.build(["unit_tests", "integration_tests", "miniLB", "demo_backend"], check=True)

    def build(self, targets, check=False):
        result = subprocess.run(
            ["cmake", "--build", str(self.build_dir), "-j", str(os.cpu_count() or 2), "--target", *targets],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if check and result.returncode != 0:
            sys.exit("the unmodified code does not build:\n" + result.stdout[-3000:])
        return result.returncode == 0

    def run_tests(self, program, timeout):
        """Returns (passed, name of the first failing test or a reason)."""
        try:
            # --gtest_fail_fast: one failing test is enough to kill a mutant,
            # and a broken miniLB can make every later test wait for a timeout.
            result = subprocess.run([str(self.build_dir / program), "--gtest_brief=1", "--gtest_fail_fast"],
                                    env=self.env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    timeout=timeout)
        except subprocess.TimeoutExpired:
            return False, "timeout (tests hung)"
        if result.returncode == 0:
            return True, ""
        failed = re.findall(r"^\[  FAILED  \] (\S+)", result.stdout, flags=re.M)
        if failed:
            return False, failed[0].rstrip(",")
        if result.returncode < 0:
            return False, f"aborted with signal {-result.returncode} (assertion or crash)"
        return False, f"exit status {result.returncode}"

    def judge(self):
        """Builds and tests whatever is in the source tree right now."""
        if not self.build(["unit_tests"]):
            return "invalid", ""
        passed, why = self.run_tests("unit_tests", timeout=120)
        if not passed:
            return "killed", "unit: " + why
        if not self.build(["integration_tests", "miniLB", "demo_backend"]):
            return "invalid", ""
        passed, why = self.run_tests("integration_tests", timeout=300)
        if not passed:
            return "killed", "integration: " + why
        return "survived", ""

    def baseline(self):
        for program, timeout in (("unit_tests", 120), ("integration_tests", 400)):
            passed, why = self.run_tests(program, timeout)
            if not passed:
                sys.exit(f"the unmodified code fails {program} ({why}); fix that first")


class Patched:
    """Context manager: a file with changed content, restored afterwards."""

    def __init__(self, path, content):
        self.path = ROOT / path
        self.content = content

    def __enter__(self):
        self.original = self.path.read_text()
        self.path.write_text(self.content)

    def __exit__(self, *_):
        self.path.write_text(self.original)


# --------------------------------- modes -------------------------------------

def mode_run(args):
    targets = args.files or DEFAULT_TARGETS
    everything = [m for path in targets for m in generate_mutants(path)]
    rng = random.Random(args.seed)
    chosen = everything if args.max >= len(everything) else rng.sample(everything, args.max)
    chosen.sort(key=lambda m: (m[0], m[1]))   # neighbours share a file: fewer rebuilds

    runner = Runner(args.build_dir)
    runner.configure()
    runner.baseline()

    results = []
    started = time.time()
    for index, (path, number, name, old, new) in enumerate(chosen, start=1):
        lines = (ROOT / path).read_text().split("\n")
        assert lines[number - 1] == old
        lines[number - 1] = new
        with Patched(path, "\n".join(lines)):
            verdict, why = runner.judge()
        results.append((path, number, name, old.strip(), new.strip(), verdict, why))
        print(f"[{index}/{len(chosen)}] {verdict:8} {path}:{number} {name}  {why}", flush=True)

    # Leave the build directory consistent with the restored sources.
    runner.build(["unit_tests", "integration_tests", "miniLB", "demo_backend"])

    killed = [r for r in results if r[5] == "killed"]
    survived = [r for r in results if r[5] == "survived"]
    invalid = [r for r in results if r[5] == "invalid"]
    by_unit = [r for r in killed if r[6].startswith("unit")]
    score = 100.0 * len(killed) / max(1, len(killed) + len(survived))

    out = []
    out.append("# Mutation testing report\n")
    out.append(f"Generated by `software-testing/tools/mutation.py run --max {args.max} --seed {args.seed}`"
               f" in {int(time.time() - started) // 60} min.\n")
    out.append("| | |\n|---|---|")
    out.append(f"| Possible mutants in the target files | {len(everything)} |")
    if len(chosen) == len(everything):
        out.append(f"| Mutants run | all {len(chosen)} (no sampling) |")
    else:
        out.append(f"| Sampled (seed {args.seed}) | {len(chosen)} |")
    out.append(f"| Invalid (did not compile, not counted) | {len(invalid)} |")
    out.append(f"| Killed | {len(killed)} ({len(by_unit)} by unit tests, {len(killed) - len(by_unit)} by integration tests) |")
    out.append(f"| Survived | {len(survived)} |")
    out.append(f"| **Mutation score** | **{score:.1f}%** ({len(killed)} of {len(killed) + len(survived)}) |\n")

    out.append("## Per file\n")
    out.append("| File | Killed | Survived | Score |\n|---|---:|---:|---:|")
    for path in sorted({r[0] for r in results}):
        k = sum(1 for r in killed if r[0] == path)
        s = sum(1 for r in survived if r[0] == path)
        if k + s:
            out.append(f"| `{path}` | {k} | {s} | {100.0 * k / (k + s):.0f}% |")
    out.append("")

    out.append("## Survivors\n")
    out.append("Each survivor is either a missing test or an equivalent mutant (a change that cannot be"
               " observed). Decide which, per row.\n")
    out.append("| # | Location | Operator | Original | Mutant | Missing test or equivalent? |\n|---|---|---|---|---|---|")
    for i, r in enumerate(survived, start=1):
        out.append(f"| {i} | `{r[0]}:{r[1]}` | {r[2]} | `{r[3]}` | `{r[4]}` | |")
    out.append("")

    out.append("## Killed mutants\n")
    out.append("| Location | Operator | Mutant | Killed by |\n|---|---|---|---|")
    for r in killed:
        out.append(f"| `{r[0]}:{r[1]}` | {r[2]} | `{r[4]}` | {r[6]} |")
    out.append("")

    report = "\n".join(out)
    if args.report:
        (ROOT / args.report).write_text(report)
        print(f"report written to {args.report}")
    print(f"\nmutation score: {score:.1f}% ({len(killed)} killed, {len(survived)} survived, {len(invalid)} invalid)")


def mode_seeded(args):
    runner = Runner(args.build_dir)
    runner.configure()
    runner.baseline()

    rows = []
    escaped = 0
    for fault in SEEDED_FAULTS:
        source = (ROOT / fault["file"]).read_text()
        if source.count(fault["find"]) != 1:
            rows.append((fault, "STALE", "the line to change was not found exactly once; update SEEDED_FAULTS"))
            escaped += 1
            continue
        with Patched(fault["file"], source.replace(fault["find"], fault["replace"])):
            verdict, why = runner.judge()
        if verdict != "killed":
            escaped += 1
        rows.append((fault, "CAUGHT" if verdict == "killed" else verdict.upper(), why))
        print(f"{fault['id']:4} {rows[-1][1]:8} {fault['what']}  [{why}]", flush=True)

    runner.build(["unit_tests", "integration_tests", "miniLB", "demo_backend"])

    print("\n| Fault | Requirement | Seeded mistake | Result | First failing test |")
    print("|---|---|---|---|---|")
    for fault, verdict, why in rows:
        print(f"| {fault['id']} | {fault['requirement']} | {fault['what']} | {verdict} | {why} |")
    print(f"\n{len(rows) - escaped} of {len(rows)} seeded faults caught")
    return 1 if escaped else 0


def main():
    # Turn "kill <pid>" into a normal exit so that a mutated file is restored.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["run", "seeded"])
    parser.add_argument("--build-dir", default="build-mutation")
    parser.add_argument("--max", type=int, default=60, help="how many mutants to sample (run mode)")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--files", nargs="*", help="source files to mutate (default: the decision logic)")
    parser.add_argument("--report", help="write a Markdown report to this path (relative to the repository)")
    args = parser.parse_args()
    sys.exit(mode_run(args) if args.mode == "run" else mode_seeded(args))


if __name__ == "__main__":
    main()

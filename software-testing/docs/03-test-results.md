# miniLB: test results (LO3)

> The numbers in this file were measured; re-run the commands in section 5 to
> reproduce them. The sections marked **YOUR ANALYSIS** are yours to write.
> Delete these quote blocks when you are done.

## 1. Summary

| | Original suite (commit `fa134d5`) | Now |
|---|---|---|
| Unit tests | 107 | 156 passing (154 in a Release build, where assertions are compiled out), 1 disabled (open defect D1) |
| Integration / system tests | 25 | 71 |
| Line coverage (`src/`, `include/`) | 94.3% (1085 / 1150) | 97.9% (1148 / 1173) |
| Branch coverage | 86.3% (1033 / 1197) | 92.6% (1109 / 1197) |
| Function coverage | not measured | 98.7% (157 / 159) |
| Mutation score | not measured | 91.1% (247 of 271 mutants killed, all mutants run) |
| Seeded faults caught | not measured | 10 of 10 |
| Defects found | | 1 (D1, open) |
| Requirements with no test | F5b, F10 (`X-Forwarded-For`) | none |

Every test passes in the Release build, under AddressSanitizer + UBSan and
under ThreadSanitizer. The new integration tests were additionally run five
times in a row under each sanitizer without a failure.

Branch coverage excludes the hidden branches GCC adds for calls that might
throw (`gcovr --exclude-throw-branches`) and the failing side of each
`MINILB_ASSERT`, which a passing run never takes. Line coverage was 98.2%
(1129 / 1150) before the assertions were added; the 23 new lines include the
four lines of the abort handler, which only run inside death tests (a process
that aborts does not write its coverage counters).

## 2. Results per technique

| Technique | Where | Adequacy criterion | Result |
|---|---|---|---|
| Functional tests from requirements | `tests/`, `software-testing/tests/integration/RetryAndForwardingTest.cpp` | Every requirement in `01-requirements.md` has at least one test | Met. Two requirements had none (F5b, F10); 12 tests added |
| Structural coverage as a gap finder | `software-testing/tests/*/CoverageGap*Test.cpp` | Line >= 95%, branch >= 88%, every remaining gap explained | Met: 97.9% / 92.6%. Remaining gaps in section 4 |
| Boundary value analysis | `software-testing/tests/unit/BoundaryValueTest.cpp` | Each numeric border tested just below, on and just above | Met for the borders listed in the file. Found D1 |
| Pairwise combinatorial | `software-testing/tests/integration/PairwiseConfigTest.cpp` | 100% of value pairs of 6 settings | Met: 93 of 93 pairs with 12 of 216 combinations, checked by `PairwiseTable.CoversEveryPairOfValues` |
| Model-based (finite state machine) | `software-testing/tests/unit/HealthStateMachineTest.cpp` | All states and transitions; random walks agree with the model | Met: 10 of 10 transitions; 5 threshold pairs x 3 x 5000 random steps |
| Dynamic analysis (sanitizers) | CI matrix | Zero reports over the whole suite | Met |
| Assertions (instrumentation) | `MINILB_ASSERT`, 18 checks in the production code | No assertion fires in any test run; the mechanism itself is tested | Met. 5 mutants were killed by an assertion (see `04-mutation-report.md`, "aborted with signal 6") |
| Mutation testing | `software-testing/tools/mutation.py run`, `04-mutation-report.md` | **Target: yours to set and justify** | 91.1%: 247 of 271 killed (213 by unit tests, 34 by integration tests), 24 survivors to triage |
| Seeded faults | `software-testing/tools/mutation.py seeded`, CI job "Seeded faults are caught" | Every seeded fault makes a test fail | Met: 10 of 10 |
| Fuzzing, property-based tests | not done yet | | |
| Performance with statistics | not done yet (only `benchmarks/run_benchmark.sh`, no target) | | |

## 3. Defects found (yield)

### D1: at the head-size limit, the parser's answer depends on how TCP splits the request

- **Found by:** boundary value analysis, `ParserBoundaries.DISABLED_RequestHeadOfExactlyTheLimitIsAcceptedHoweverItIsFragmented`.
- **Requirement:** F14 (same answer in any fragmentation), Q2 (head size limit).
- **Behaviour:** a request head of exactly `max_request_header_bytes` is accepted when it arrives in one read, but rejected with `431` when a read ends inside the final `\r\n\r\n`.
- **Cause:** `HttpParser::feed` (`src/http/HttpParser.cpp`) compares the *whole buffer* with the limit while the blank line is incomplete, but compares only the *head without the blank line* once it is complete. With 1 to 3 bytes of the terminator buffered the two checks disagree.
- **Severity:** low (needs a head within 3 bytes of the limit and an unlucky packet boundary), but it is non-deterministic from the client's point of view.
- **Status:** open. The test is committed with the `DISABLED_` prefix so that CI stays green; remove the prefix together with the fix.
- **Why the original suite missed it:** its oversize tests use heads far above the limit, and its fragmentation tests use heads far below it. Line and branch coverage of the code involved was already 100%.

> **YOUR ANALYSIS:** what does D1 say about coverage as an adequacy criterion?

## 4. Code that is still not executed

| Code | What it handles | Why no test | What would be needed |
|---|---|---|---|
| `src/core/Connection.cpp:218-219` | Writing the request to a backend fails after the connection was established | Needs the backend to reset the connection between connect and write | A fault-injecting proxy (e.g. toxiproxy) between miniLB and the backend |
| `src/core/Server.cpp:45-52` | `accept()` fails (typically "too many open files") and is retried after 100 ms | Needs the process to run out of file descriptors | Start miniLB with a very low `ulimit -n` and open more connections than that |
| `src/health/HealthChecker.cpp:101-104` | A health probe connects but its write fails | Same as the first row, for the probe | Fault-injecting proxy |
| `src/health/HealthChecker.cpp:123-125` | An exception inside a probe | No known input makes Asio throw here | Fault injection at the library boundary, or accept as defensive code |
| `src/balancer/WeightedRoundRobin.cpp:30` | Fallback `return` after the loop | Unreachable while every weight is >= 1 (enforced by `Backend` and `Config`); now guarded by an assertion | Nothing: infeasible path, kept to satisfy the compiler |
| `include/miniLB/util/Assert.hpp:26-31` | The abort handler of `MINILB_ASSERT` | Runs only in death tests, whose process aborts before coverage counters are written | Nothing: tested by `AssertionDeathTest.*`, invisible to gcov |

> **YOUR ANALYSIS (element 3.4):** evaluate the results. Questions to answer:
>
> - Coverage went from 86.3% to 92.6% of branches. Which of the remaining 88 branches matter, and which are the second half of a condition that cannot happen?
> - The pairwise suite uses 12 of 216 combinations. Which kinds of fault can it not find (three-way interactions)? Is there a three-way interaction in miniLB you would worry about?
> - The model-based test compares against a model you wrote. What if the model has the same mistake as the code?
> - One defect was found by 95 new tests. What does that suggest about the remaining defect density?
> - The mutation score is 91.1% with 24 survivors. How many of them are equivalent mutants, and what is the score once those are removed?

## 5. How to reproduce

```bash
# Everything, with coverage and the HTML report:
sudo apt-get install -y gcovr
software-testing/scripts/coverage.sh
xdg-open build-coverage/coverage/index.html

# Only the tests added in software-testing/:
scripts/build.sh
./build/unit_tests --gtest_filter='*Gaps*:*Boundar*:*HealthStateMachine*'
./build/integration_tests --gtest_filter='*Pairwise*:*RetryByMethod*:*ForwardedRequest*'

# The open defect D1:
./build/unit_tests --gtest_also_run_disabled_tests --gtest_filter='*Fragmented*'

# Mutation testing (all mutants, about 25 minutes) and the seeded faults (about 3 minutes):
python3 software-testing/tools/mutation.py run --max 100000 --report software-testing/docs/04-mutation-report.md
python3 software-testing/tools/mutation.py seeded

# Under the sanitizers (as CI does):
scripts/build.sh build-tsan -DMINILB_SANITIZER=thread  -DCMAKE_BUILD_TYPE=RelWithDebInfo
scripts/build.sh build-asan -DMINILB_SANITIZER=address -DCMAKE_BUILD_TYPE=RelWithDebInfo
ctest --test-dir build-tsan --output-on-failure
ctest --test-dir build-asan --output-on-failure
```

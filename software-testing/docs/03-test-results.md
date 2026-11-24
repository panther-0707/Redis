# miniLB: test results (LO3)

> The numbers in this file were measured; re-run the commands in section 5 to
> reproduce them. The sections marked **YOUR ANALYSIS** are yours to write.
> Delete these quote blocks when you are done.

## 1. Summary

| | Original suite (commit `fa134d5`) | Now |
|---|---|---|
| Unit tests | 107 | 183 passing (181 in a Release build, where assertions are compiled out; 10 of them are property-based and need RapidCheck) |
| Integration / system tests | 25 | 73 |
| Line coverage (`src/`, `include/`) | 94.3% (1085 / 1150) | 97.8% (1154 / 1180) |
| Branch coverage | 86.3% (1033 / 1197) | 93.0% (1117 / 1201) |
| Function coverage | not measured | 98.8% (158 / 160) |
| Mutation score | not measured | 94.5% (260 of 275 mutants killed, all mutants run); 91.1% in the first run |
| Seeded faults caught | not measured | 10 of 10 |
| Defects found | | 3, all fixed test-first: D1 (boundary testing), D2 (fuzzing), R2 (code review, confirmed by a test) |
| Fuzzing | not done | 2.38 million inputs in 150 s after the D2 fix, no finding |
| Requirements with no test | F5b, F10 (`X-Forwarded-For`) | none |

Every test passes in the Release build, under AddressSanitizer + UBSan and
under ThreadSanitizer. The integration tests added in the first round were
additionally run five times in a row under each sanitizer without a failure.

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
| Tests from review findings | `software-testing/tests/integration/ReviewFindingsTest.cpp` | Each confirmed finding has a test that fails before its fix | Done for R2. R1 and R3 are not tested yet |
| Structural coverage as a gap finder | `software-testing/tests/*/CoverageGap*Test.cpp` | Line >= 95%, branch >= 88%, every remaining gap explained | Met: 97.8% / 93.0%. Remaining gaps in section 4 |
| Boundary value analysis | `software-testing/tests/unit/BoundaryValueTest.cpp` | Each numeric border tested just below, on and just above | Met for the borders listed in the file. Found D1 (now fixed) |
| Pairwise combinatorial | `software-testing/tests/integration/PairwiseConfigTest.cpp` | 100% of value pairs of 6 settings | Met: 93 of 93 pairs with 12 of 216 combinations, checked by `PairwiseTable.CoversEveryPairOfValues` |
| Model-based (finite state machine) | `software-testing/tests/unit/HealthStateMachineTest.cpp` | All states and transitions; random walks agree with the model | Met: 10 of 10 transitions; 5 threshold pairs x 3 x 5000 random steps |
| Dynamic analysis (sanitizers) | CI matrix | Zero reports over the whole suite | Met |
| Assertions (instrumentation) | `MINILB_ASSERT`, 18 checks in the production code | No assertion fires in any test run; the mechanism itself is tested | Met. Mutants killed by an assertion are marked "aborted with signal 6" in the mutation reports |
| Mutation testing | `software-testing/tools/mutation.py run`, `04-mutation-report.md` | **Target: yours to set and justify** | First run 91.1% (24 survivors). After tests for 9 survivors and the three fixes: 94.5%, 260 of 275 killed (226 by unit tests, 34 by integration tests), 15 survivors to triage |
| Seeded faults | `software-testing/tools/mutation.py seeded`, CI job "Seeded faults are caught" | Every seeded fault makes a test fail | Met: 10 of 10 |
| Property-based testing | `software-testing/tests/unit/PropertyTest.cpp` (RapidCheck) | Each property holds for 2000 generated cases under `ctest` | Met: 10 properties. Checked against a known defect: with the D1 fix removed, the fragmentation property fails after 2310 cases and shrinks to "head of exactly the limit, split inside the blank line" |
| Fuzzing | `software-testing/fuzz/http_parser_fuzz.cpp` (libFuzzer + ASan + UBSan), `scripts/fuzz.sh`, CI job (60 s) | No crash, no sanitizer report, no broken property | Found D2 within about one second on its first run. After the fix: 2.38 million inputs in 150 s, 637 coverage points, no finding |
| Performance with statistics | `software-testing/tools/perf_stats.py`, `06-performance-report.md` | Target (Q3, reason in `01-requirements.md` §3): three backends give at least 2.5 times the throughput of one on `/work` | Met. 8 runs: three backends give 3.07 times the throughput of one (95% CI 3.04 to 3.08); cache on is 1.16 times cache off (1.13 to 1.20); both p = 0.0002 (exact Mann-Whitney) |

## 3. Defects found (yield)

### D1: at the head-size limit, the parser's answer depends on how TCP splits the request

- **Found by:** boundary value analysis, `ParserBoundaries.RequestHeadOfExactlyTheLimitIsAcceptedHoweverItIsFragmented`.
- **Requirement:** F14 (same answer in any fragmentation), Q2 (head size limit).
- **Behaviour (before the fix):** a request head of exactly `max_request_header_bytes` was accepted when it arrived in one read, but rejected with `431` when a read ended inside the final `\r\n\r\n`.
- **Cause:** `HttpParser::feed` (`src/http/HttpParser.cpp`) compared the *whole buffer* with the limit while the blank line was incomplete, but only the *head without the blank line* once it was complete. With 1 to 3 bytes of the terminator buffered the two checks disagreed.
- **Severity:** low (needs a head within 3 bytes of the limit and an unlucky packet boundary), but it is non-deterministic from the client's point of view.
- **Status:** fixed, test-first. Commit `1b8bf88` enables the test (it fails there); commit `ec79b88` changes `HttpParser::feed` so that both checks apply the same limit (the test passes). A second test checks that a head one byte over the limit still ends in `431` for every split point.
- **Why the original suite missed it:** its oversize tests use heads far above the limit, and its fragmentation tests use heads far below it. Line and branch coverage of the code involved was already 100%.

> **YOUR ANALYSIS:** what does D1 say about coverage as an adequacy criterion?

### D2: bytes after the end of a response counted towards the response size cap

- **Found by:** fuzzing (`software-testing/fuzz/http_parser_fuzz.cpp`), within about one second (43,000 inputs) of its first run. The property it broke: "a response parsed in one piece gives the same result as the same bytes in three pieces".
- **Requirement:** Q2 (`max_response_bytes`), F9 (`502` only for oversized or invalid responses).
- **Behaviour (before the fix):** a complete backend response followed by further bytes in the same read was rejected as too large when response plus extra bytes exceeded `max_response_bytes`, although the response itself fitted. The same extra bytes in a later read were ignored. Through miniLB that is a `502` for a valid response, depending on packet boundaries.
- **Cause:** `HttpResponseParser::feed` compared the raw buffer with the cap before parsing anything.
- **Status:** fixed, test-first. Commit `ac5b1d4` adds the fuzzer and keeps its input as a regression seed; `4255394` adds three unit tests reduced from it (they fail there); `f19b1c7` applies the cap to the response itself. The cap is now also enforced as soon as a declared length is known, before the body is buffered.
- **Why everything else missed it:** the response parser had tests for "too large" and for "split into pieces", but none for bytes after a complete response, and the property-based tests only covered the request parser.

### R2: successful requests did not end a backend's failure streak

- **Found by:** code review (`05-code-review.md`), then confirmed by a test written before any change (`MiniLbTest.R2_FailuresSeparatedBySuccessesDoNotRemoveABackend`).
- **Requirement:** F4 (removed after `fail_threshold` *consecutive* failures).
- **Behaviour (before the fix):** a backend that failed every second request was removed after its third failure, although no two failures were adjacent. Requests 6 to 10 of the test got `503`.
- **Status:** fixed, test-first. Commit `aaee16d` adds the failing test; `e3ee127` adds `Backend::recordRequestSuccess()` and calls it when a backend returns a complete response.
- **Why testing missed it:** every existing test and every mutant concerned code that was there. This fault was a missing call, which neither coverage nor mutation testing can point at.

## 4. Code that is still not executed

| Code | What it handles | Why no test | What would be needed |
|---|---|---|---|
| `src/core/Connection.cpp:222-223` | Writing the request to a backend fails after the connection was established | Needs the backend to reset the connection between connect and write | A fault-injecting proxy (e.g. toxiproxy) between miniLB and the backend |
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
> - Three defects were found: one each by boundary testing, fuzzing and review, none by the 132 original tests or by coverage. What does that say about relying on one technique?
> - The mutation score is 94.5% with 15 survivors. How many of them are equivalent mutants, and what is the score once those are removed?
> - The fuzzer found D2 in a second but nothing in the following 2.38 million inputs. How much confidence does that give, and what could it still not find (anything outside the two parsers)?

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

# Defect D1: the test fails at commit 1b8bf88 and passes from ec79b88 on
./build/unit_tests --gtest_filter='*Fragmented*'

# Mutation testing (all mutants, about 25 minutes) and the seeded faults (about 3 minutes):
python3 software-testing/tools/mutation.py run --max 100000 --report software-testing/docs/04-mutation-report.md
python3 software-testing/tools/mutation.py seeded

# Property-based tests with more cases, and replaying a failure:
RC_PARAMS="max_success=20000" ./build/unit_tests --gtest_filter='Property*'
RC_PARAMS="reproduce=<string printed by the failure>" ./build/unit_tests --gtest_filter='Property*'

# Fuzzing for five minutes (needs clang-18 and libclang-rt-18-dev):
software-testing/scripts/fuzz.sh 300

# Performance with statistics (needs wrk; about 4 minutes):
software-testing/tools/perf_stats.py --runs 8 --duration 5s --report software-testing/docs/06-performance-report.md

# Under the sanitizers (as CI does):
scripts/build.sh build-tsan -DMINILB_SANITIZER=thread  -DCMAKE_BUILD_TYPE=RelWithDebInfo
scripts/build.sh build-asan -DMINILB_SANITIZER=address -DCMAKE_BUILD_TYPE=RelWithDebInfo
ctest --test-dir build-tsan --output-on-failure
ctest --test-dir build-asan --output-on-failure
```

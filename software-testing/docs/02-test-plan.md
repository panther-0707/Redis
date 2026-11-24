# miniLB: test plan (LO2)

Requirement IDs refer to `01-requirements.md`.

## 1. Scope and priorities

| Priority | Requirements | Reason |
|---|---|---|
| High | L1, F5b, Q4, F4, F14 | These would have the most impact if incorrect and would be the hardest to notice. A cache leak or data race would not be visible to the client, and neither would a duplicated `POST`. A parser fault (F14) is what an attacker would try first. Most of the testing is focused here: retry tests, sanitizers, boundary values, fuzzing and property-based tests. |
| Medium | F1 to F3, F6 to F9, F13, Q1, Q2 | These would be visible if incorrect (wrong status code, a timeout that fails to fire, wrong backend) and would be reported quickly, but they would still affect all clients. They are covered by unit and system tests, pairwise tests and boundary values. |
| Low | F11, F12, L2, L4, Q3 | These relate mainly to performance, start-up and monitoring rather than to the correctness of answers. Most failures here would be visible immediately (a bad configuration stops miniLB from starting). They are covered by basic unit and system tests and the performance report. |

Out of scope, with reasons: TLS, HTTP/2, keep-alive (not implemented, see README "Known limitations").

## 2. Plan per requirement (element 2.1)

One row per test activity. "Done when" is the exit criterion you will measure
against in the LO4 evaluation.

| Activity | Requirements | Technique | Level | Tooling | Done when | Status |
|---|---|---|---|---|---|---|
| A1 | F1 to F14 | Functional unit and system tests | U, S | GoogleTest | Every F-row has at least one passing test | done (F5b and F10 added in `RetryAndForwardingTest.cpp`) |
| A2 | Q7 | Structural coverage, gap-driven tests | all | gcovr, `software-testing/scripts/coverage.sh` | Line >= 95%, branch >= 88%, remaining gaps listed and justified | done (`CoverageGap*Test.cpp`) |
| A3 | F6 to F8, Q2 | Boundary-value tests | U | GoogleTest | Each boundary has tests at limit - 1, limit, limit + 1 | done (`BoundaryValueTest.cpp`); found defect D1, since fixed |
| A4 | F12 and config options | Pairwise combinatorial | S | `tools/pairwise.py` + parameterised test | All 93 value pairs covered (checked by `PairwiseTable.CoversEveryPairOfValues`) | done (`PairwiseConfigTest.cpp`, 12 of 216 combinations) |
| A5 | F4 | Model-based (health state machine) | U | GoogleTest | All 10 transitions covered; random walks agree with the model | done (`HealthStateMachineTest.cpp`) |
| A6 | Q7 | Mutation testing | U, S | `tools/mutation.py run` | At least 95% of non-equivalent mutants killed; every survivor explained | done: 91.1%, then 94.5% after new tests; 98.1% of non-equivalent mutants (`04-mutation-report.md`) |
| A7 | F14, L1 | Fuzzing | U | libFuzzer + ASan + UBSan (`scripts/fuzz.sh`) | No crash and no broken property; seed corpus committed; 60 s in CI | done; found D2, then 2.38 million inputs clean |
| A8 | F1 to F3, F6, F8, F14 | Property-based tests | U | RapidCheck | Properties hold for 2000 generated cases under `ctest` | done (`PropertyTest.cpp`, 10 properties) |
| A9 | Q3 | Performance with statistics | S | wrk, `tools/perf_stats.py` | Target met with stated confidence | done: target "3 backends at least 2.5x one backend on `/work`" set in `01-requirements.md` §3; measured 3.07 (95% CI 3.04 to 3.08) |
| A10 | L1 to L4 | Code review | n/a | Checklist | Issues logged and triaged | first pass logged (`05-code-review.md`); triage open |
| A11 | all F | Seeded faults in CI | U, S | `tools/mutation.py seeded` | All 10 seeded faults make a test fail | done (CI job "Seeded faults are caught") |

## 3. How the plan evolved (element 2.1)

The baseline suite existed before this plan; the plan was written around it and then extended.

| Date | Change to the plan | Trigger | Commit / issue |
|---|---|---|---|
| 2026-11-12 | Baseline: existing unit and integration suite (107 + 25 tests) | Start of coursework | `f50265b` |
| 2026-11-14 | Added A2 after measuring coverage (94.3% line, 86.3% branch) and finding untested paths (host-name backends, grace-period expiry, CLI exit codes) | Coverage report | `8b43780` |
| 2026-11-14 | Added tests for F5b and F10 after requirements analysis showed that nothing checked "a POST is not retried" or `X-Forwarded-For` | Requirements review | `e060595` |
| 2026-11-14 | Added A3 to A5 (boundary, pairwise, model-based) to cover what line coverage cannot show | Review of A2 results | `50b72ce`, `77e6d1b`, `70fadf5` |
| 2026-11-14 | Boundary tests found defect D1 (head-size limit depends on fragmentation); test kept as `DISABLED_` until fixed | A3 | `50b72ce` |
| 2026-11-14 | Fix for D1, test-first: enable the disabled test, then change `HttpParser::feed` | D1 | `16da725` (test, red), `f59b54b` (fix, green) |
| 2026-11-14 | Tests added for 9 surviving mutants from the first mutation run | `04-mutation-report-run1.md` | `7c780db` |
| 2026-11-14 | Review finding R2 turned into a failing test, then fixed | `05-code-review.md` | `286ee83` (red), `3edcd5e` (green) |
| 2026-11-14 | Added A8 (property-based tests); checked that they re-find D1 when its fix is removed | D1 needed an input nobody had thought of | `c086109` |
| 2026-11-14 | Added A7 (fuzzing); it found D2 on its first run; test, then fix | A8 only covered the request parser | `6d35a35`, `0bb0681` (red), `73ff17f` (green) |
| 2026-11-14 | Added A9 (repeated benchmarks with statistics) | Q3 had single-run numbers only | `d2809ea` |

## 4. Instrumentation (element 2.3)

What is in the code to make failures visible, and why.

| Instrumentation | Where | What it reveals | Requirement |
|---|---|---|---|
| Access-log line per request | `Connection::logRequest` | status, backend, cache result, latency | L2 |
| `/stats` counters | `Metrics` | request, error, cache and rate-limit counts; backend health | F11 |
| `X-Cache`, `X-Backend` response headers | `Connection` | which path a request took, visible to black-box tests | F6, F1 to F3 |
| ThreadSanitizer, AddressSanitizer, UBSan builds | `CMakeLists.txt` (`MINILB_SANITIZER`) | data races, memory errors, undefined behaviour | Q4 |
| Coverage counters | `CMakeLists.txt` (`MINILB_COVERAGE`), `software-testing/scripts/coverage.sh` | which code the tests execute | Q7 |
| Assertions on invariants (`MINILB_ASSERT`, on in every build except Release) | `include/miniLB/util/Assert.hpp`; 18 checks in `Connection`, `Backend`, `BackendLease`, the strategies, `RateLimiter`, `HttpParser` | A broken internal state at the moment it happens, with message, condition and location. An abort makes every integration test fail (they require exit status 0). Examples: a connection is answered exactly once; in-flight counts never go below zero; no backend is contacted after the reply; `Retry-After` is between 1 and the window length | F4, F5, F8, F14 |

The assertion mechanism itself is tested in
`software-testing/tests/unit/AssertionTest.cpp` (a broken invariant aborts
with the right message; a Release build does not evaluate the condition).

## 5. Evaluation of the plan (element 2.2)

**Strengths.** Several independent techniques were used, and several of them found something the others missed: coverage found untested code paths, requirements analysis found two requirements with no test (F5b, F10), boundary testing found D1, fuzzing found D2 and code review found R2. Every activity also has a clear "done when" criterion, so progress can be measured rather than estimated.

**Omissions and weaknesses.** The tests do not simulate packet loss or delay over a real network. There are no long-running soak tests to detect slow leaks. There is no test of rate limiting with two miniLB instances sharing a single Redis instance, which is how it is meant to scale. The fuzzer only covers the two HTTP parsers, not the connection code around them.

**Vulnerabilities.** Several system tests rely on timing, for example waiting 300 ms for a cache write before expecting a cache hit, or expecting a timeout to fire within a time window. These tests were stable over repeated runs, but they could fail on a slower or busier machine for no real reason. The full mutation run takes about 25 minutes, so it is run by hand rather than on every push.

**Adequacy.** Even if every activity meets its "done when" criterion, I would not be fully confident in Q4 (thread safety), since sanitizers only observe the interleavings that occur during a test run, or in Q1 (deadlines) over a real network with packet loss and delay. Review findings R1 and R3 are also still open, so L1 and Q1 are not fully covered.

## 6. Evaluation of the instrumentation (element 2.4)

**Silent failures.** The 18 assertions are compiled out of the Release build, so a broken invariant can go unnoticed in production. A backend that returns the correct status code (200) but wrong data is not detected and its answer is passed on as it is. When Redis is down, rate limiting and caching are switched off and only a log message is written, so a client can make more requests than its rate limit allows.

**Timing and sanitizers.** Both sanitizers change the run time significantly: AddressSanitizer roughly doubles it, and ThreadSanitizer slows the program down many times. The change in timing can reveal races that need a slow thread, but it can also hide races that only occur when operations happen very close together. Running the tests in both a normal and a sanitizer build reduces the chance of this, but does not eliminate it.

**What I would add.** A request ID on every log line, so that all lines for one request can be found under load. Latency histograms in `/stats`, so that slow requests can be identified rather than just counted. A counter for completed cache writes, so that tests can wait for the write instead of sleeping for 300 ms (review finding R6).

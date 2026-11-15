# miniLB: test plan (LO2)

> Skeleton. Requirement IDs refer to `01-requirements.md`. Sections marked
> **YOUR ANALYSIS** need your own reasoning. Delete these quote blocks when
> you are done.

## 1. Scope and priorities

| Priority | Requirements | Reason |
|---|---|---|
| High | (e.g. L1, Q4, F5, F9) | **YOUR ANALYSIS:** impact if wrong x chance of being wrong |
| Medium | | |
| Low | | |

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
| A6 | Q7 | Mutation testing | U, S | `tools/mutation.py run` | Mutation score target met; survivors explained | two runs done: 91.1%, then 94.5% after new tests (`04-mutation-report.md`); target and triage of 15 survivors open |
| A7 | F14, L1 | Fuzzing | U | libFuzzer + ASan + UBSan (`scripts/fuzz.sh`) | No crash and no broken property; seed corpus committed; 60 s in CI | done; found D2, then 2.38 million inputs clean |
| A8 | F1 to F3, F6, F8, F14 | Property-based tests | U | RapidCheck | Properties hold for 2000 generated cases under `ctest` | done (`PropertyTest.cpp`, 10 properties) |
| A9 | Q3 | Performance with statistics | S | wrk, `tools/perf_stats.py` | Target met with stated confidence | tool and first report done (`06-performance-report.md`); **target not set yet** |
| A10 | L1 to L4 | Code review | n/a | Checklist | Issues logged and triaged | first pass logged (`05-code-review.md`); triage open |
| A11 | all F | Seeded faults in CI | U, S | `tools/mutation.py seeded` | All 10 seeded faults make a test fail | done (CI job "Seeded faults are caught") |

## 3. How the plan evolved (element 2.1)

> Keep this log honest and dated; link each entry to a commit or issue. Do
> not rewrite history: the baseline suite existed before the plan.

| Date | Change to the plan | Trigger | Commit / issue |
|---|---|---|---|
| | Baseline: existing unit and integration suite (107 + 25 tests) | Start of coursework | `fa134d5` |
| | Added A2 after measuring coverage (94.3% line, 86.3% branch) and finding untested paths (host-name backends, grace-period expiry, CLI exit codes) | Coverage report | |
| | Added tests for F5b and F10 after requirements analysis showed that nothing checked "a POST is not retried" or `X-Forwarded-For` | Requirements review | |
| | Added A3 to A5 (boundary, pairwise, model-based) to cover what line coverage cannot show | Review of A2 results | |
| | Boundary tests found defect D1 (head-size limit depends on fragmentation); test kept as `DISABLED_` until fixed | A3 | `d863640` |
| | Fix for D1, test-first: enable the disabled test, then change `HttpParser::feed` | D1 | `1b8bf88` (test, red), `ec79b88` (fix, green) |
| | Tests added for 9 surviving mutants from the first mutation run | `04-mutation-report-run1.md` | `3486cef` |
| | Review finding R2 turned into a failing test, then fixed | `05-code-review.md` | `aaee16d` (red), `e3ee127` (green) |
| | Added A8 (property-based tests); checked that they re-find D1 when its fix is removed | D1 needed an input nobody had thought of | `eeed56f` |
| | Added A7 (fuzzing); it found D2 on its first run; test, then fix | A8 only covered the request parser | `ac5b1d4`, `4255394` (red), `f19b1c7` (green) |
| | Added A9 (repeated benchmarks with statistics) | Q3 had single-run numbers only | `f20bd3e` |
| | New feature built test-first: (e.g. smooth weighted round robin) | README "Known limitations" | one commit for the failing test, one for the code |

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

> **YOUR ANALYSIS.** One short paragraph each:
>
> - **Strengths:** what does this plan catch well?
> - **Weaknesses and omissions:** what is not planned (e.g. real network conditions, long soak runs, multiple miniLB instances sharing Redis)?
> - **Vulnerabilities:** what could make the plan fail (time, flaky timing-based tests, tool support for C++ mutation testing)?
> - **Adequacy:** if every activity reaches its "Done when", which requirements would you still not trust, and why?

## 6. Evaluation of the instrumentation (element 2.4)

> **YOUR ANALYSIS.** Questions to answer:
>
> - Which failures would pass silently today (no log line, no counter, no assertion)?
> - Do sanitizer builds change timing enough to hide or expose races?
> - What would you add with more time (request IDs across log lines, latency histograms in `/stats`, structured logs)?

# miniLB: requirements and test strategy (LO1)

Test names are GoogleTest names (`Suite.Test`). Tests from the original suite
are in `tests/`; tests added during this work are in `software-testing/tests/`.

## 1. Requirements

Types: **F** = functional, **Q** = measurable quality attribute, **L** = qualitative.
Levels: **U** = unit, **I** = integration (two or more components), **S** = system (real miniLB process).

### 1.1 Functional requirements

| ID | Requirement | Level | Tests today |
|---|---|---|---|
| F1 | Round robin sends requests to healthy backends in strict rotation. | U, S | `RoundRobin.*`, `MiniLbTest.RoundRobinRotatesAcrossBackends` |
| F2 | Least connections picks the healthy backend with the fewest in-flight requests. | U, S | `LeastConnections.*`, `MiniLbTest.LeastConnectionsAvoidsTheBusyBackend` |
| F3 | Weighted round robin sends traffic in proportion to `weight`. | U, S | `WeightedRoundRobin.*`, `MiniLbTest.WeightedSendsProportionalTraffic` |
| F4 | A backend is removed after `fail_threshold` consecutive failures and re-added after `success_threshold` consecutive successes. Failed real requests count as failures. | U, I, S | `Backend.*`, `HealthStateMachine/*` (model-based), `MiniLbTest.FailoverWhenABackendDiesAndRecovers`, `MiniLbTest.HealthCheckerAloneRemovesADeadBackend`, `MiniLbTest.R2_*`. **Review finding R2** was a fault here, now fixed (see `03-test-results.md`). |
| F5a | A request that fails on one backend is retried once on a different healthy backend. | S | `MiniLbTest.RetriesOnceOnAnotherBackend`, `MiniLbTest.GivesUpAfterOneRetry` |
| F5b | A request that may already have reached a backend is retried only if its method is idempotent (a `POST` is not repeated). | S | `Methods/RetryByMethod.*` (7 methods), `MiniLbTest.APostIsRetriedWhenTheFirstBackendCouldNotBeReached`. **No test existed before this analysis.** |
| F6 | Cacheable `GET` responses are stored in Redis with a TTL and served without contacting a backend; `X-Cache` is `HIT`, `MISS` or `BYPASS`. | U, S | `Cache.*`, `MiniLbTest.CacheMissThenHit`, `MiniLbTest.CacheEntriesExpireAfterTheTtl` |
| F7 | Only shareable responses are cached (rules in README "What is cached"). | U, S | `Cache.OnlyPlainGetRequestsAreCacheable`, `Cache.OnlyShareable200ResponsesAreCacheable`, `MiniLbTest.OnlyCacheableRequestsUseTheCache` |
| F8 | Each client IP may send `rate_limit.requests` per window; further requests get `429` with `Retry-After`. `0` disables limiting. | U, S | `RateLimiter.*`, `RateLimitBoundaries.*`, `Limits/RateLimitBoundary.*`, `MiniLbTest.RateLimitReturns429AfterTheLimit` |
| F9 | miniLB answers `400`, `413`, `431`, `501`, `502`, `503`, `504` in the situations listed in README "HTTP behaviour"; other backend statuses pass through. | U, S | `HttpParser.*`, `MalformedRequest.*`, `MiniLbTest.Returns5xx*`, `MiniLbTest.PassesBackendStatusCodesThrough` |
| F10 | Requests to backends carry `X-Forwarded-For` (client IP appended) and `Connection: close`; replies carry `X-Backend`. | U, S | `Headers.*`, `ForwardedRequest.*`. **`X-Forwarded-For` was not asserted anywhere before this analysis.** |
| F11 | `GET /stats` returns valid JSON with the counters and per-backend state. | U, S | `Metrics.*`, `MiniLbTest.StatsReportsCountersAndBackends` |
| F12 | Unknown config keys and invalid values are rejected at start-up; exit code 0 for help, 2 for wrong usage, 1 for start-up errors. | U, S | `Config.*`, `ConfigGaps.*`, `ConfigBoundaries.*`, `MiniLbTest.HelpPrintsUsageAndExitsWithZero`, `MiniLbTest.WrongUsageExitsWithTwo`, `MiniLbTest.StartUpErrorsExitWithOneAndSayWhy`, `MiniLbTest.ExitsWithOneWhenThePortIsAlreadyTaken` |
| F13 | On `SIGINT`/`SIGTERM` in-flight requests finish, for at most `shutdown_grace_ms`, and miniLB exits 0. | S | `MiniLbTest.GracefulShutdownLetsInFlightRequestsFinish`, `MiniLbTest.ShutdownDropsRequestsThatOutliveTheGracePeriod` |
| F14 | The request parser accepts input in any fragmentation and rejects conflicting `Content-Length`, spaces in header names and folded header lines. | U | `HttpParser.HandlesOneByteAtATime`, `MalformedRequest.*`, `ParserBoundaries.*`, `PropertyParser.*`, the fuzzer. **Defect D1** was found here and fixed (see `03-test-results.md`): at the head-size limit the answer depended on fragmentation. |

### 1.2 Measurable quality attributes

| ID | Requirement | Measure | Level | Tests today |
|---|---|---|---|---|
| Q1 | Every phase has a deadline: connect, read, write (`timeouts_ms.*`). | Reply or close within timeout + margin | S | `MiniLbTest.Returns504WhenTheBackendIsTooSlow`, `MiniLbTest.ClosesAClientThatStallsInTheMiddleOfItsRequest` |
| Q2 | Size caps: request head, request body, backend response (`limits.*`). | Boundary: at the limit accepted, limit + 1 rejected | U, S | `HttpParser.AcceptsBodyExactlyAtTheLimit`, `ParserBoundaries.*`, `CacheBoundaries.*`, `FuzzFindings.*`, `MiniLbTest.OversizedRequestsAreRejected`. **Defect D2** was found here by fuzzing and fixed. |
| Q3 | Performance: throughput and latency through miniLB. | Not stated in the README. Target set in §3: with three backends, throughput on `/work` is at least 2.5 times that of one backend | S | `benchmarks/run_benchmark.sh` (single run), `software-testing/tools/perf_stats.py` (repeated runs, confidence intervals, optional pass/fail targets), `06-performance-report.md` |
| Q4 | Thread safety and memory safety under concurrency. | Zero ThreadSanitizer / AddressSanitizer / UBSan reports | U, S | whole suite under sanitizers in CI; `MiniLbTest.HandlesManyConcurrentClients` |
| Q5 | Fairness under concurrency. | Round robin exactly fair; weighted split matches weights | U | `RoundRobin.IsExactlyFairUnderConcurrency`, `WeightedRoundRobin.DistributesProportionallyToWeight` |
| Q6 | Availability when Redis is down: requests keep being proxied (fail-open), Redis is left alone for 1 s. | 100% of requests answered 2xx while Redis is down | U, S | `RedisClient.FailsOpenWhenRedisIsUnreachable`, `MiniLbTest.FailsOpenWhenRedisIsDown`, `MiniLbTest.SurvivesRedisGoingAwayAndComingBack` |
| Q7 | Test adequacy. | Line and branch coverage thresholds in `software-testing/scripts/coverage.sh` | all | CI "Coverage gate" job |

### 1.3 Qualitative requirements

| ID | Requirement | How it can be checked |
|---|---|---|
| L1 | Security: no request smuggling; one user's private response is never served to another. | Negative tests on the parser and cacheability rules; code review |
| L2 | Observability: one access-log line per request; `/stats` reflects reality. | Tests that read the log and `/stats`; review |
| L3 | Testability: `Cache` and `RateLimiter` depend on the `KeyValueStore` interface, not on Redis. | Review; unit tests run with `FakeStore` and no network |
| L4 | Portability: Linux, GCC 13, Ubuntu 24.04, Docker image. | CI Release job; Docker smoke test |

### 1.4 Which requirements matter most

1. **L1, security** (no request smuggling, no private page for one user served to another). These issues are silent failures that affect innocent users. For example a logged in page for one user might be cached and served to another user, or a specially constructed request might get through the proxy. There will be no error visible to a user and only testing and code review can pick these up.

2. **F5b, a `POST` should not be delivered twice.** This could cause an order or payment to be placed twice if a `POST` is retried after having been delivered to a backend by miniLB. This issue is similar to L1 because there is no visible error. Prior to this work there was no testing for this issue.

3. **Q4, thread safety.** There are multiple threads in miniLB. A data race might only be visible under some load after thousands of requests. Upon occurrence of such a data race counters might be corrupted or the application might crash. After occurrence of such a data race it is very difficult to reproduce the issue.

4. **F4, health checking.** Capacity is lost for all clients if a healthy backend is removed by accident. For this issue only code review discovered the problem (specifically finding R2): a backend was removed after three failures even though those failures were not in a row. Every second request to this backend would fail.

Issues in F9 (incorrect status codes) or F8 (rate limiting) are less important because the issues are visible to all clients immediately and are also reported relatively quickly.

## 2. Test approach per requirement (element 1.3)

| Requirements | Approach | Why this approach |
|---|---|---|
| F1 to F3, Q5 | Unit tests on the strategies; property-based tests for "any healthy set, any weights" | The code has no I/O and is based on pure logic. Unit tests can easily exercise specific paths. Property tests exercise the rule over arbitrary numbers of backends and weights, and not just the ones I chose. |
| F4 | Model-based: state machine healthy/unhealthy with streak counters, transition coverage | Health is a state (healthy/unhealthy) with a streak counter. Modelling this allows all possible transitions to be enumerated and verified to be covered by tests. |
| F5, F9, F13, Q1 | System tests with real processes and scripted fake backends (fault injection) | Timeout, shutdown, errors and retries require real time and real sockets. Each of these failure modes can be induced on demand using fake backends that either fail to respond or hang. |
| F6 to F8 | Category-partition and boundary values on the cache rules and the rate-limit window | These rules have clear boundaries (cacheable/non-cacheable, the Nth request within a time window). Off-by-one errors are likely to occur on these boundaries. Thus boundary values are good for detecting these errors. |
| F12 and the interaction of settings | Category-partition on config keys; pairwise over algorithm x cache x Redis x rate limit x method x backend state (`AllPairs/PairwiseConfig.*`) | There are 216 combinations for six settings which is too many to test exhaustively with system tests. Pairwise testing covers all pairs of setting values in just 12 tests. The majority of configuration-related bugs will involve one or two settings. |
| F14, L1 | Negative tests from a malformed-input catalogue; coverage-guided fuzzing | The parser must be tested with untrusted input. A fuzzer creates millions of such inputs and has discovered defect D2. Nobody had anticipated all such inputs. |
| Q3 | Repeated benchmark runs with a statistical comparison against the target | A single benchmark run is noisy. Using multiple runs with confidence intervals can reveal if a change is real or not. |
| Q4 | Dynamic analysis: sanitizers on the whole suite | Races cannot be detected by examining the output of tests. Races can be detected using ThreadSanitizer during normal test execution. |
| Q7 | Structural coverage (line, branch) and mutation score | Code coverage tracks which lines of code are executed during test runs. Mutation testing tests if errors would be detected by the current test suite. These two together give a picture of the quality of the tests. |
| L1 to L4 | Code review against a checklist | Trusting client headers and testability are design decisions and not behaviour. A checklist is appropriate for verifying these. |

## 3. Appropriateness and limits of the chosen approaches (element 1.4)

1. **Testing locally.** All system tests are run in a single node with essentially no network latency (less than 1 ms) and no data loss. There are no real issues such as packet loss, latency, real DNS lookups or multiple different client IP addresses. In practice all test clients use the same IP address and so the requirement of "one rate-limit budget per client IP" is only exercised at the unit level.

2. **Sanitizers.** ThreadSanitizer can indicate that there were no data races in the actual thread interleaving that took place in a given run. It does not however indicate that there are no data races in the code. A data race that requires a very specific timing may not be observable in CI. The small number of highly concurrent tests make detecting such a race more likely, but it is not guaranteed.

3. **Mutation vs coverage.** D1 was located in code that already had 100% branch and line coverage. This indicates that code coverage does not necessarily mean that the code has been sufficiently exercised. Mutation testing helps here; 91.1% of mutants were killed in the initial run (with ~98% line coverage). Nine new tests were created from the surviving mutants.

4. **Performance target.** A ratio rather than number of requests per second was used as a performance target. With three backends the performance on `/work` should be at least 2.5x that of a single backend. The raw performance can vary by up to 4.6% between runs on the same test infrastructure and would vary significantly on different infrastructure. Using a ratio measured on the same infrastructure however is more stable. A ratio of 3.07 (95% CI 3.04 to 3.08) was observed which satisfies the performance target (`06-performance-report.md`).

5. **Requirements which can only be verified via code inspection.** L3 (testability) and some portions of L1 (such as trusting the `X-Forwarded-For` header provided by a client, which results in R3) can only be verified by code inspection. There is a risk here that a requirement is only verified based on the capabilities of the reviewer and an issue identified via code inspection cannot be prevented from recurring.

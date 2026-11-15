# miniLB: requirements and test strategy (LO1)

> Requirements extracted from `README.md`. Check every row against the code
> and write the sections marked **YOUR ANALYSIS**. Delete these quote blocks
> when you are done.

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
| Q3 | Performance: throughput and latency through miniLB. | **No target stated in README: you must set one** (e.g. p99 under X ms at 200 connections; 3 backends give at least 2.5x one backend on `/work`) | S | `benchmarks/run_benchmark.sh` (single run), `software-testing/tools/perf_stats.py` (repeated runs, confidence intervals, optional pass/fail targets), `06-performance-report.md` |
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

> **YOUR ANALYSIS (1.1, 1.2):** Which requirements matter most if they fail,
> and why? Rank them (for example: L1 and Q4 are silent failures; F9 is
> visible to every client). Two or three sentences per high-risk requirement.

## 2. Test approach per requirement (element 1.3)

> For each group, name the technique and say why it fits. Suggested starting
> points are filled in; replace them with your own reasoning.

| Requirements | Approach | Why this approach (YOUR ANALYSIS) |
|---|---|---|
| F1 to F3, Q5 | Unit tests on the strategies; property-based tests for "any healthy set, any weights" | |
| F4 | Model-based: state machine healthy/unhealthy with streak counters, transition coverage | |
| F5, F9, F13, Q1 | System tests with real processes and scripted fake backends (fault injection) | |
| F6 to F8 | Category-partition and boundary values on the cache rules and the rate-limit window | |
| F12 and the interaction of settings | Category-partition on config keys; pairwise over algorithm x cache x Redis x rate limit x method x backend state (`AllPairs/PairwiseConfig.*`) | |
| F14, L1 | Negative tests from a malformed-input catalogue; coverage-guided fuzzing | |
| Q3 | Repeated benchmark runs with a statistical comparison against the target | |
| Q4 | Dynamic analysis: sanitizers on the whole suite | |
| Q7 | Structural coverage (line, branch) and mutation score | |
| L1 to L4 | Code review against a checklist | |

## 3. Appropriateness and limits of the chosen approaches (element 1.4)

> **YOUR ANALYSIS.** Be honest; this element rewards naming limitations.
> Questions to answer in a short paragraph each:
>
> 1. System tests use `localhost` and a demo backend. What real-world behaviour does that hide (latency, packet loss, real DNS, many client IPs)?
> 2. Sanitizers only see the interleavings that actually happen in a run. What does a clean ThreadSanitizer run prove, and what does it not?
> 3. Coverage shows which code ran, not whether the assertions were strong. How will mutation testing compensate?
> 4. Q3 has no stated target. How did you choose yours, and how sensitive are the numbers to the machine?
> 5. Which requirements can only be reviewed, not tested (L3, parts of L1), and what is the risk of that?

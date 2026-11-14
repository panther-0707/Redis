# miniLB: code review of `Connection` (LO5, element 5.1)

> **Status: first pass only.** The findings below come from a first read by
> an AI reviewer (Claude). The author's part is still open: confirm or reject
> each finding against the code, set the severity, decide the action, and add
> anything the first pass missed. Fill the last two columns of the findings
> table and section 5, then delete this block.

## 1. What was reviewed and why

| | |
|---|---|
| Code | `src/core/Connection.cpp`, `include/miniLB/core/Connection.hpp` (about 550 lines) |
| Why this file | It holds the whole request path (rate limit, cache, backend choice, retry, deadlines). It is the largest source file, holds 41 of the 88 branches no test takes, and can only be tested through real sockets, so review is the main defence for it. |
| Method | Checklist-based inspection: every function read once per checklist category, findings logged with location and the requirement they affect |
| Reviewer | Claude (AI assistant), first pass |
| Author's triage | to do |

## 2. Review criteria (checklist)

| # | Category | Questions asked of every function |
|---|---|---|
| C1 | Correctness against requirements | Does the behaviour match `01-requirements.md` and the README? Any case where the documented rule does not hold? |
| C2 | Failure paths | What happens on every error code, timeout and early close? Is the client always answered or cleanly dropped? Is state cleaned up on each path? |
| C3 | Deadlines and resource bounds | Is every wait bounded by a timer? Is every buffer and queue bounded? |
| C4 | Concurrency and lifetime | Is each member only touched on the connection's strand? Does every pending operation keep the object alive? What is copied to other threads? |
| C5 | Security and input trust | Which client-controlled values reach backends, Redis keys or logs, and are they validated? |
| C6 | Observability | Would a failure here be visible in the access log or `/stats`? |
| C7 | Maintainability and testability | One responsibility per function? Can the logic be tested without real processes? Do comments match the code? |

## 3. Findings

Severity suggested by the reviewer: **M** = should be fixed or explicitly accepted, **L** = minor.

| ID | Where | Category | Finding | Suggested severity | Confirmed? (author) | Action (author) |
|---|---|---|---|---|---|---|
| R1 | `readRequest` -> `onRequestComplete` -> `connectToBackend` | C3 | **No deadline covers the Redis step.** The read timer is stopped when the request is complete and the next timer is only armed in `connectToBackend`. In between, the request waits in the blocking pool's queue and for the Redis calls. Each call is bounded by `redis.timeout_ms`, but the queue is not: with a slow (not dead) Redis and 4 pool threads, requests pile up and clients wait with no 504. This contradicts README "Deadlines everywhere". | M | | |
| R2 | `onBackendFailure`, `onBackendResponse` | C1 (F4) | **Successful requests never reset the failure streak.** A failed request calls `Backend::recordFailure`, but a successful one does not call `recordSuccess`; only the health checker's probes do. "Failures in a row" therefore means "failures since the last successful probe". Under load, `fail_threshold` sporadic failures inside one probe interval remove a backend that is answering thousands of requests correctly. | M | | |
| R3 | `forwardRequest` | C5 (L1) | **A client-supplied `X-Forwarded-For` is trusted.** The existing value is kept and the peer address appended. A backend that reads the first entry can be told any client address. This is common proxy behaviour, but it is not documented and there is no "trusted proxy" setting. | M | | |
| R4 | `reply` | C6 (L2) | **The result of writing the reply is ignored.** The completion handler takes no error code, so the access log records the status as delivered even if the client had gone or the write timed out. | L | | |
| R5 | constructor | C2 (F8) | When `remote_endpoint()` fails, `clientIp_` becomes `"unknown"`, and every such connection shares the rate-limit bucket `ratelimit:unknown:*`. Rare (the peer reset right after accept). | L | | |
| R6 | `onBackendResponse` | C7 | **The cache write gives no signal when it is done** (fire-and-forget on the blocking pool). Tests have to `sleep(300 ms)` before expecting a `HIT`, which is slow and can be flaky on a loaded machine. A `cache_writes` counter in `/stats` would let tests wait for the write. | L | | |
| R7 | `onBackendFailure` | C7 | The function does four jobs (clean-up, health marking, retry decision, error reply) and depends on a subtle order (`timedOut_` must be copied before `stopTimer()`). Hard to change safely; it is also where the remaining uncovered branches are. | L | | |
| R8 | `onRedisResult` | C1 | A cache hit is replayed with its original `Date` header and no `Age` header, so a client cannot tell that the response is up to `ttl_seconds` old. Not promised by the README; noted for HTTP correctness. | L | | |
| R9 | `onBackendFailure` | C1 (README "Known limitations") | Timeouts, oversized responses and unparsable responses all count as health failures. Already documented by the author; listed here because together with R2 one slow endpoint can remove a healthy backend. | L (known) | | |
| D1 | `HttpParser::feed` (not in this file) | C1 (F14) | Found by boundary testing, not by review: see `03-test-results.md`. Listed so the review log links to it. | M | yes (test) | fix test-first |

## 4. Checked, no issue found

Recording what was looked at and found sound is part of the evidence.

| Category | What was checked | Result |
|---|---|---|
| C4 lifetime | Every asynchronous operation in the file captures `self` (`shared_from_this()`), including the timer and both blocking-pool tasks | Sound. No handler can run on a destroyed `Connection`. |
| C4 threads | The blocking-pool task in `onRequestComplete` uses only its own copies (`clientIp`, `key`, `lookup`) and the thread-safe `RateLimiter` and `Cache`; it posts the result back to the strand and touches no member | Sound. Supported by the ThreadSanitizer runs. |
| C4 threads | The cache write in `onBackendResponse` captures the cache by reference and a *copy* of the response; `main()` destroys the pool before the cache | Sound. |
| C2 retry | `canRetry`: one retry at most; after bytes were sent only idempotent methods | Sound, and now tested (`RetryByMethod`, `GivesUpAfterOneRetry`). |
| C2 timer race | A timer handler that was already queued when its phase ended is ignored through the generation counter | Sound. |
| C3 buffers | Request head, request body and backend response are capped before they are buffered | Sound apart from D1. |
| C5 logs | Request target and method come from the parser, which rejects control characters and spaces, so they cannot inject log lines | Sound. |

## 5. Outcome

> **Author to write.** Suggested content:
>
> - Which findings you confirmed, which you rejected and why.
> - Which you will fix now (for example R2 is a few lines plus a test), which you accept and document in the README, and which you defer.
> - What the review found that the tests could not (R1 to R3 pass every test and are invisible to coverage) and what the tests found that the review did not (D1).
> - How you would run the review differently with more time (a second human reviewer, a walkthrough of the Server and HealthChecker code as well).

# miniLB: code review of `Connection` (LO5, element 5.1)

## 1. What was reviewed and why

| | |
|---|---|
| Code | `src/core/Connection.cpp`, `include/miniLB/core/Connection.hpp` (about 550 lines) |
| Why this file | It holds the whole request path (rate limit, cache, backend choice, retry, deadlines). It is the largest source file, holds 41 of the 84 branches no test takes, and can only be tested through real sockets, so review is the main defence for it. |
| Method | Checklist-based inspection: every function read once per checklist category, findings logged with location and the requirement they affect |
| Reviewer | First pass by Claude (AI assistant); every finding then checked against the code by the author |
| Author's triage | Done: verdict and action for each finding in §3 |

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
| R1 | `readRequest` -> `onRequestComplete` -> `connectToBackend` | C3 | **No deadline covers the Redis step.** The read timer is stopped when the request is complete and the next timer is only armed in `connectToBackend`. In between, the request waits in the blocking pool's queue and for the Redis calls. Each call is bounded by `redis.timeout_ms`, but the queue is not: with a slow (not dead) Redis and 4 pool threads, requests pile up and clients wait with no 504. This contradicts README "Deadlines everywhere". | M | Confirmed: no timer runs between the end of the request read and the start of the backend connection, so a slow Redis can make a client wait with no deadline and no 504. | Fix later: arm a deadline before the Redis step. Recorded as a known issue. |
| R2 | `onBackendFailure`, `onBackendResponse` | C1 (F4) | **Successful requests never reset the failure streak.** A failed request calls `Backend::recordFailure`, but a successful one does not call `recordSuccess`; only the health checker's probes do. "Failures in a row" therefore means "failures since the last successful probe". Under load, `fail_threshold` sporadic failures inside one probe interval remove a backend that is answering thousands of requests correctly. | M | yes: `MiniLbTest.R2_FailuresSeparatedBySuccessesDoNotRemoveABackend` failed before the fix | fixed test-first (`aaee16d`, `e3ee127`) |
| R3 | `forwardRequest` | C5 (L1) | **A client-supplied `X-Forwarded-For` is trusted.** The existing value is kept and the peer address appended. A backend that reads the first entry can be told any client address. This is common proxy behaviour, but it is not documented and there is no "trusted proxy" setting. | M | Confirmed: a client-supplied `X-Forwarded-For` header is kept and the real client IP is appended. | Accept and document. This is standard proxy behaviour; backends should only trust the last address in the header. |
| R4 | `reply` | C6 (L2) | **The result of writing the reply is ignored.** The completion handler takes no error code, so the access log records the status as delivered even if the client had gone or the write timed out. | L | Confirmed: the result of writing the reply is ignored, so the access log can report a status the client never received. | Defer: low impact, it only affects the access log. |
| R5 | constructor | C2 (F8) | When `remote_endpoint()` fails, `clientIp_` becomes `"unknown"`, and every such connection shares the rate-limit bucket `ratelimit:unknown:*`. Rare (the peer reset right after accept). | L | Confirmed: if the client's address cannot be read, the request is rate-limited under the shared "unknown" key. | Accept: very rare, it only happens when a client disconnects immediately after connecting. |
| R6 | `onBackendResponse` | C7 | **The cache write gives no signal when it is done** (fire-and-forget on the blocking pool). Tests have to `sleep(300 ms)` before expecting a `HIT`, which is slow and can be flaky on a loaded machine. A `cache_writes` counter in `/stats` would let tests wait for the write. | L | Confirmed: nothing signals when a cache write has completed, so tests sleep for 300 ms. | Defer: add a cache-writes counter to `/stats`. |
| R7 | `onBackendFailure` | C7 | The function does four jobs (clean-up, health marking, retry decision, error reply) and depends on a subtle order (`timedOut_` must be copied before `stopTimer()`). Hard to change safely; it is also where the remaining uncovered branches are. | L | Confirmed: `onBackendFailure` does four jobs and relies on a specific order of operations. | Defer the refactoring; the retry tests cover this behaviour meanwhile. |
| R8 | `onRedisResult` | C1 | A cache hit is replayed with its original `Date` header and no `Age` header, so a client cannot tell that the response is up to `ttl_seconds` old. Not promised by the README; noted for HTTP correctness. | L | Confirmed: a cached response keeps its original `Date` header and has no `Age` header. | Accept: not required by the README. |
| R9 | `onBackendFailure` | C1 (README "Known limitations") | Timeouts, oversized responses and unparsable responses all count as health failures. Already documented by the author; listed here because together with R2 one slow endpoint can remove a healthy backend. | L (known) | Confirmed: a timeout counts as a health failure; this is already documented as a known limitation. | Accept. The R2 fix makes it less likely that this removes a healthy backend. |
| D1 | `HttpParser::feed` (not in this file) | C1 (F14) | Found by boundary testing, not by review: see `03-test-results.md`. Listed so the review log links to it. | M | yes (test) | fixed test-first (`1b8bf88`, `ec79b88`) |

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

All nine findings were confirmed, and one (R2) was fixed test-first: a test that failed before the change and passes after it. R1 and R3 are the most significant open findings, because they affect deadlines and security. R1 has the highest priority for a fix; R3 is accepted as standard proxy behaviour and documented. The other findings are low impact and are either deferred or accepted.

The review found problems the tests could not. Before the review, R1, R2 and R3 passed every test and were invisible to coverage and mutation testing, because they concern missing code or a design choice rather than incorrect code. In the other direction, the tests found D1 (boundary testing) and D2 (fuzzing), which a reader would most likely have missed, because they only appear at one specific input and split point.

With more time, a second person would review the same code independently, since one reviewer's blind spots stay hidden. The review would also be extended to `HealthChecker.cpp` and `Server.cpp`, which both contain untested error paths.

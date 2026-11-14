# Software testing of miniLB

Everything added for the Software Testing coursework lives in this folder.
The original test suite is untouched in `tests/` (its shared helpers moved to
`tests/integration/TestSupport.hpp` so that the tests here can use the same
fixture).

## What is here

```
software-testing/
  docs/
    01-requirements.md      requirements, levels, test approach per requirement   (LO1)
    02-test-plan.md         activities, exit criteria, plan history, instrumentation (LO2)
    03-test-results.md      numbers, defects found, code still untested            (LO3, LO4)
    04-mutation-report.md   mutation score and the list of surviving mutants       (LO3, LO4)
    05-code-review.md       checklist review of Connection, findings to triage     (LO5)
  tests/
    unit/
      AssertionTest.cpp           the assertion mechanism itself (MINILB_ASSERT)
      CoverageGapUnitTest.cpp     tests for code the original unit suite never ran
      BoundaryValueTest.cpp       just below / on / just above every numeric limit
      HealthStateMachineTest.cpp  model-based: backend health as a state machine
    integration/
      CoverageGapTest.cpp         system tests for paths the original suite never ran
      RetryAndForwardingTest.cpp  requirements that had no test (F5b, F10)
      PairwiseConfigTest.cpp      all-pairs over six settings, 12 of 216 combinations
      PairwiseCases.inc           the generated table (do not edit)
  tools/
    pairwise.py             generates and checks PairwiseCases.inc
    mutation.py             mutation testing (`run`) and the seeded-fault check for CI (`seeded`)
  scripts/
    coverage.sh             build with gcov, run everything, report and gate coverage
```

The test files are compiled into the existing `unit_tests` and
`integration_tests` programs (see `CMakeLists.txt`), so `ctest`, the sanitizer
jobs and the coverage job run them with no extra steps.

## Technique, tests, requirement

| Technique | Tests | Requirements (`docs/01-requirements.md`) |
|---|---|---|
| Structural coverage as a gap finder | `*Gaps.*`, `MiniLbTest.*` in `CoverageGapTest.cpp`, `HealthProbe.*`, `HealthChecker.*` | F4, F5a, F9, F12, F13, Q1 |
| Requirements-based functional tests | `Methods/RetryByMethod.*`, `ForwardedRequest.*` | F5b, F10 |
| Boundary value analysis | `*Boundar*` | F8, F12, F14, Q2 |
| Pairwise combinatorial | `AllPairs/PairwiseConfig.*`, `PairwiseTable.*` | F1 to F3, F5 to F9 in combination |
| Model-based (state machine) | `HealthStateMachine/*` | F4 |
| Dynamic analysis | whole suite under ThreadSanitizer and AddressSanitizer + UBSan (CI) | Q4 |
| Assertions (instrumentation) | `MINILB_ASSERT` in the production code, `AssertionDeathTest.*` | F4, F5, F8, F14 |
| Mutation testing | `tools/mutation.py run`, report in `docs/04-mutation-report.md` | adequacy of the whole suite (Q7) |
| Seeded faults | `tools/mutation.py seeded`, CI job "Seeded faults are caught" | one fault per requirement F3 to F14 |

## Running

```bash
scripts/build.sh
ctest --test-dir build --output-on-failure        # original + new tests

software-testing/scripts/coverage.sh               # needs gcovr; report in build-coverage/coverage/
python3 software-testing/tools/pairwise.py --write # regenerate the pairwise table
python3 software-testing/tools/mutation.py seeded  # 10 seeded faults, all must be caught (about 5 min)
python3 software-testing/tools/mutation.py run --max 70 --seed 1 --report software-testing/docs/04-mutation-report.md
```

## Results in one paragraph

229 tests pass (132 before), line coverage is 97.9% (94.3% before) and branch
coverage 92.6% (86.3% before). The suite kills 91.1% of all 271 mutants and
all 10 seeded faults. Two requirements that had no test now have one, and
boundary testing found one defect, fixed test-first (D1 in
`docs/03-test-results.md`).

## Not done yet

The sections marked YOUR ANALYSIS in `docs/`, the triage of the mutation
survivors and of the review findings, and (optional) fuzzing,
property-based tests and performance tests with a target.

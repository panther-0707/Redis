// Model-based tests of backend health (requirement F4).
//
// Technique: model-based testing with a finite state machine. The model is
// written from the specification, not from the code:
//
//   "A backend becomes unhealthy after `failThreshold` failures in a row and
//    healthy again after `successThreshold` successes in a row."
//    (Backend.hpp, README "Health checks")
//
// With failThreshold = F and successThreshold = S the states are
//
//   H0 .. H(F-1)   healthy,   with 0 .. F-1 failures in a row so far
//   U0 .. U(S-1)   unhealthy, with 0 .. S-1 successes in a row so far
//
// and the transitions are
//
//   Hf --failure--> H(f+1)         if f+1 < F
//   H(F-1) --failure--> U0         reports "changed"
//   Hf --success--> H0             (a success breaks the failure streak)
//   Us --success--> U(s+1)         if s+1 < S
//   U(S-1) --success--> H0         reports "changed"
//   Us --failure--> U0             (a failure breaks the success streak)
//
// For F = 3, S = 2 that is 5 states and 10 transitions:
//
//   state | on failure      | on success
//   ------+-----------------+----------------
//   H0    | H1              | H0
//   H1    | H2              | H0
//   H2    | U0  (changed)   | H0
//   U0    | U0              | U1
//   U1    | U0              | H0  (changed)
//
// Adequacy criteria used here:
//   1. state coverage and transition coverage: every transition of the model
//      is taken at least once (AllTransitions test, one case per transition);
//   2. long random walks compare the implementation with the model after
//      every step, for several threshold pairs including the smallest (1, 1).

#include "miniLB/balancer/Backend.hpp"

#include <gtest/gtest.h>

#include <ostream>
#include <random>
#include <string>
#include <vector>

using minilb::Backend;

namespace {

enum class Event { Failure, Success };

// The specification as an executable model.
class HealthModel {
public:
    HealthModel(int failThreshold, int successThreshold)
        : failThreshold_(failThreshold), successThreshold_(successThreshold) {}

    // Applies one probe result. Returns true if `healthy` changed.
    bool apply(Event event) {
        if (healthy_) {
            if (event == Event::Success) {
                streak_ = 0;
                return false;
            }
            if (++streak_ < failThreshold_) {
                return false;
            }
            healthy_ = false;
            streak_ = 0;
            return true;
        }
        if (event == Event::Failure) {
            streak_ = 0;
            return false;
        }
        if (++streak_ < successThreshold_) {
            return false;
        }
        healthy_ = true;
        streak_ = 0;
        return true;
    }

    bool healthy() const { return healthy_; }

    // "H1", "U0", ... for failure messages.
    std::string name() const { return (healthy_ ? "H" : "U") + std::to_string(streak_); }

private:
    const int failThreshold_;
    const int successThreshold_;
    bool healthy_ = true;   // backends start healthy
    int streak_ = 0;        // failures in a row while healthy, successes in a row while unhealthy
};

// Feeds the same event to the real Backend and to the model and compares
// both observable outputs: the `healthy` flag and the "changed" return value.
class Pair {
public:
    Pair(int failThreshold, int successThreshold)
        : failThreshold_(failThreshold),
          successThreshold_(successThreshold),
          backend_("10.0.0.1", 80),
          model_(failThreshold, successThreshold) {}

    void step(Event event, const std::string& context) {
        const std::string from = model_.name();
        const bool expectedChange = model_.apply(event);
        const bool actualChange = event == Event::Failure ? backend_.recordFailure(failThreshold_)
                                                          : backend_.recordSuccess(successThreshold_);
        const char* label = event == Event::Failure ? "failure" : "success";
        EXPECT_EQ(actualChange, expectedChange)
            << context << ": " << from << " --" << label << "--> " << model_.name() << " (changed?)";
        EXPECT_EQ(backend_.healthy.load(), model_.healthy())
            << context << ": " << from << " --" << label << "--> " << model_.name() << " (healthy?)";
    }

    const HealthModel& model() const { return model_; }

private:
    const int failThreshold_;
    const int successThreshold_;
    Backend backend_;
    HealthModel model_;
};

// ---- Criterion 1: every transition of the F = 3, S = 2 machine --------------

struct Transition {
    const char* from;                // state the transition starts in
    std::vector<Event> pathToFrom;   // shortest way there from the start state H0
    Event event;
    const char* to;
    bool changes;                    // does `healthy` flip?
};

void PrintTo(const Transition& transition, std::ostream* out) {
    *out << transition.from << (transition.event == Event::Failure ? " --failure--> " : " --success--> ")
         << transition.to;
}

class AllTransitions : public ::testing::TestWithParam<Transition> {};

TEST_P(AllTransitions, ImplementationFollowsTheModel) {
    const Transition& transition = GetParam();
    Pair pair(3, 2);

    for (const Event event : transition.pathToFrom) {
        pair.step(event, "on the way to the start state");
    }
    ASSERT_EQ(pair.model().name(), transition.from) << "the test did not reach its start state";
    const bool healthyBefore = pair.model().healthy();

    pair.step(transition.event, "transition under test");

    EXPECT_EQ(pair.model().name(), transition.to);
    EXPECT_EQ(pair.model().healthy() != healthyBefore, transition.changes);
}

const Event F = Event::Failure;
const Event S = Event::Success;

INSTANTIATE_TEST_SUITE_P(
    HealthStateMachine, AllTransitions,
    ::testing::Values(
        //          from  path to it          event to    changes
        Transition{"H0", {},                  F,    "H1", false},
        Transition{"H0", {},                  S,    "H0", false},
        Transition{"H1", {F},                 F,    "H2", false},
        Transition{"H1", {F},                 S,    "H0", false},
        Transition{"H2", {F, F},              F,    "U0", true},    // threshold reached: removed
        Transition{"H2", {F, F},              S,    "H0", false},   // one failure short, then saved
        Transition{"U0", {F, F, F},           S,    "U1", false},
        Transition{"U0", {F, F, F},           F,    "U0", false},   // further failures change nothing
        Transition{"U1", {F, F, F, S},        S,    "H0", true},    // threshold reached: back in
        Transition{"U1", {F, F, F, S},        F,    "U0", false})); // one success short, then lost

// ---- Criterion 2: random walks for several threshold pairs ------------------

struct Thresholds {
    int fail;
    int success;
};

void PrintTo(const Thresholds& thresholds, std::ostream* out) {
    *out << "fail=" << thresholds.fail << " success=" << thresholds.success;
}

class RandomWalk : public ::testing::TestWithParam<Thresholds> {};

TEST_P(RandomWalk, ImplementationAndModelAgreeAfterEveryStep) {
    const Thresholds thresholds = GetParam();

    // Fixed seeds: a failure can be reproduced exactly. Three different
    // failure probabilities so that the walk spends time in the healthy
    // states, the unhealthy states and on the border between them.
    int flips = 0;
    for (const double failureProbability : {0.2, 0.5, 0.8}) {
        std::mt19937 random(20261114);
        std::bernoulli_distribution fails(failureProbability);
        Pair pair(thresholds.fail, thresholds.success);

        bool wasHealthy = true;
        for (int step = 0; step < 5000 && !::testing::Test::HasFailure(); ++step) {
            pair.step(fails(random) ? Event::Failure : Event::Success,
                      "step " + std::to_string(step) + ", p(failure)=" + std::to_string(failureProbability));
            if (pair.model().healthy() != wasHealthy) {
                wasHealthy = !wasHealthy;
                ++flips;
            }
        }
    }
    // Guard against walks that prove nothing because they never left the
    // start state: the backend must have gone down and come back.
    EXPECT_GE(flips, 2);
}

INSTANTIATE_TEST_SUITE_P(HealthStateMachine, RandomWalk,
                         ::testing::Values(Thresholds{1, 1},   // smallest machine: 2 states
                                           Thresholds{1, 3}, Thresholds{3, 1},
                                           Thresholds{3, 2},   // the shipped defaults
                                           Thresholds{5, 4}));

// ---- The model's start state is part of the specification ------------------

TEST(HealthStateMachine, BackendsStartHealthy) {
    const Backend backend("10.0.0.1", 80);
    EXPECT_TRUE(backend.healthy.load()) << "README: backends start out healthy";
}

} // namespace

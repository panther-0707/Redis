// Tests of the instrumentation itself (MINILB_ASSERT).
//
// An assertion that never fires could be dead code: disabled by a build flag,
// or written so that it cannot fail. These tests show that
//   1. a broken invariant really stops the process, with a message that says
//      what was expected and where (in builds that have assertions on), and
//   2. a build without assertions does not even evaluate the condition.
//
// MINILB_ASSERTIONS is defined by CMake for every build type except Release.

#include "miniLB/balancer/Backend.hpp"
#include "miniLB/util/Assert.hpp"

#include <gtest/gtest.h>

namespace {

#ifdef MINILB_ASSERTIONS

class AssertionDeathTest : public ::testing::Test {
protected:
    // "threadsafe" re-executes the test binary for the death test instead of
    // only forking, which is the reliable style when sanitizers are in use.
    void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST_F(AssertionDeathTest, ABrokenInvariantAbortsWithMessageConditionAndLocation) {
    const int two = 2;
    EXPECT_DEATH(MINILB_ASSERT(two == 3, "two must be three"),
                 "miniLB assertion failed: two must be three\n"
                 "  condition: two == 3\n"
                 "  at .*AssertionTest\\.cpp:[0-9]+");
}

TEST_F(AssertionDeathTest, AHoldingInvariantDoesNothing) {
    int evaluations = 0;
    MINILB_ASSERT(++evaluations == 1, "evaluated once, and true");
    EXPECT_EQ(evaluations, 1);
}

// A real invariant from the production code. Config::validate() rejects a
// threshold below 1 at start-up, so miniLB itself can never make this call;
// the assertion guards the contract for any future caller.
TEST_F(AssertionDeathTest, BackendRejectsAThresholdBelowOne) {
    minilb::Backend backend("10.0.0.1", 80);
    EXPECT_DEATH(backend.recordFailure(0), "the failure threshold must be at least 1");
    EXPECT_DEATH(backend.recordSuccess(0), "the success threshold must be at least 1");
}

#else

TEST(Assertion, IsNotEvaluatedWhenAssertionsAreOff) {
    int evaluations = 0;
    MINILB_ASSERT(++evaluations == 99, "never evaluated in this build");
    EXPECT_EQ(evaluations, 0);
}

#endif

} // namespace

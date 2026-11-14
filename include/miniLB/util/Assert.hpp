#pragma once

// MINILB_ASSERT(condition, "what must be true here")
//
// Checks an internal invariant: something that must hold if miniLB's own
// code is correct, whatever clients, backends or Redis do. (Bad *input* is
// never an assertion failure; it is answered with a 4xx/5xx status.)
//
// When a check fails the process prints the message, the condition and the
// source location to stderr and aborts. Failing loudly at the first wrong
// state is the point: without the check the same bug would show up much
// later as a wrong counter in /stats or a request sent to the wrong place.
// An abort also fails every integration test, because they require miniLB
// to exit with status 0.
//
// The checks are compiled in when MINILB_ASSERTIONS is defined. CMake defines
// it for every build type except Release (option MINILB_ASSERTIONS), so the
// sanitizer, coverage and debug builds run with them and the production
// build pays nothing.

#include <cstdio>
#include <cstdlib>

namespace minilb::detail {

[[noreturn]] inline void assertionFailed(const char* file, int line, const char* condition,
                                         const char* message) {
    std::fprintf(stderr, "miniLB assertion failed: %s\n  condition: %s\n  at %s:%d\n", message,
                 condition, file, line);
    std::fflush(stderr);
    std::abort();
}

} // namespace minilb::detail

#ifdef MINILB_ASSERTIONS
#define MINILB_ASSERT(condition, message)                                                          \
    ((condition) ? static_cast<void>(0)                                                            \
                 : ::minilb::detail::assertionFailed(__FILE__, __LINE__, #condition, message))
#else
// Not evaluated, but still type-checked, so a disabled assertion cannot rot
// and its variables do not count as unused.
#define MINILB_ASSERT(condition, message) static_cast<void>(sizeof((condition) ? 1 : 0))
#endif

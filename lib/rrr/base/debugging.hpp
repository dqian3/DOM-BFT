#pragma once

#include <assert.h>
#include <stdio.h>

#ifndef likely
#define likely(x) __builtin_expect((x), 1)
#endif   // likely

#ifndef unlikely
#define unlikely(x) __builtin_expect((x), 0)
#endif   // unlikely

namespace rrr {

/**
 * Use assert() when the test is only intended for debugging.
 * Use rrr_verify() when the test is crucial for both debug and release binary.
 */
#ifdef NDEBUG
#define rrr_verify(expr)                                                                                               \
    do {                                                                                                               \
        if (unlikely(!(expr))) {                                                                                       \
            printf("  *** verify failed: %s at %s, line %d\n", #expr, __FILE__, __LINE__);                             \
            ::rrr::print_stack_trace();                                                                                \
            ::abort();                                                                                                 \
        }                                                                                                              \
    } while (0)
#else
#define rrr_verify(expr) assert(expr)
#endif

void print_stack_trace(FILE *fp = stderr) __attribute__((noinline));

}   // namespace rrr

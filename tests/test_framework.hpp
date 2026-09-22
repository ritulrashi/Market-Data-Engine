#pragma once

#include <iostream>

inline int g_test_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "CHECK FAILED: " << #cond << " at " << __FILE__ << ":"          \
                      << __LINE__ << "\n";                                               \
            ++g_test_failures;                                                           \
        }                                                                                 \
    } while (0)

#define RUN_TEST(fn)                        \
    do {                                     \
        std::cerr << "running " #fn "...\n"; \
        fn();                                \
    } while (0)

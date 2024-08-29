#include <gtest/gtest.h>

#include "lib/log.h"

#include <iostream>

// Demonstrate some basic assertions.
TEST(TestLog, TestBasicLog)
{
    Log log;

    log.addEntry(1, 1, "test");

    std::cout << log;
}

TEST(TestLog, TestCircular)
{
    Log log;

    for (int i = 1; i < 2 * MAX_SPEC_HIST; i++) {
        log.addEntry(1, i, "Test");
    }
}

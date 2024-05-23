#include <gtest/gtest.h>

#include "lib/log.h"

#include <iostream>

// Demonstrate some basic assertions.
TEST(TestLog, TestBasicLog)
{
    Log log;

    const char *req = "test";

    log.addEntry(1, 1, (byte *) req, strlen(req));

    std::cout << log;
}



TEST(TestLog, TestCircular)
{
    Log log;

    const char *req = "test";

    for (int i = 1; i < 2 * MAX_SPEC_HIST; i++) {
        log.addEntry(1, i, (byte *) req, strlen(req));

    }


}

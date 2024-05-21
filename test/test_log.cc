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

TEST(TestLog, TestExecuteFails)
{
    Log log;

    const char *req = "test";
    log.addEntry(1, 1, (byte *) req, strlen(req));


    ASSERT_FALSE(log.addAndExecuteEntry(2, 4, (byte *) req, strlen(req)));

}

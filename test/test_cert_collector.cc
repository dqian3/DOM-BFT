#include <gtest/gtest.h>

#include "lib/cert_collector.h"

// TODO write tests

using namespace ::testing;
using namespace dombft::proto;

TEST(CertCollectorTest, CollectCertF1)
{
    uint32_t f = 1;
    CertCollector collector(1);

    Reply replyBase;
    replyBase.set_client_id(1);
    replyBase.set_instance(0);
    replyBase.set_client_seq(5);
    replyBase.set_digest("abc");
    replyBase.set_seq(10);

    for (int i = 0; i < 2 * f + 1; i++) {
        Reply reply = replyBase;
        reply.set_replica_id(i);

        EXPECT_EQ(collector.insertReply(reply, {}), i + 1);
    }

    ASSERT_TRUE(collector.hasCert());
}

TEST(CertCollectorTest, CollectCertF10)
{
    uint32_t f = 10;
    CertCollector collector(1);

    Reply replyBase;
    replyBase.set_client_id(1);
    replyBase.set_instance(0);
    replyBase.set_client_seq(5);
    replyBase.set_digest("abc");
    replyBase.set_seq(10);

    for (int i = 0; i < 2 * f + 1; i++) {
        Reply reply = replyBase;
        reply.set_replica_id(i);

        EXPECT_EQ(collector.insertReply(reply, {}), i + 1);
    }

    ASSERT_TRUE(collector.hasCert());
}

TEST(CertCollectorTest, CollectProof)
{
    uint32_t f = 10;
    CertCollector collector(1);

    Reply replyBase;
    replyBase.set_client_id(1);
    replyBase.set_instance(0);
    replyBase.set_client_seq(5);
    replyBase.set_digest("abc");
    replyBase.set_seq(10);

    for (int i = 0; i < 2 * f + 1; i++) {
        Reply reply = replyBase;
        reply.set_replica_id(i);
        reply.set_digest("abc" + std::to_string(i));
        reply.set_seq(10 + i);

        EXPECT_EQ(collector.insertReply(reply, {}), 1);
    }

    ASSERT_FALSE(collector.hasCert());
    ASSERT_EQ(collector.numReceived(), 2 * f + 1);
}

TEST(CertCollectorTest, InstanceChange)
{
    uint32_t f = 1;
    CertCollector collector(1);

    Reply replyBase;
    replyBase.set_client_id(1);
    replyBase.set_instance(0);
    replyBase.set_client_seq(5);
    replyBase.set_digest("abc");
    replyBase.set_seq(10);

    for (int i = 0; i < 2 * f + 1; i++) {
        Reply reply = replyBase;
        reply.set_replica_id(i);

        EXPECT_EQ(collector.insertReply(reply, {}), i + 1);
    }

    ASSERT_TRUE(collector.hasCert());

    for (int i = 0; i < f + 1; i++) {
        Reply reply = replyBase;
        reply.set_replica_id(i);
        reply.set_instance(1);

        if (i == f + 1) {
            EXPECT_EQ(collector.insertReply(reply, {}), f + 1);
        } else {
            EXPECT_EQ(collector.insertReply(reply, {}), 2 * f + 1);
        }
    }

    ASSERT_FALSE(collector.hasCert());
}
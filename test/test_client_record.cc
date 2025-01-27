#include "lib/client_record.h"

#include <gtest/gtest.h>

using namespace ::testing;
using namespace dombft;

class ClientSequenceUtilTest : public Test {
protected:
    ClientSequence clientSequence;

    void SetUp() override
    {
        for (uint32_t i = 0; i < 5; i++)
            clientSequence.update(i);
    }
};

void setSequence(ClientRecord &record, uint32_t id, uint32_t lastSeq, std::set<uint32_t> missedSeqs)
{
    for (int i = 0; i <= lastSeq; i++) {
        if (missedSeqs.contains(i)) {
            // Missed requests
            continue;
        }
        record.update(id, i);
    }
}

class ClientRecordShiftTest : public Test {
protected:
    ClientRecord checkpointRecord;
    ClientRecord replicaRecord;

    void SetUp() override
    {
        setSequence(checkpointRecord, 0, 10, {6, 7});
        setSequence(replicaRecord, 1, 18, {16, 17});
        setSequence(replicaRecord, 1, 18, {16, 17});
    }
};

TEST_F(ClientSequenceUtilTest, UpdateSequenceWithHigherSeq)
{
    uint32_t initialSeq = 5;
    clientSequence.update(initialSeq);
    EXPECT_EQ(clientSequence.lastSeq_, initialSeq);

    uint32_t newSeq = 10;
    EXPECT_TRUE(clientSequence.update(newSeq));
    EXPECT_EQ(clientSequence.lastSeq_, newSeq);
    // Check missed sequences
    EXPECT_EQ(clientSequence.missedSeqs_.size(), newSeq - initialSeq - 1);
    std::vector seqs(clientSequence.missedSeqs_.begin(), clientSequence.missedSeqs_.end());
    std::sort(seqs.begin(), seqs.end());
    for (uint32_t i : seqs) {
        EXPECT_EQ(i, ++initialSeq);
    }
    EXPECT_EQ(clientSequence.lastSeq_, newSeq);
}

TEST_F(ClientSequenceUtilTest, UpdateSequenceWithSameSeq)
{
    clientSequence.update(5);
    EXPECT_FALSE(clientSequence.update(5));
    EXPECT_EQ(clientSequence.lastSeq_, 5);
}

TEST_F(ClientSequenceUtilTest, UpdateSequenceWithOldSeq)
{
    clientSequence.update(5);
    EXPECT_FALSE(clientSequence.update(4));
}

TEST_F(ClientSequenceUtilTest, UpdateSequenceWithMissingSeq)
{
    clientSequence.update(5);
    clientSequence.update(10);
    uint32_t missedSeq = 6;
    EXPECT_TRUE(clientSequence.missedSeqs_.contains(missedSeq));
    EXPECT_TRUE(clientSequence.update(missedSeq));
    EXPECT_FALSE(clientSequence.missedSeqs_.contains(missedSeq));
}

TEST_F(ClientRecordShiftTest, GetShiftNumWhenSlow)
{
    // replica:    ... 4 5 _ _ 8 9 10
    // checkpoint: ... 4 5 6 7 8 _  _
    setSequence(replicaRecord, 0, 8, {6, 7});
    replicaRecord.update(1, 19);
    replicaRecord.update(1, 20);

    EXPECT_EQ(replicaRecord.numMissing(checkpointRecord), 2);
};

TEST_F(ClientRecordShiftTest, GetShiftNumWhenMiss1)
{
    // replica:    ... 4 5 _ _ 8 9 10 _ _ __ __
    // checkpoint: ... 4 5 _ _ 8 _ __ _ _ 13 14

    setSequence(replicaRecord, 0, 14, {6, 7, 9, 10, 11, 12});

    EXPECT_EQ(replicaRecord.numMissing(checkpointRecord), 2);
};

TEST_F(ClientRecordShiftTest, GetShiftNumWhenMissAndSlow)
{
    // replica:    ... 4 5 _ _ 8 9 10
    // checkpoint: ... 4 _ _ 7 8 _  _
    setSequence(replicaRecord, 0, 8, {5, 6});

    replicaRecord.update(1, 19);

    EXPECT_EQ(replicaRecord.numMissing(checkpointRecord), 3);
};
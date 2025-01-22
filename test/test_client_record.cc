#include "lib/client_record.h"

#include <gtest/gtest.h>

using namespace ::testing;
using namespace dombft;

class ClientRecordUtilTest : public Test {
protected:
    ClientRecord clientRecord;

    void SetUp() override
    {
        for (uint32_t i = 0; i < 5; i++)
            clientRecord.update(i);
    }
};

class ClientRecordShiftTest : public Test {
protected:
    ClientRecords checkpointRecords;
    ClientRecords replicaRecords;

    void SetUp() override
    {
        checkpointRecords[0].lastSeq_ = 10;
        checkpointRecords[0].missedSeqs_ = {6, 7};
        checkpointRecords[1].lastSeq_ = 18;
        checkpointRecords[1].missedSeqs_ = {16, 17};

        checkpointRecords[0].lastSeq_ = 10;
        checkpointRecords[0].missedSeqs_ = {6, 7};
        replicaRecords[1].lastSeq_ = 18;
        replicaRecords[1].missedSeqs_ = {16, 17};
    }
};

TEST_F(ClientRecordUtilTest, UpdateRecordWithHigherSeq)
{
    uint32_t initialSeq = 5;
    clientRecord.update(initialSeq);
    EXPECT_EQ(clientRecord.lastSeq_, initialSeq);

    uint32_t newSeq = 10;
    EXPECT_TRUE(clientRecord.update(newSeq));
    EXPECT_EQ(clientRecord.lastSeq_, newSeq);
    // Check missed sequences
    EXPECT_EQ(clientRecord.missedSeqs_.size(), newSeq - initialSeq - 1);
    std::vector seqs(clientRecord.missedSeqs_.begin(), clientRecord.missedSeqs_.end());
    std::sort(seqs.begin(), seqs.end());
    for (uint32_t i : seqs) {
        EXPECT_EQ(i, ++initialSeq);
    }
    EXPECT_EQ(clientRecord.lastSeq_, newSeq);
}

TEST_F(ClientRecordUtilTest, UpdateRecordWithSameSeq)
{
    clientRecord.update(5);
    EXPECT_FALSE(clientRecord.update(5));
    EXPECT_EQ(clientRecord.lastSeq_, 5);
}

TEST_F(ClientRecordUtilTest, UpdateRecordWithOldSeq)
{
    clientRecord.update(5);
    EXPECT_FALSE(clientRecord.update(4));
}

TEST_F(ClientRecordUtilTest, UpdateRecordWithMissingSeq)
{
    clientRecord.update(5);
    clientRecord.update(10);
    uint32_t missedSeq = 6;
    EXPECT_TRUE(clientRecord.missedSeqs_.contains(missedSeq));
    EXPECT_TRUE(clientRecord.update(missedSeq));
    EXPECT_FALSE(clientRecord.missedSeqs_.contains(missedSeq));
}

TEST_F(ClientRecordShiftTest, GetShiftNumWhenSlow)
{

    // client 0 is slow on replica
    replicaRecords[0].lastSeq_ = 8;
    replicaRecords[0].missedSeqs_ = {6, 7};
    replicaRecords[1].update(19);
    replicaRecords[1].update(20);

    EXPECT_EQ(getRightShiftNumWithRecords(checkpointRecords, replicaRecords), 2);
};

TEST_F(ClientRecordShiftTest, GetShiftNumWhenMiss1)
{

    replicaRecords[0].lastSeq_ = 14;
    replicaRecords[0].missedSeqs_ = {6, 7, 9, 10, 11, 12};

    EXPECT_EQ(getRightShiftNumWithRecords(checkpointRecords, replicaRecords), 2);
};

TEST_F(ClientRecordShiftTest, GetShiftNumWhenMissAndSlow)
{
    replicaRecords[0].lastSeq_ = 8;
    replicaRecords[0].missedSeqs_ = {5, 6};

    replicaRecords[1].update(19);

    EXPECT_EQ(getRightShiftNumWithRecords(checkpointRecords, replicaRecords), 2);
};
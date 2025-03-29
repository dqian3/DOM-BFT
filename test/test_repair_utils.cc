#include "lib/application.h"
#include "lib/log.h"
#include "lib/repair_utils.h"
#include "lib/signature_provider.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <iostream>
#include <vector>

using namespace ::testing;

// TODO Mock out signatures
class MockSignatureProvider : public SignatureProvider {
public:
    MOCK_METHOD(
        bool, verify,
        (byte * data, uint32_t dataLen, byte *sig, uint32_t sigLen, const std::string &pubKeyType, int pubKeyId)
    );
    MOCK_METHOD(bool, verify, (MessageHeader * hdr, byte *body, const std::string &pubKeyType, int pubKeyId));

    MockSignatureProvider()
    {
        // Default behavior for methods to return true
        ON_CALL(*this, verify(_, _, _, _, _, _))
            .WillByDefault(Return(true));   // Default true for both verify overloads

        ON_CALL(*this, verify(_, _, _, _)).WillByDefault(Return(true));   // Default true for both verify overloads
    }
};

// Mock class for the Application abstract class
class MockApplication : public Application {
public:
    // Mock the pure virtual functions from the Application class
    MOCK_METHOD(std::string, execute, (const std::string &serialized_request, uint32_t execute_idx), (override));
    MOCK_METHOD(bool, commit, (uint32_t commit_idx), (override));
    MOCK_METHOD(bool, abort, (uint32_t abort_idx), (override));
    MOCK_METHOD(bool, applySnapshot, (const std::string &snapshot, const std::string &digest), (override));
    MOCK_METHOD(bool, applyDelta, (const std::string &delta, const std::string &digest), (override));
    MOCK_METHOD(AppSnapshot, takeSnapshot, (), (override));

    MockApplication()
    {
        // Default behavior for methods to return true
        EXPECT_CALL(*this, execute(_, _)).WillRepeatedly(Return("res"));
    }
};

//************************ Test case specification types ************************/
struct TestLogEntry {
    uint32_t c_id;
    uint32_t c_seq;

    std::string req = "request";
};

struct TestLog {
    uint32_t startSeq;
    std::string logDigest;
    uint32_t certSeq;

    std::vector<TestLogEntry> entries;

    uint32_t roundNum = 5;
};

typedef std::vector<TestLog> TestHistory;

/************************ Test case generation helpers ************************/
std::pair<std::shared_ptr<Log>, std::shared_ptr<Application>> logFromTestLog(const TestLog &testLog)
{
    std::shared_ptr<MockApplication> app = std::make_shared<MockApplication>();
    std::shared_ptr<Log> log = std::make_shared<Log>(app);

    log->getCheckpoint().seq = testLog.startSeq;
    log->abort(testLog.startSeq + 1);

    // Zero digest because provided TestLog.digest might be smaller
    log->getCheckpoint().logDigest = testLog.logDigest.c_str();

    for (const TestLogEntry &e : testLog.entries) {
        std::string res;

        dombft::proto::PaddedRequestData reqData;
        reqData.set_req_data(e.req);

        if (!log->addEntry(e.c_id, e.c_seq, reqData.SerializeAsString(), res)) {
            LOG(WARNING) << "Could not add entry in logFromTestLog c_id=" << e.c_id << " c_seq=" << e.c_seq;
        }

        if (log->getNextSeq() - 1 == testLog.certSeq) {
            // TODO make this cert "real"
            dombft::proto::Cert cert;
            cert.set_round(testLog.roundNum);
            cert.set_seq(testLog.certSeq);

            auto &r = (*cert.add_replies());
            r.set_client_id(e.c_id);
            r.set_client_seq(e.c_seq);
            r.set_digest(log->getDigest());
            if (!log->addCert(log->getNextSeq() - 1, cert)) {
                LOG(WARNING) << "Could not add cert in logFromTestLog";
            }
        }
    }

    return {log, app};
}

std::unique_ptr<dombft::proto::RepairStart> suffixFromTestLog(const TestLog &testLog, LogSuffix &ret)
{
    auto [log, app] = logFromTestLog(testLog);

    auto logMsg = std::make_unique<dombft::proto::RepairStart>();
    log->toProto(*logMsg);

    ret.checkpoint = &logMsg->checkpoint();
    ret.replicaId = 5;
    ret.round = 5;

    for (auto &e : logMsg->log_entries()) {
        ret.entries.push_back(&e);
    }

    // We need to return logMsg here so the pointers in ret aren't invalid
    return logMsg;
}

// Create logs with requests of just raw integers, based on
// TODO make a checkpoint here too
std::unique_ptr<dombft::proto::RepairProposal> generateRepairProposal(int f, TestHistory &history)
{
    std::unique_ptr<dombft::proto::RepairProposal> ret = std::make_unique<dombft::proto::RepairProposal>();
    int roundNum = 5;

    for (TestLog &t : history) {
        auto [log, app] = logFromTestLog(t);

        // This is a bit messy oops
        dombft::proto::RepairStart logMsg;
        log->toProto(logMsg);

        (*ret->add_logs()) = logMsg;
        ret->add_signatures("");
    }

    ret->set_round(roundNum);
    ret->set_replica_id(roundNum % (3 * f + 1));

    return ret;
}

/************************ Test assert helpers ************************/

void assertLogSuffixEq(const LogSuffix &suffix, const TestLog &expected)
{
    // TODO check checkpoint

    ASSERT_EQ(suffix.entries.size(), expected.entries.size());

    int n = expected.entries.size();
    for (int i = 0; i < n; i++) {
        const dombft::proto::LogEntry *actual = suffix.entries[i];
        const TestLogEntry &expectedEntry = expected.entries[i];

        EXPECT_EQ(actual->client_id(), expectedEntry.c_id);
        EXPECT_EQ(actual->client_seq(), expectedEntry.c_seq);

        // You might think we shuold do this (I did at first), but since
        // the log suffix doesn't update the underlying seq, these will not be right
        // EXPECT_EQ(actual->seq(), expected.startSeq + i + 1);
    }
}

void assertLogEq(Log &log, const TestLog &expected)
{
    // TODO check the checkpoints

    uint32_t startSeq = expected.startSeq;

    // Size of suffixes are the same
    ASSERT_EQ(expected.entries.size(), log.getNextSeq() - log.getCheckpoint().seq - 1);

    int n = expected.entries.size();
    for (int i = 0; i < n; i++) {
        const TestLogEntry &expectedEntry = expected.entries[i];
        const LogEntry &actual = log.getEntry(startSeq + i + 1);

        EXPECT_EQ(actual.client_id, expectedEntry.c_id);
        EXPECT_EQ(actual.client_seq, expectedEntry.c_seq);
    }
}

#include <gtest/gtest.h>

class LoggingFixture : public ::testing::Test {
protected:
    void SetUp() override { FLAGS_v = 6; }
};

/************************ Start getLogSuffixFromProposal tests ************************/
TEST_F(LoggingFixture, LogSuffixFromProposalFPlus1)
{
    // Test
    TestHistory hist;

    hist.push_back({10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}}});
    hist.push_back({10, "aaaa", 0, {{2, 2}, {1, 2}, {3, 2}}});
    hist.push_back({10, "aaaa", 0, {{2, 2}, {1, 2}, {3, 2}}});

    auto f = generateRepairProposal(1, hist);

    LogSuffix suffix;
    getLogSuffixFromProposal(*f, suffix);

    TestLog expectedLog{10, "aaaa", 0, {{2, 2}, {1, 2}, {3, 2}}};
    assertLogSuffixEq(suffix, expectedLog);
}

TEST_F(LoggingFixture, LogSuffixFromProposalScrambed)
{
    // Test
    TestHistory hist;

    hist.push_back({10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}}});
    hist.push_back({10, "aaaa", 0, {{2, 2}, {3, 2}, {1, 2}}});
    hist.push_back({10, "aaaa", 0, {{3, 2}, {1, 2}, {2, 2}}});

    auto f = generateRepairProposal(1, hist);

    LogSuffix suffix;
    getLogSuffixFromProposal(*f, suffix);

    TestLog expectedLog{10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}}};
    assertLogSuffixEq(suffix, expectedLog);
}

// TODO more cases with certs and such

/************************ Start applyLogSuffix tests ************************/

TEST_F(LoggingFixture, ApplyLogSuffix)
{
    // Test
    // TODO we would probably need to mock out verifaction of certs and stuff here
    TestLog curLog{10, "aaaa", 0, {{1, 2}, {3, 2}, {2, 2}}};
    TestLog newLog{10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}}};

    // Generate protocol log
    auto [log, app] = logFromTestLog(curLog);

    // Generate suffix for repair
    LogSuffix suffix;
    auto ret = suffixFromTestLog(newLog, suffix);

    std::shared_ptr<MockApplication> mockApp = std::dynamic_pointer_cast<MockApplication>(app);
    EXPECT_CALL(*mockApp, abort(12)).WillOnce(Return(true));

    applySuffix(suffix, log);

    assertLogEq(*log, newLog);
}

TEST_F(LoggingFixture, ApplyReplicaAhead)
{
    // Test
    // TODO we would probably need to mock out verifaction of certs and stuff here
    TestLog curLog{10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}, {4, 2}}};
    TestLog newLog{10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}}};

    // Generate protocol log
    auto [log, app] = logFromTestLog(curLog);

    // Generate suffix for repair
    LogSuffix suffix;
    auto ret = suffixFromTestLog(newLog, suffix);

    LOG(INFO) << "Before apply: " << *log;
    applySuffix(suffix, log);
    LOG(INFO) << "After apply: " << *log;
    assertLogEq(*log, newLog);
}

TEST_F(LoggingFixture, ApplyReplicaInserted)
{
    // Test
    // TODO we would probably need to mock out verifaction of certs and stuff here
    TestLog curLog{10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}, {4, 2}}};
    TestLog newLog{10, "aaaa", 0, {{2, 2}, {3, 2}, {4, 2}}};

    // Generate protocol log
    auto [log, app] = logFromTestLog(curLog);

    // Generate suffix for repair
    LogSuffix suffix;
    auto ret = suffixFromTestLog(newLog, suffix);

    applySuffix(suffix, log);
    assertLogEq(*log, newLog);
}

// TODO add case where checkpoitn is used

/************************ Start end to end tests ************************/

TEST_F(LoggingFixture, Cert)
{
    // Test
    TestHistory hist;
    TestLog curLog{10, "aaaa", 0, {{1, 2}}};

    hist.push_back(curLog);
    hist.push_back(curLog);
    // Logs with cert
    hist.push_back({10, "aaaa", 13, {{1, 3}, {2, 3}, {3, 3}}});
    TestLog expectedLog{10, "aaaa", 0, {{1, 3}, {2, 3}, {3, 3}, {1, 2}}};

    auto proposal = generateRepairProposal(1, hist);

    // Test suffix
    LogSuffix suffix;
    getLogSuffixFromProposal(*proposal, suffix);
    assertLogSuffixEq(suffix, expectedLog);

    // Test apply
    auto [log, app] = logFromTestLog(curLog);

    std::shared_ptr<MockApplication> mockApp = std::dynamic_pointer_cast<MockApplication>(app);
    EXPECT_CALL(*mockApp, abort(11)).WillOnce(Return(true));

    applySuffix(suffix, log);
    assertLogEq(*log, expectedLog);
}

TEST_F(LoggingFixture, Cert2)
{
    // Test
    TestHistory hist;
    TestLog curLog{10, "aaaa", 0, {{1, 2}, {2, 2}, {4, 2}, {3, 2}}};

    hist.push_back(curLog);
    hist.push_back(curLog);
    // Logs with certs
    hist.push_back({10, "aaaa", 13, {{1, 2}, {2, 2}, {3, 2}}});
    TestLog expectedLog{10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}, {4, 2}}};

    auto proposal = generateRepairProposal(1, hist);

    // Test suffix
    LogSuffix suffix;
    getLogSuffixFromProposal(*proposal, suffix);
    // assertLogSuffixEq(suffix, expectedLog); // Log suffix has duplicates, so we skip this

    // Test apply
    auto [log, app] = logFromTestLog(curLog);

    std::shared_ptr<MockApplication> mockApp = std::dynamic_pointer_cast<MockApplication>(app);
    EXPECT_CALL(*mockApp, abort(13)).WillOnce(Return(true));

    applySuffix(suffix, log);
    assertLogEq(*log, expectedLog);
}

TEST_F(LoggingFixture, Catchup)
{
    // TODO this fails because we don't have a way to apply the snapshot
    // TestHistory hist;
    // TestLog behindTestLog = {5, "bbbb", {}};
    // auto [behindLog, behindLogApp] = logFromTestLog(behindTestLog);

    // hist.push_back({10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}}});
    // hist.push_back({10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}}});
    // hist.push_back(behindTestLog);

    // auto proposal = generateRepairProposal(1, hist);

    // LogSuffix suffix;
    // getLogSuffixFromProposal(*proposal, suffix);

    // // Apply snapshot should be called
    // MockApplication *mockApp = static_cast<MockApplication *>(behindLogApp.get());
    // EXPECT_CALL(*mockApp, applySnapshot(_)).Times(AtLeast(1));

    // applySuffix(suffix, behindLog);

    // TestLog expectedLog{10, "aaaa", 0, {{1, 2}, {2, 2}, {3, 2}}};
    // assertLogEq(*behindLog, expectedLog);
}

TEST_F(LoggingFixture, CheckpointAhead)
{
    // TODO this fails because we don't have a way to apply the snapshot

    // // Test
    // // TODO we would probably need to mock out verifaction of certs and stuff here
    // TestLog ahead{10, "bbbb", 0, {{1, 2}, {2, 2}, {3, 2}, {4, 2}}};
    // TestLog behind{8, "aaaa", 0, {{0, 1}, {0, 2}, {1, 2}, {2, 2}, {3, 2}, {4, 2}}};

    // TestHistory hist;

    // hist.push_back(behind);
    // hist.push_back(behind);
    // hist.push_back(behind);

    // auto proposal = generateRepairProposal(1, hist);

    // // Generate suffix for repair
    // LogSuffix suffix;
    // getLogSuffixFromProposal(*proposal, suffix);

    // // Apply to Log
    // auto [aheadLog, app] = logFromTestLog(ahead);
    // LOG(INFO) << "Before apply: " << *aheadLog;
    // applySuffix(suffix, aheadLog);
    // LOG(INFO) << "After apply: " << *aheadLog;

    // // Generate protocol log
    // TestLog expected{10, "bbbb", 0, {{1, 2}, {2, 2}, {3, 2}, {4, 2}}};
    // assertLogEq(*aheadLog, expected);
}
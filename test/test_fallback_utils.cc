#include "lib/application.h"
#include "lib/fallback_utils.h"
#include "lib/log.h"
#include "lib/signature_provider.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <iostream>
#include <vector>

using namespace ::testing;

// TODO Mock out signatures
class MockSignatureProvider : public SignatureProvider {
public:
    MOCK_METHOD(bool, verify,
                (byte * data, uint32_t dataLen, byte *sig, uint32_t sigLen, const std::string &pubKeyType,
                 int pubKeyId));
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
    MOCK_METHOD(std::string, execute, (const std::string &serialized_request, const uint32_t execute_idx), (override));
    MOCK_METHOD(bool, commit, (uint32_t commit_idx), (override));
    MOCK_METHOD(bool, abort, (const uint32_t abort_idx), (override));
    MOCK_METHOD(std::string, getDigest, (uint32_t digest_idx), (override));
    MOCK_METHOD(std::string, takeSnapshot, (), (override));
    MOCK_METHOD(void, applySnapshot, (const std::string &snapshot), (override));

    MockApplication()
    {
        // Default behavior for methods to return true
        EXPECT_CALL(*this, execute(_, _)).WillRepeatedly(Return(""));
    }
};

//************************ Test case specification types ************************/
struct TestLogEntry {
    uint32_t c_id;
    uint32_t c_seq;

    bool cert = false;
    std::string req = "request";
};

struct TestLog {
    uint32_t startSeq;
    std::string logDigest;

    std::vector<TestLogEntry> entries;
};

typedef std::vector<TestLog> TestHistory;

/************************ Test case generation helpers ************************/
std::shared_ptr<Log> logFromTestLog(const TestLog &testLog)
{
    std::shared_ptr<Log> ret = std::make_shared<Log>(std::make_unique<MockApplication>());

    ret->nextSeq = testLog.startSeq + 1;

    ret->checkpoint.seq = testLog.startSeq;

    // Zero digest because provided TestLog.digest might be smaller
    memset(ret->checkpoint.logDigest, 0, SHA256_DIGEST_LENGTH);
    memcpy(ret->checkpoint.logDigest, testLog.logDigest.c_str(), testLog.logDigest.size());
    memset(ret->checkpoint.appDigest, 0, SHA256_DIGEST_LENGTH);

    // TODO make this cert "real"
    ret->checkpoint.cert = dombft::proto::Cert();

    for (const TestLogEntry &e : testLog.entries) {
        std::string res;
        ret->addEntry(e.c_id, e.c_seq, e.req, res);

        if (e.cert) {
            // TODO make this cert "real"
            ret->addCert(ret->nextSeq - 1, dombft::proto::Cert());
        }
    }

    return ret;
}

std::unique_ptr<dombft::proto::FallbackStart> suffixFromTestLog(const TestLog &testLog, LogSuffix &ret)
{
    auto log = logFromTestLog(testLog);

    auto logMsg = std::make_unique<dombft::proto::FallbackStart>();
    log->toProto(*logMsg);

    ret.checkpoint = &logMsg->checkpoint();

    for (auto &e : logMsg->log_entries()) {
        ret.entries.push_back(&e);
    }

    // We need to return logMsg here so the pointers in ret aren't invalid
    return logMsg;
}

// Create logs with requests of just raw integers, based on
// TODO make a checkpoint here too
std::unique_ptr<dombft::proto::FallbackProposal> generateFallbackProposal(int f, TestHistory &history)
{
    std::unique_ptr<dombft::proto::FallbackProposal> ret = std::make_unique<dombft::proto::FallbackProposal>();
    ret->set_instance(0);
    for (TestLog &t : history) {
        auto l = logFromTestLog(t);

        // This is a bit messy oops
        dombft::proto::FallbackStart logMsg;
        l->toProto(logMsg);

        (*ret->add_logs()) = logMsg;
        ret->add_signatures("");
    }

    int instanceNum = 5;
    ret->set_instance(instanceNum);
    ret->set_replica_id(instanceNum % (3 * f + 1));

    return ret;
}

/************************ Test assert helpers ************************/

void assertLogSuffixEq(const LogSuffix &suffix, const TestLog &expected)
{
    // TODO check checkpoint

    EXPECT_EQ(suffix.entries.size(), expected.entries.size());

    int n = expected.entries.size();
    for (int i = 0; i < n; i++) {
        const dombft::proto::LogEntry *actual = suffix.entries[i];
        const TestLogEntry &expectedEntry = expected.entries[i];

        EXPECT_EQ(actual->client_id(), expectedEntry.c_id);
        EXPECT_EQ(actual->client_seq(), expectedEntry.c_seq);

        // You might think we shuold do this (I did at first), but since
        // the log suffix jumbles the requests, these will not be right
        // EXPECT_EQ(actual->seq(), expected.startSeq + i + 1);
    }
}

void assertLogEq(Log &log, const TestLog &expected)
{
    // TODO check the checkpoints

    uint32_t startSeq = expected.startSeq;

    // Size of suffixes are the same
    ASSERT_EQ(expected.entries.size(), log.nextSeq - log.checkpoint.seq - 1);

    LOG(INFO) << log;

    int n = expected.entries.size();
    for (int i = 0; i < n; i++) {
        const TestLogEntry &expectedEntry = expected.entries[i];
        auto actual = log.getEntry(startSeq + i + 1);

        ASSERT_NE(actual, nullptr);

        EXPECT_EQ(actual->client_id, expectedEntry.c_id);
        EXPECT_EQ(actual->client_seq, expectedEntry.c_seq);
    }
}

/************************ Start getLogSuffixFromProposal tests ************************/
TEST(TestFallbackUtils, LogSuffixFromProposalFPlus1)
{
    // Test
    TestHistory hist;

    hist.push_back({10, "aaaa", {{1, 2}, {2, 2}, {3, 2}}});
    hist.push_back({10, "aaaa", {{2, 2}, {1, 2}, {3, 2}}});
    hist.push_back({10, "aaaa", {{2, 2}, {1, 2}, {3, 2}}});

    auto f = generateFallbackProposal(1, hist);

    LogSuffix suffix;
    getLogSuffixFromProposal(*f, suffix);

    TestLog expectedLog{10, "aaaa", {{2, 2}, {1, 2}, {3, 2}}};
    assertLogSuffixEq(suffix, expectedLog);
}

TEST(TestFallbackUtils, LogSuffixFromProposalScrambed)
{
    // Test
    TestHistory hist;

    hist.push_back({10, "aaaa", {{1, 2}, {2, 2}, {3, 2}}});
    hist.push_back({10, "aaaa", {{2, 2}, {3, 2}, {1, 2}}});
    hist.push_back({10, "aaaa", {{3, 2}, {1, 2}, {2, 2}}});

    auto f = generateFallbackProposal(1, hist);

    LogSuffix suffix;
    getLogSuffixFromProposal(*f, suffix);

    TestLog expectedLog{10, "aaaa", {{1, 2}, {2, 2}, {3, 2}}};
    assertLogSuffixEq(suffix, expectedLog);
}

// TODO more cases with certs and such

/************************ Start applyLogSuffix tests ************************/

TEST(TestFallbackUtils, ApplyLogSuffix)
{
    // Test
    // TODO we would probably need to mock out verifaction of certs and stuff here
    TestLog curLog{10, "aaaa", {{1, 2}, {3, 2}, {2, 2}}};
    TestLog newLog{10, "aaaa", {{1, 2}, {2, 2}, {3, 2}}};

    // Generate protocol log
    auto log = logFromTestLog(curLog);

    // Generate suffix for fallback
    LogSuffix suffix;
    auto ret = suffixFromTestLog(newLog, suffix);

    MockApplication *mockApp = static_cast<MockApplication *>(log->app_.get());
    EXPECT_CALL(*mockApp, abort(11)).WillOnce(Return(true));

    applySuffixToLog(suffix, log);

    assertLogEq(*log, newLog);
}

// TODO add case where checkpoitn is used

/************************ Start end to end tests ************************/

TEST(TestFallbackUtils, Catchup)
{
    // Test
    TestHistory hist;
    TestLog behindTestLog = {5, "bbbb", {}};
    auto behindLog = logFromTestLog(behindTestLog);

    hist.push_back({10, "aaaa", {{1, 2}, {2, 2}, {3, 2}}});
    hist.push_back({10, "aaaa", {{1, 2}, {2, 2}, {3, 2}}});
    hist.push_back(behindTestLog);

    auto f = generateFallbackProposal(1, hist);

    LogSuffix suffix;
    getLogSuffixFromProposal(*f, suffix);

    // Apply snapshot should be called
    MockApplication *mockApp = static_cast<MockApplication *>(behindLog->app_.get());
    EXPECT_CALL(*mockApp, applySnapshot(_)).Times(AtLeast(1));
    EXPECT_CALL(*mockApp, abort(10)).WillOnce(Return(true));

    applySuffixToLog(suffix, behindLog);

    TestLog expectedLog{10, "aaaa", {{1, 2}, {2, 2}, {3, 2}}};
    assertLogEq(*behindLog, expectedLog);
}
#include "lib/application.h"
#include "lib/fallback_utils.h"
#include "lib/log.h"
#include "lib/signature_provider.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <iostream>
#include <vector>

using ::testing::_;
using ::testing::Return;

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

// Types to specify Tests for slow Path
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

// Create logs with requests of just raw integers, based on
// TODO make a checkpoint here too
dombft::proto::FallbackProposal generateFallbackProposal(int f, TestHistory &history)
{
    dombft::proto::FallbackProposal ret;
    ret.set_instance(0);
    for (TestLog &log : history) {
        Log l(std::make_unique<MockApplication>());

        l.nextSeq = log.startSeq + 1;

        l.checkpoint.seq = log.startSeq;

        // Zero digest because provided TestLogDigest might be smaller
        memset(l.checkpoint.appDigest, 0, SHA256_DIGEST_LENGTH);
        memcpy(l.checkpoint.logDigest, log.logDigest.c_str(), log.logDigest.size());
        memset(l.checkpoint.appDigest, 0, SHA256_DIGEST_LENGTH);

        // TODO make this cert "real"
        l.checkpoint.cert = dombft::proto::Cert();

        for (TestLogEntry &e : log.entries) {
            std::string res;
            l.addEntry(e.c_id, e.c_seq, e.req, res);

            if (e.cert) {
                // TODO make this cert "real"
                l.addCert(l.nextSeq - 1, dombft::proto::Cert());
            }
        }

        // This is a bit messy oops
        dombft::proto::FallbackStart logMsg;
        l.toProto(logMsg);
        (*ret.add_logs()) = logMsg;
        ret.add_signatures("");
    }

    int instanceNum = 5;
    ret.set_instance(instanceNum);
    ret.set_replica_id(instanceNum % (3 * f + 1));
    return ret;
}

void assertLogSuffixEq(const LogSuffix &suffix, const TestLog &expected)
{

    EXPECT_EQ(suffix.entries.size(), expected.entries.size());

    int n = expected.entries.size();
    for (int i = 0; i < n; i++) {
        const dombft::proto::LogEntry *actual = suffix.entries[i];
        const TestLogEntry &expectedEntry = expected.entries[i];

        EXPECT_EQ(actual->client_id(), expectedEntry.c_id);
        EXPECT_EQ(actual->client_seq(), expectedEntry.c_seq);
        EXPECT_EQ(actual->seq(), expected.startSeq + i + 1);
    }
}

TEST(TestFallbackUtils, TestLogSuffix)
{
    // Test
    TestHistory hist;

    hist.push_back({10, "aaaa", {{1, 2}, {2, 2}, {3, 2}}});
    hist.push_back({10, "aaaa", {{2, 2}, {1, 2}, {3, 2}}});
    hist.push_back({10, "aaaa", {{2, 2}, {1, 2}, {3, 2}}});

    dombft::proto::FallbackProposal f = generateFallbackProposal(1, hist);

    LogSuffix suffix;
    getLogSuffixFromProposal(f, suffix);

    TestLog expectedLog{10, "aaaa", {{2, 2}, {1, 2}, {3, 2}}};
    assertLogSuffixEq(suffix, expectedLog);
}
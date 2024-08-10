struct LogCheckpoint {
    uint32_t seq = 0;
    // TODO shared ptr here so we don't duplicate it from certs.
    dombft::proto::Cert cert;
    byte logDigest[SHA256_DIGEST_LENGTH];
    byte appDigest[SHA256_DIGEST_LENGTH];

    std::map<uint32_t, dombft::proto::Commit> commitMessages;
    std::map<uint32_t, std::string> signatures;

    // Default constructor
    LogCheckpoint() = default;

    // Copy constructor
    LogCheckpoint(const LogCheckpoint &other)
        : seq(other.seq)
        , cert(other.cert)
        , commitMessages(other.commitMessages)
        , signatures(other.signatures)
    {
        std::memcpy(appDigest, other.appDigest, SHA256_DIGEST_LENGTH);
    }
};
#ifndef VERIFICATION_MANAGER_H
#define VERIFICATION_MANAGER_H

#include "proto/dombft_proto.pb.h"
#include "signature_provider.h"
// #include <cstddef>
// #include <set>
// #include <string>

// a class that handles all the verification stuff. 
class VerificationManager {
public:
    VerificationManager(uint32_t f, SignatureProvider& sigProvider);

    bool verifyCert(const dombft::proto::Cert& cert);

    bool verifyReply(const dombft::proto::Reply& reply, const std::string& signature);

    bool verifyFallbackTrigger(const dombft::proto::FallbackTrigger& trigger);


private:
    int f_; // Fault tolerance parameter
    SignatureProvider& sigProvider_;
};

#endif // VERIFICATION_MANAGER_H

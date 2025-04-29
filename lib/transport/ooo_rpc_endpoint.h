#ifndef OOO_RPC_ENDPOINT_H
#define OOO_RPC_ENDPOINT_H

#include "lib/transport/endpoint.h"
#include "lib/transport/ooo_service_implementation.h"

void OOORPCMsgHandler(const std::string &msg) { printf("The message is %s", msg.c_str()); }

using namespace OOO_BFT_RPC;
class OOORPCEndpoint : public Endpoint {
protected:
    // Maintain a proxy for each dest-node that this endpoint will SendPreparedMsgTo
    std::unordered_map<Address, OOOBFTProxy *> proxies_;

public:
    OOORPCEndpoint(
        const std::string &ip, const int port, const bool isMasterReceiver = false,
        const std::optional<Address> &loopbackAddr = std::nullopt
    );
    ~OOORPCEndpoint();

    // Unlike UDP which is connectionless, for RPC based on TCP,
    // we must actively set up the connection before we want to send data to it
    void ConnectTo(const Address &dstAddr);
    // Sends message in buffer
    virtual int SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr) override;

    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;
};

#endif
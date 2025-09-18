#include "ooo_rpc_endpoint.h"

OOORPCEndpoint::OOORPCEndpoint(const std::string &ip, const int port, const std::vector<Address> &targetAddrs)
    : myIP_(ip)
    , myListeningPort_(port)
    , targetAddrs_(targetAddrs)
{
    hdlrFunc_ = NULL;
    clientPoll_ = new rrr::PollMgr(2);
    serverPoll_ = new rrr::PollMgr(2);
    thrpool_ = new rrr::ThreadPool(8);
}

OOORPCEndpoint::~OOORPCEndpoint() {}

void OOORPCEndpoint::ConnectTo(const Address &dstAddr)
{
    if (proxies_.find(dstAddr) != proxies_.end()) {
        LOG(INFO) << "Already Connected " << dstAddr.ip_ << ":" << dstAddr.port_;
        return;
    }
    LOG(INFO) << "ConnectTo= " << dstAddr.ip_ << ":" << dstAddr.port_;
    int ret = -1;
    rrr::Client *client = new rrr::Client(clientPoll_);
    do {
        std::string addrString = dstAddr.ip_ + ":" + std::to_string(dstAddr.port_);
        ret = client->connect(addrString.c_str());
        if (ret == 0) {
            // success
            LOG(INFO) << "Sucessful Connection " << addrString;
        } else {
            sleep(1);
        }
    } while (ret != 0);

    OOOBFTProxy *proxy = new OOOBFTProxy(client);
    // Record this proxy info
    proxies_[dstAddr] = {client, proxy};
}

void OOORPCEndpoint::SetupServer()
{
    // Use different thread, do not block the caller
    serverThread_ = new std::thread([this] {
        std::string myServerAddr = myIP_ + ":" + std::to_string(myListeningPort_);
        LOG(INFO) << "Start Service serverAddr=" << myServerAddr;
        oooServer_->start(myServerAddr.c_str());
    });

    for (int i = 0; i < 4; i++) {
        replyThreads_.emplace_back([this] {
            rrr::DeferredReply *reply = nullptr;

            // TODO better handlign of exit and no busy wait
            // TODO how are the replies cleaned up?
            while (true) {
                if (replyQueue_.try_dequeue(reply)) {
                    reply->reply();
                }
            }
        });
    }
}

int OOORPCEndpoint::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr)
{
    assert(connected_);

    int ret = -1;
    // TODO: Need lock?
    if (hdr == nullptr) {
        hdr = (MessageHeader *) sendBuffer_;
    }

    auto iter = proxies_.find(dstAddr);
    if (iter == proxies_.end()) {
        LOG(ERROR) << "Cannot find the proxy for addr: " << dstAddr.ip() << ":" << dstAddr.port();
        return -1;
    }

    VLOG(2) << "SendPreparedMsgTo " << dstAddr.ip() << ":" << dstAddr.port_ << " msgType=" << (int) hdr->msgType
            << " msgLen=" << hdr->msgLen;

    OOOBFTProxy *proxy = iter->second.proxy_;

    OOOPrepareRequest req;
    req.senderIPInt_ = inet_addr(myIP_.c_str());
    req.senderPort_ = myListeningPort_;
    req.length_ = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;
    req.content_.resize(req.length_, '\0');
    memcpy(&(req.content_[0]), hdr, req.length_);
    VLOG(2) << "SendPreparedMsgTo " << dstAddr.ip() << ":" << dstAddr.port_ << " msgType=" << (int) hdr->msgType
            << " msgLen=" << hdr->msgLen;

    ret = proxy->SendOOOPrepareRequest(req);

    VLOG(2) << "SendPreparedMsgTo " << dstAddr.ip() << ":" << dstAddr.port_ << " msgType=" << (int) hdr->msgType
            << " msgLen=" << hdr->msgLen;
    return ret;
}

bool OOORPCEndpoint::RegisterMsgHandler(MessageHandlerFunc f)
{
    if (hdlrFunc_) {
        LOG(INFO) << "handler already set, do not overwrite it (causing data race)";
        return false;
    }
    hdlrFunc_ = f;
    oooHdl_ = [this](const OOOPrepareRequest &req, rrr::DeferredReply *deferred) {
        // Parse the string to MessageHeader style
        uint32_t senderIPInt = req.senderIPInt_;
        uint32_t senderPort = req.senderPort_;
        in_addr addr;
        addr.s_addr = senderIPInt;
        const char *ipStr = inet_ntoa(addr);
        Address senderAddr(ipStr, senderPort);

        std::string content = req.content_;
        MessageHeader *header = reinterpret_cast<MessageHeader *>(&(content[0]));
        assert(req.content_.length() == req.length_);
        assert(req.length_ >= sizeof(MessageHeader));
        byte *payload = reinterpret_cast<byte *>(&(content[sizeof(MessageHeader)]));

        // Queue the deferred reply for processing in another thread
        replyQueue_.enqueue(deferred);

        // Delegate to the hdlrFunc_;
        hdlrFunc_(header, payload, &senderAddr);
    };
    oooServer_ = new rrr::Server(serverPoll_, thrpool_);
    OOOBFTServiceImpl *oooService_ = new OOOBFTServiceImpl(oooHdl_);
    oooServer_->reg(oooService_);

    return true;
}

void OOORPCEndpoint::Connect()
{
    SetupServer();
    // Connect to my target receivers
    for (auto &targetAddr : targetAddrs_) {
        ConnectTo(targetAddr);
    }
    connected_ = true;
}

void OOORPCEndpoint::LoopRun()
{
    assert(connected_);

    ev_run(evLoop_, 0);
}

void OOORPCEndpoint::LoopBreak()
{
    // Destruct the RPC-related
    clientPoll_->release();
    serverPoll_->release();
    thrpool_->release();
    delete oooServer_;
}
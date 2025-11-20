#include "ooo_rpc_endpoint.h"

#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

OOORPCEndpoint::OOORPCEndpoint(
    const std::string &ip, const int port, const std::vector<Address> &targetAddrs, int numProxiesPerAddr
)
    : myIP_(ip)
    , myListeningPort_(port)
    , targetAddrs_(targetAddrs)
    , numProxiesPerAddr_(numProxiesPerAddr)
{
    hdlrFunc_ = NULL;
    clientPoll_ = new rrr::PollMgr(2);
    serverPoll_ = new rrr::PollMgr(2);
    thrpool_ = new rrr::ThreadPool(8);
}

OOORPCEndpoint::~OOORPCEndpoint()
{
    serverThread_->join();
    delete serverThread_;

    for (auto &t : replyThreads_) {
        t.join();
    }
}

void OOORPCEndpoint::ConnectTo(const Address &dstAddr)
{
    if (proxies_.find(dstAddr) != proxies_.end()) {
        LOG(INFO) << "Already Connected " << dstAddr.ip_ << ":" << dstAddr.port_;
        return;
    }
    LOG(INFO) << "ConnectTo= " << dstAddr.ip_ << ":" << dstAddr.port_;
    int ret = -1;

    for (int i = 0; i < numProxiesPerAddr_; i++) {
        // Create more proxies if needed
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
        proxies_[dstAddr].push_back({client, proxy});
    }
}

void OOORPCEndpoint::SetupServer()
{
    // Use different thread, do not block the caller
    serverThread_ = new std::thread([this] {
        std::string myServerAddr = myIP_ + ":" + std::to_string(myListeningPort_);
        LOG(INFO) << "Start Service serverAddr=" << myServerAddr;
        oooServer_->start(myServerAddr.c_str());
    });

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
        exit(1);
    }

    // TODO this is a bit of a hack, need a better way to select the proxy
    OOOBFTProxy *proxy = iter->second[rand() % iter->second.size()].proxy_;

    OOOPrepareRequest req;
    req.senderIPInt_ = inet_addr(myIP_.c_str());
    req.senderPort_ = myListeningPort_;
    req.length_ = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;
    req.content_.resize(req.length_, '\0');
    memcpy(&(req.content_[0]), hdr, req.length_);

    rrr::Future::safe_release(proxy->async_SendOOOPrepareRequest(req));
    VLOG(6) << "SendPreparedMsgTo " << dstAddr.ip() << ":" << dstAddr.port_ << " msgType=" << (int) hdr->msgType
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
    recvWatcher_.data = this;

    auto cb = [](struct ev_loop *loop, ev_async *w, int revents) {
        OOORPCEndpoint *ep = (OOORPCEndpoint *) w->data;
        std::pair<std::string, Address> item;

        while (ep->recvQueue_.try_dequeue(item)) {
            auto &[msg, addr] = item;
            size_t totalLen = msg.size();
            size_t offset = 0;

            while (totalLen - offset > sizeof(MessageHeader)) {
                byte *msgStart = reinterpret_cast<byte *>(msg.data()) + offset;
                MessageHeader *hdr = (MessageHeader *) (msgStart);
                size_t msgLen = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;

                if (offset + msgLen <= totalLen) {
                    ep->hdlrFunc_(hdr, msgStart + sizeof(MessageHeader), &addr);
                } else {
                    LOG(WARNING) << "Malformed message " << totalLen << " " << offset << " " << msgLen;
                }

                offset += msgLen;
            }
        }
    };

    // Register the event handler in the endpoint event loop
    ev_async_init(&recvWatcher_, cb);
    ev_async_start(evLoop_, &recvWatcher_);

    oooHdl_ = [this](const OOOPrepareRequest &req, rrr::DeferredReply *deferred) {
        // Parse the string to MessageHeader style
        uint32_t senderIPInt = req.senderIPInt_;
        uint32_t senderPort = req.senderPort_;
        in_addr addr;
        addr.s_addr = senderIPInt;
        const char *ipStr = inet_ntoa(addr);
        Address senderAddr(ipStr, senderPort);

        std::string content = req.content_;
        MessageHeader *header = reinterpret_cast<MessageHeader *>(content.data());
        assert(req.content_.length() == req.length_);
        assert(req.length_ >= sizeof(MessageHeader));

        // Check the deadline here for debugging

        if (header->msgType == DOM_REQUEST) {
            uint64_t now = GetMicrosecondTimestamp();
            dombft::proto::DOMRequest request;

            if (!request.ParseFromString(content.substr(sizeof(MessageHeader), header->msgLen))) {
                LOG(ERROR) << "Unable to parse DOM_REQUEST message";
                return;
            }

            int64_t recv_time = GetMicrosecondTimestamp();
            VLOG(3) << "RECEIVE RPC c_id=" << request.client_id() << " c_seq=" << request.client_seq()
                    << " Measured delay " << recv_time - request.send_time() << " usec";

            if (recv_time > request.deadline()) {
                request.set_late(true);
                VLOG(2) << "Request " << request.client_id() << ", " << request.client_seq() << " is late at RPC by "
                        << recv_time - request.deadline() << "us";
            }
        }
        // Queue the deferred reply for processing in another thread
        replyQueue_.enqueue(deferred);

        // Delegate to the hdlrFunc_;
        recvQueue_.enqueue(std::pair<std::string, Address>{content, senderAddr});

        ev_async_send(evLoop_, &recvWatcher_);
    };

    // register the handle to the RPC server
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

    LOG(INFO) << "OOORPCEndpoint done connecting to all target addresses";
}

void OOORPCEndpoint::LoopRun()
{
    assert(connected_);

    ev_run(evLoop_, 0);
}

void OOORPCEndpoint::LoopBreak()
{
    Endpoint::LoopBreak();
    // Destruct the RPC-related
    delete oooServer_;
}
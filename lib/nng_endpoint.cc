#include "lib/nng_endpoint.h"

NngMessageHandler::NngMessageHandler(MessageHandlerFunc msghdl, void *ctx)
    : MessageHandler(msghdl, ctx)
{
    ev_init(evWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents)
            {
        NngMessageHandler* m = (NngMessageHandler*)(w->data);
        socklen_t sockLen = sizeof(struct sockaddr_in);
        
        int msgLen = recvfrom(w->fd, m->buffer_, UDP_BUFFER_SIZE, 0,
                                (struct sockaddr*)(&(m->sender_.addr_)), &sockLen);
        if (msgLen > 0 && (uint32_t)msgLen > sizeof(MessageHeader)) 
        {
            MessageHeader* msgHeader = (MessageHeader*)(void*)(m->buffer_);
            if (sizeof(MessageHeader) + msgHeader->msgLen + msgHeader->sigLen >= (uint32_t)msgLen) 
            {
                m->msgHandler_(msgHeader, m->buffer_ + sizeof(MessageHeader),
                                &(m->sender_), m->context_);
            }
        } });
}

NngMessageHandler::~NngMessageHandler() {}

UDPEndpoint::UDPEndpoint(const std::string &ip, const int port,
                         const bool isMasterReceiver)
    : Endpoint(isMasterReceiver), msgHandler_(NULL)
{
    fd_ = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0)
    {
        LOG(ERROR) << "Receiver Fd fail ";
        return;
    }
    // Set Non-Blocking
    int status = fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    if (status < 0)
    {
        LOG(ERROR) << " Set NonBlocking Fail";
    }
    if (ip == "" || port < 0)
    {
        return;
    }

    bound_ = true;
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    // Bind socket to Address
    int bindRet = bind(fd_, (struct sockaddr *)&addr, sizeof(addr));
    if (bindRet != 0)
    {
        LOG(ERROR) << "bind error\t" << bindRet << "\t port=" << port;
        return;
    }
}

UDPEndpoint::~UDPEndpoint() {}

int UDPEndpoint::SendPreparedMsgTo(const Address &dstAddr, bool reuseBuffer)
{
    MessageHeader *hdr = (MessageHeader *)buffer_;

    if (!bufReady_)
    {
        LOG(ERROR) << "SendPreparedMsgTo called while bufferReady_ = false, make "
                   << "sure you call PrepareMsg() or PrepareProtoMsg() first ";
        return -1;
    }

    int ret = sendto(fd_, buffer_, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0,
                     (struct sockaddr *)(&(dstAddr.addr_)), sizeof(sockaddr_in));
    if (ret < 0)
    {
        VLOG(1) << "\tSend Fail ret =" << ret;
    }

    if (!reuseBuffer)
        bufReady_ = false;

    return ret;
}

MessageHeader *UDPEndpoint::PrepareMsg(const byte *msg,
                                       u_int32_t msgLen,
                                       byte msgType)
{
    if (bufReady_)
    {
        LOG(ERROR) << "PrepareMsg called while bufferReady_ = true, make "
                   << "sure you call SendPreparedMessage() after PrepareMsg() is called again ";
        return nullptr;
    }

    MessageHeader *hdr = (MessageHeader *)buffer_;
    hdr->msgType = msgType;
    hdr->msgLen = msgLen;
    hdr->sigLen = 0;
    if (msgLen + sizeof(MessageHeader) > UDP_BUFFER_SIZE)
    {
        LOG(ERROR) << "Msg too large " << (uint32_t)msgType
                   << "\t length=" << msgLen;
        return nullptr;
    }

    memcpy(buffer_ + sizeof(MessageHeader), msg,
           hdr->msgLen);

    bufReady_ = true;
    return hdr;
}

MessageHeader *UDPEndpoint::PrepareProtoMsg(const google::protobuf::Message &msg,
                                            byte msgType)
{
    std::string serializedString = msg.SerializeAsString();
    uint32_t msgLen = serializedString.length();
    if (msgLen > 0)
    {
        return PrepareMsg((const byte *)serializedString.c_str(), msgLen, msgType);
    }
    return nullptr;
}

void UDPEndpoint::setBufReady(bool bufReady) 
{ 
    bufReady_ = bufReady;
}

bool UDPEndpoint::RegisterMsgHandler(MessageHandler *msgHdl)
{
    UDPMessageHandler *udpMsgHdl = (UDPMessageHandler *)msgHdl;
    if (!bound_)
    {
        LOG(ERROR) << "Endpoint is not bound!";
        return false;
    }
    if (evLoop_ == NULL)
    {
        LOG(ERROR) << "No evLoop!";
        return false;
    }
    if (isMsgHandlerRegistered(msgHdl))
    {
        LOG(ERROR) << "This msgHdl has already been registered";
        return false;
    }
    msgHandler_ = udpMsgHdl;
    ev_io_set(udpMsgHdl->evWatcher_, fd_, EV_READ);
    ev_io_start(evLoop_, udpMsgHdl->evWatcher_);

    return true;
}

bool UDPEndpoint::UnRegisterMsgHandler(MessageHandler *msgHdl)
{
    UDPMessageHandler *udpMsgHdl = (UDPMessageHandler *)msgHdl;
    if (evLoop_ == NULL)
    {
        LOG(ERROR) << "No evLoop!";
        return false;
    }
    if (!isMsgHandlerRegistered(udpMsgHdl))
    {
        LOG(ERROR) << "The handler has not been registered ";
        return false;
    }
    ev_io_stop(evLoop_, udpMsgHdl->evWatcher_);
    msgHandler_ = NULL;
    return true;
}

bool UDPEndpoint::isMsgHandlerRegistered(MessageHandler *msgHdl)
{
    return (UDPMessageHandler *)msgHdl == msgHandler_;
}

void UDPEndpoint::UnRegisterAllMsgHandlers()
{
    ev_io_stop(evLoop_, msgHandler_->evWatcher_);
    msgHandler_ = NULL;
}

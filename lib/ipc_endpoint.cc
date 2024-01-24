#include "lib/ipc_endpoint.h"

IPCEndpoint::IPCEndpoint(const std::string& ipc_addr,
                        const bool isMasterReceiver)
    : Endpoint("", 0, isMasterReceiver), msgHandler_(NULL) 
{
  // TODO: setup ipc datagram socket

}

IPCEndpoint::~IPCEndpoint() {}

// This is basically the same as UDP, could be consolidated...
int IPCEndpoint::SendMsgTo(const Address& dstAddr,
                const char* msg,
                u_int32_t msgLen,
                char msgType) 
{
  char buffer[UDP_BUFFER_SIZE];
  MessageHeader* msgHdr = (MessageHeader*)(void*)buffer;
  msgHdr->msgType = msgType;
  msgHdr->msgLen = msgLen;
  if (msgLen + sizeof(MessageHeader) > UDP_BUFFER_SIZE) {
    LOG(ERROR) << "Msg too large " << (uint32_t)msgType
               << "\t length=" << msgLen;
    return -1;
  }
  
  memcpy(buffer + sizeof(MessageHeader), msg,
          msgHdr->msgLen);
  int ret = sendto(fd_, buffer, msgHdr->msgLen + sizeof(MessageHeader), 0,
                    (struct sockaddr*)(&(dstAddr.addr_)), sizeof(sockaddr_in));
  if (ret < 0) {
    VLOG(1) << pthread_self() << "\tSend Fail ret =" << ret;
  }
  return ret;
}

int IPCEndpoint::SendProtoMsgTo(const Address& dstAddr,
                const google::protobuf::Message& msg,
                char msgType) 
{
  std::string serializedString = msg.SerializeAsString();
  uint32_t msgLen = serializedString.length();
  if (msgLen > 0) {
    SendMsgTo(dstAddr, serializedString.c_str(), msgLen, msgType);
  }
  return -1;
}


bool IPCEndpoint::RegisterMsgHandler(MessageHandler* msgHdl) {
  IPCMsgHandler* ipcMsgHdl = (IPCMsgHandler*)msgHdl;
  if (evLoop_ == NULL) {
    LOG(ERROR) << "No evLoop!";
    return false;
  }
  if (isMsgHandlerRegistered(msgHdl)) {
    LOG(ERROR) << "This msgHdl has already been registered";
    return false;
  }

  msgHandler_ = ipcMsgHdl;
  ev_io_set(ipcMsgHdl->evWatcher_, fd_, EV_READ);
  ev_io_start(evLoop_, ipcMsgHdl->evWatcher_);

  return true;
}

bool IPCEndpoint::UnRegisterMsgHandler(MessageHandler* msgHdl) {
  UDPMsgHandler* udpMsgHdl = (UDPMsgHandler*)msgHdl;
  if (evLoop_ == NULL) {
    LOG(ERROR) << "No evLoop!";
    return false;
  }
  if (!isMsgHandlerRegistered(udpMsgHdl)) {
    LOG(ERROR) << "The handler has not been registered ";
    return false;
  }
  ev_io_stop(evLoop_, udpMsgHdl->evWatcher_);
  msgHandler_ = NULL;
  return true;
}

bool IPCEndpoint::isMsgHandlerRegistered(MessageHandler* msgHdl) {
  return (IPCMsgHandler*)msgHdl == msgHandler_;
}

void IPCEndpoint::UnRegisterAllMsgHandlers() {
  ev_io_stop(evLoop_, msgHandler_->evWatcher_);
  msgHandler_ = NULL;
}

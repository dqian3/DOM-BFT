

#ifndef COMMON_STRUCT_H
#define COMMON_STRUCT_H
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <string>
#include <vector>

// TODO what about padding on the msgType? Might want to check that.

/**
 * When the proto message has been serialized and is about to be sent by the
 * endpoint, MessageHeader is prepended to the head of the proto message (refer
 * to SendMsgTo in udp_socket_endpoint.h), which describes the type of proto
 * message and its length. In this way, when the receiver endpoint receives the
 * message, it can know the type and length of the proto message, then it can
 * choose the proper way to deserialize it.
 */
struct MessageHeader {
  uint32_t msgLen;
  char msgType;
  MessageHeader(const uint32_t l, const char t) : msgLen(l), msgType(t)  {}
};

/**
 * Similar to the MessageHeader, a header sent prepended to the serialized 
 * proto message. However, we include a sigLen field that indicates the
 * len of a signature, which comes after the protobuf. The signature also
 * includes the msgType.
 */
struct SignedMessageHeader {
  uint32_t msgLen;
  uint32_t sigLen;

  char msgType; // Included in signature!
  SignedMessageHeader(const uint32_t ml, const uint32_t sl, const char t) 
    : msgLen(ml), sigLen(sl), msgType(t)  {}
};


#endif
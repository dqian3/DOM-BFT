

#ifndef COMMON_STRUCT_H
#define COMMON_STRUCT_H
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <string>
#include <vector>

/**
 * Nezha relies on proto messages to communicate.
 * When the proto message has been serialized and is about to be sent by the
 * endpoint, MessageHeader is prepended to the head of the proto message (refer
 * to SendMsgTo in udp_socket_endpoint.h), which describes the type of proto
 * message and its length. In this way, when the receiver endpoint receives the
 * message, it can know the type and length of the proto message, then it can
 * choose the proper way to deserialize it.
 */
struct MessageHeader {
  char msgType;
  uint32_t msgLen;
  MessageHeader(const char t, const uint32_t l) : msgType(t), msgLen(l) {}
};

#endif
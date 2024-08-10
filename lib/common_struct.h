

#ifndef COMMON_STRUCT_H
#define COMMON_STRUCT_H
#include <cstring>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#define SIGNED_FLAG

#define SEND_BUFFER_SIZE (10000)
#define UDP_BUFFER_SIZE  (1024)
#define NNG_BUFFER_SIZE  (10000)
#define IPC_BUFFER_SIZE  (1024)

typedef unsigned char byte;

/**
 * When the message has been serialized and is about to be sent by the
 * endpoint, MessageHeader is prepended to the head of message which
 * describes the type of message and its length. In this way, when the
 * receiver endpoint receives the message, it can know the type and length
 * of the proto message, then it can choose the proper way to deserialize it.
 *
 * A signature can be optinally appended to the end of the message as well, with
 * anohter field reporting its len. sigLen = 0 corresponds to no signature
 */
struct MessageHeader {
    byte msgType;
    uint32_t msgLen;
    uint32_t sigLen;
    MessageHeader(const byte t, const uint32_t l, const uint32_t sl)
        : msgType(t)
        , msgLen(l)
        , sigLen(sl) {};
};

#endif
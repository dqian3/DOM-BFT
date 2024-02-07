

#ifndef COMMON_STRUCT_H
#define COMMON_STRUCT_H
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <string>
#include <vector>

#define SIGNED_FLAG

#define UDP_BUFFER_SIZE (1024)
#define IPC_BUFFER_SIZE (1024)

// TODO what about padding on the msgType? Might want to check that.

/**
 * When the message has been serialized and is about to be sent by the
 * endpoint, MessageHeader is prepended to the head of message which
 * describes the type of message and its length. In this way, when the
 * receiver endpoint receives the message, it can know the type and length
 * of the proto message, then it can choose the proper way to deserialize it.
 */
struct MessageHeader
{
    char msgType;
    uint32_t msgLen;
    MessageHeader(const uint32_t l, const char t) : msgType(t), msgLen(l) {}
};

/**
 * Encapsulated within a message header, dataLen is the length of the encoded
 * proto message, sigLen is the len of the signature. This header is present
 * depending on msgType above
 */
// TODO just add msgType to the top, so no need for encapsulation
struct SignedMessageHeader
{
    uint32_t dataLen;
    uint32_t sigLen;
    SignedMessageHeader(const uint32_t dl, const uint32_t sl) : dataLen(dl), sigLen(sl) {}
};

#endif

#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <functional>

#include "lib/common.h"

/**
 * MessageHeader* describes the type and length of the received message
 * byte* is the payload of the message
 * Address* is the address of the sender
 */
typedef std::function<void(MessageHeader *, byte *, Address *)> MessageHandlerFunc;

#endif
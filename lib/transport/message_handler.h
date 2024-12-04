
#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <functional>

#include "lib/common.h"

/**
 * MessageHeader* describes the type and length of the received message
 *      It is followed immediately by the payload/body of the message
 *      TODO is enforce this with some sort of slice instead of a raw ptr
 * Address* is the address of the sender
 */
typedef std::function<void(MessageHeader *, const Address &)> MessageHandlerFunc;

#endif
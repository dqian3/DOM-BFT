
#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

/**
 * MessageHeader* describes the type and length of the received message
 * byte* is the payload of the message
 * Address* is the address of the sender
 * void* points to the (optional) context that is needed by the callback
 * function, such as a this pointer
 */
typedef std::function<void(MessageHeader *, byte *, Address *, void *)>
    MessageHandlerFunc;

#endif
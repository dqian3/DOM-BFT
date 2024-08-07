#ifndef MESSAGE_TYPE_H
#define MESSAGE_TYPE_H

/**
 * The message types are defined according to the proto files and the
 * information will be included in each message to facilitate
 * serialize/deserialize proto messages
 */
enum MessageType {
    // DOM Sending Messages
    CLIENT_REQUEST = 1,
    DOM_REQUEST = 2,
    MEASUREMENT_REPLY = 3,

    // Fast/normal path messages
    FAST_REPLY = 4,
    REPLY = 5,
    CERT = 6,
    CERT_REPLY = 7,

    COMMIT = 8,

    FALLBACK_TRIGGER = 9,
    FALLBACK_START = 10,
    FALLBACK_PROPOSAL = 11,
    FALLBACK_EXECUTED = 12,
    
    DUMMY_PROTO = 13
};

#endif
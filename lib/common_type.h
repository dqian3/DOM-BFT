#ifndef NEZHA_COMMON_TYPE_H
#define NEZHA_COMMON_TYPE_H

/**
 * The message types are defined according to the proto files and the
 * information will be included in each message to facilitate
 * serialize/deserialize proto messages
 */
enum MessageType {
    CLIENT_REQUEST = 1,
    DOM_REQUEST = 2,
    MEASUREMENT_REPLY = 3,
};

#endif
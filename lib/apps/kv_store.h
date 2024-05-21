#ifndef KV_STORE_H
#define KV_STORE_H

#include "lib/application.h"

#include <unordered_map>
#include <string>

// TODO instead of requests and responses being raw bytes, have 
// request and response types that can be serialized/unserialized.
class KVStore : Application{

    std::unordered_map<std::string, std::string> data;


    virtual std::unique_ptr<byte *> execute(const byte *request);

};

#endif
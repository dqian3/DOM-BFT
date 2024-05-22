#ifndef APPLICATION_H
#define APPLICATION_H

#include <memory>

#include "common_struct.h"

#include <google/protobuf/message.h>

// Originally had some custom classes here, but this is easier lol
typedef google::protobuf::Message AppRequest;
typedef google::protobuf::Message AppResponse;

class Application
{

public:
    virtual std::unique_ptr<AppResponse> execute(const AppRequest* request) = 0;

    // virtual uint32_t takeSnapshot() = 0;
    // virtual bool restoreSnapshot() = 0;

};

#endif
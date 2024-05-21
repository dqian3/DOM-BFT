#ifndef APPLICATION_H
#define APPLICATION_H

#include <memory>

#include "common_struct.h"

class Application
{

public:
    virtual std::unique_ptr<byte *> execute(const byte *request) = 0;

    // virtual uint32_t takeSnapshot() = 0;
    // virtual bool restoreSnapshot() = 0;

};

#endif
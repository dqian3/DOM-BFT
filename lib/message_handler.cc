#include "message_handler.h"


// Even though this was pure virtual above, we need define it here to cleanup ev_io
// We need to make it pure virtual because it segfaults if it is initialized by itself
// which took me a few hours to figure out -Dan
MessageHandler::~MessageHandler() {delete evWatcher_;}

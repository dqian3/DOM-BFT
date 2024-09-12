#include "lib/asyncLogger.h"

int main() {
    int receiverPort = 8080;

    LOG(INFO) << "receiverPort=" << receiverPort;
    LOG(INFO) << "Another log with multiple arguments: " << 1 << ", " << 2 << ", " << 3;

    VLOG(3) << "This is a verbose log";

    globalLogger_->flush();

    return 0;
}
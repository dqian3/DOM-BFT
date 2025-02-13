#ifndef APP_SNAPSHOT_STORE_H
#define APP_SNAPSHOT_STORE_H

#include <string>

#include "lib/application.h"

class AppSnapshotStore {

    AppSnapshotStore() {}

    void updateSnapshot(AppSnapshot &&snapshot);

    std::string getSnapshotForIdx(uint32_t fromIdx);

private:
    // Corresponds to stable checkpoint
    std::string stableSnapshot_;
    uint32_t stableSnapshotIdx_;

    std::map<uint32_t.std::string> deltas_;
};

#endif
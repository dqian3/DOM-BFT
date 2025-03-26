#ifndef APP_SNAPSHOT_STORE_H
#define APP_SNAPSHOT_STORE_H

#include <string>

#include "lib/application.h"

class AppSnapshotStore {

public:
    AppSnapshotStore() {}

    void addSnapshot(std::string &&snapshot, uint32_t idx);
    // void addSnapshotWithDelta(AppSnapshot &&snapshot, uint32_t idx);

    std::string getSnapshot();
    uint32_t getSnapshotIdx();
    // std::string getDeltasFromIdx(uint32_t fromIdx);

private:
    // Corresponds to stable checkpoint
    std::string stableSnapshot_;
    uint32_t stableSnapshotIdx_;

    // Enocded deltas and the indices they work from
    // std::map<uint32_t, std::string> deltas_;
};

#endif
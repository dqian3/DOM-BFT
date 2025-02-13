#include "lib/app_snapshot_store.h"

void AppSnapshotStore::addSnapshot(std::string &&snapshot, uint32_t idx)
{
    // TODO use delta information
    stableSnapshot_ = std::move(snapshot);
    stableSnapshotIdx_ = idx;
}

std::string AppSnapshotStore::getSnapshot() { return stableSnapshot_; }

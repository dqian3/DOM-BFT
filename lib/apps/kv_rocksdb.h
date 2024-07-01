#ifndef KV_STORE_H
#define KV_STORE_H

#include <rocksdb/db.h>
#include <string>
#include <iostream> 

class KVStore {
public:
    KVStore(const std::string &dbPath);
    ~KVStore();

    bool set(const std::string &key, const std::string &value);
    bool get(const std::string &key, std::string *value);
    bool del(const std::string &key);
    const rocksdb::Snapshot *snapshot();
    void rollback(const rocksdb::Snapshot *snapshot);
    // bool commit();
private:
    rocksdb::DB *db_ = nullptr;
};

#endif // KV_STORE_H
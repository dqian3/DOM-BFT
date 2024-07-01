#include <iostream>
#include <cassert>
#include "lib/apps/kv_rocksdb.h"

int main() {
    // Step 1: Initialize the KVStore
    std::string dbPath = "./testdb";
    KVStore kvStore(dbPath);

    // Step 2: Set a Key-Value Pair
    std::string key = "testKey";
    std::string value = "Hello, NYU!";
    kvStore.set(key, value);

    // Step 3: Get the Value
    std::string retrievedValue;
    bool found = kvStore.get(key, &retrievedValue);
    if (found) {
        std::cout << "Retrieved Value: " << retrievedValue << std::endl;
    } else {
        std::cerr << "Key not found." << std::endl;
    }

    // Optional: Assert to ensure the value matches
    assert(retrievedValue == value);

    // Step 4: Delete the Key (Optional)
    kvStore.del(key);

    // Verify deletion
    found = kvStore.get(key, &retrievedValue);
    assert(!found);

    // Step 5: Close the KVStore (if necessary, depending on implementation)
    // kvStore.close(); // Uncomment if your implementation requires explicit close

    return 0;
}
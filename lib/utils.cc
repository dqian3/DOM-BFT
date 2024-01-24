#include "lib/utils.h"

// Get Current Microsecond Timestamp
uint64_t GetMicrosecondTimestamp() {
  auto tse = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(tse).count();
}

#ifndef JOURNAL_TYPES_H_
#define JOURNAL_TYPES_H_

#include <cstdint>

// Plain enums shared by the journal store, its view model and the host-side preview (no
// ESP-IDF dependency).
namespace journal_service {

enum class ItemType : uint8_t {
    kTask = 0,
    kNote,
    kEvent,
};

enum class ItemStatus : uint8_t {
    kOpen = 0,
    kDone,
    kCancelled,
};

}  // namespace journal_service

#endif  // JOURNAL_TYPES_H_

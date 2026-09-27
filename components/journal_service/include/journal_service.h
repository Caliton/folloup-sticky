#ifndef JOURNAL_SERVICE_H_
#define JOURNAL_SERVICE_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "journal_period.h"
#include "journal_types.h"

// Bullet-journal items (tasks, notes and events filed under a year/month/week/day period).
// The store is a JSON-lines file on the SD card (`<mount>/journal/items.jsonl`) mirrored in a
// PSRAM cache; see docs/journal.md for the model and the sync contract with the web app.
namespace journal_service {

// The item cache can hold hundreds of short strings. Pin it to PSRAM explicitly: malloc would
// put anything under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL bytes in scarce internal RAM.
template <typename T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() noexcept = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept
    {
    }
    T* allocate(size_t count)
    {
        void* memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory == nullptr) {
            memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_8BIT);
        }
        if (memory == nullptr) {
            // -fno-exceptions: there is no bad_alloc to throw; fail loudly.
            abort();
        }
        return static_cast<T*>(memory);
    }
    void deallocate(T* pointer, size_t) noexcept { heap_caps_free(pointer); }
    template <typename U>
    bool operator==(const PsramAllocator<U>&) const noexcept
    {
        return true;
    }
    template <typename U>
    bool operator!=(const PsramAllocator<U>&) const noexcept
    {
        return false;
    }
};

using PString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

struct Item {
    PString id = {};
    ItemType type = ItemType::kTask;
    PString text = {};
    // Where the item lives now, and the widest period it was first filed under (so a month
    // view can still show a task that was pulled into a week, with a migration mark).
    PString period = {};
    PString planned = {};
    ItemStatus status = ItemStatus::kOpen;
    PString recording_id = {};
    PString origin = {};
    int64_t created_at = 0;
    int64_t updated_at = 0;
    bool deleted = false;
    // Changed locally since the last successful push to the cloud.
    bool dirty = false;
};

using ItemList = std::vector<Item, PsramAllocator<Item>>;

struct Snapshot {
    bool loaded = false;
    int item_count = 0;
    int dirty_count = 0;
    uint32_t revision = 0;
};

struct Event {
    uint32_t revision = 0;
    // False when the change came from the cloud (the sync service does not need to push it).
    bool local_change = true;
};

using Listener = void (*)(const Event& event, void* context);

constexpr size_t kMaxTextLength = 500;

esp_err_t Init();
// Up to four listeners (UI refresh, cloud sync, ...). Called after every committed change,
// outside the service lock, on the task that made the change.
bool AddListener(Listener listener, void* context);
Snapshot GetSnapshot();
// Drop the cache and re-read the SD card (after a format, OTG session or remount).
void Reload();

// Non-deleted items (in file order). Loads the store on first use; SD I/O on the caller.
ItemList ListItems();
bool FindItem(const std::string& id, Item* item);

// Mutations persist immediately and return false on a bad argument or a failed SD write
// (the cache is then rolled back). `period` must be a valid journal_period key.
std::string CreateItem(ItemType type, const std::string& text, const std::string& period,
                       const std::string& recording_id = {});
bool SetStatus(const std::string& id, ItemStatus status);
// Move to another period (pull into week/day, postpone, send back). `planned` is untouched
// unless the new period is wider than it.
bool MoveTo(const std::string& id, const std::string& period);
bool SetType(const std::string& id, ItemType type);
bool SetText(const std::string& id, const std::string& text);
bool Delete(const std::string& id);

// --- Cloud sync support -------------------------------------------------------------------
// Everything with dirty=true, including soft-deleted tombstones.
ItemList DirtyItems();
// Clears the dirty flag if the item was not modified again after `updated_at` was pushed.
void MarkPushed(const std::string& id, int64_t updated_at);
// Last-writer-wins merge of a document pulled from the cloud. Returns true when it changed
// the local store. Notifies listeners with local_change=false once per batch (see
// EndRemoteBatch) to avoid a repaint per document.
bool ApplyRemote(const Item& remote);
void EndRemoteBatch();

const char* TypeName(ItemType type);       // "task" | "note" | "event"
const char* StatusName(ItemStatus status);  // "open" | "done" | "cancelled"
bool ParseType(const std::string& text, ItemType* type);
bool ParseStatus(const std::string& text, ItemStatus* status);
const char* TypeLabel(ItemType type);  // "Tarefa" | "Nota" | "Evento"

// Open items whose period already ended (the BuJo review queue).
bool IsPending(const Item& item, const journal_period::Date& today);

}  // namespace journal_service

#endif  // JOURNAL_SERVICE_H_

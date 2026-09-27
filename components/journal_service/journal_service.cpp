#include "journal_service.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "storage_service.h"

namespace journal_service {
namespace {

constexpr const char* kTag = "Journal";
constexpr const char* kDirectory = "journal";
constexpr const char* kItemsFile = "items.jsonl";
constexpr const char* kItemsTempFile = "items.tmp";
constexpr int64_t kMinValidEpoch = 1704067200;  // 2024-01-01 UTC
// Synced tombstones are kept this long so a device that was offline still learns about them.
constexpr int64_t kTombstoneRetentionSeconds = 30LL * 24 * 60 * 60;
constexpr size_t kMaxFileBytes = 2 * 1024 * 1024;
constexpr size_t kMaxListeners = 4;

struct ListenerSlot {
    Listener listener = nullptr;
    void* context = nullptr;
};

std::mutex s_mutex;
ItemList s_items;
bool s_loaded = false;
uint32_t s_revision = 0;
bool s_remote_batch_changed = false;
std::array<ListenerSlot, kMaxListeners> s_listeners = {};

int64_t NowSeconds()
{
    const int64_t now = static_cast<int64_t>(time(nullptr));
    return now >= kMinValidEpoch ? now : 0;
}

std::string JoinPath(const char* left, const char* right)
{
    std::string path = left != nullptr ? left : "";
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    return path + right;
}

PString ToP(const std::string& text)
{
    return PString(text.data(), text.size());
}

std::string ToStd(const PString& text)
{
    return std::string(text.data(), text.size());
}

std::string GenerateId()
{
    char buffer[32] = {};
    const uint32_t high = esp_random();
    const uint32_t low = esp_random();
    std::snprintf(buffer, sizeof(buffer), "d%04x%08x", static_cast<unsigned>(high & 0xFFFF),
                  static_cast<unsigned>(low));
    return buffer;
}

// Keeps the text on one line and within the contract's 500 characters (without splitting a
// UTF-8 sequence).
std::string NormalizeText(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        result += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    const size_t first = result.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = result.find_last_not_of(' ');
    result = result.substr(first, last - first + 1);
    if (result.size() > kMaxTextLength) {
        size_t cut = kMaxTextLength;
        while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        result.resize(cut);
    }
    return result;
}

std::string JsonString(cJSON* object, const char* name)
{
    cJSON* field = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(field) && field->valuestring != nullptr ? field->valuestring
                                                                  : std::string();
}

int64_t JsonInt(cJSON* object, const char* name)
{
    cJSON* field = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(field) ? static_cast<int64_t>(field->valuedouble) : 0;
}

bool ParseLine(const char* line, size_t length, Item* item)
{
    cJSON* root = cJSON_ParseWithLength(line, length);
    if (root == nullptr) {
        return false;
    }
    Item parsed = {};
    parsed.id = ToP(JsonString(root, "id"));
    parsed.text = ToP(JsonString(root, "text"));
    parsed.period = ToP(JsonString(root, "period"));
    parsed.planned = ToP(JsonString(root, "planned"));
    parsed.recording_id = ToP(JsonString(root, "recordingId"));
    parsed.origin = ToP(JsonString(root, "origin"));
    parsed.created_at = JsonInt(root, "createdAt");
    parsed.updated_at = JsonInt(root, "updatedAt");
    parsed.deleted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "deleted"));
    parsed.dirty = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "dirty"));
    const bool type_ok = ParseType(JsonString(root, "type"), &parsed.type);
    const bool status_ok = ParseStatus(JsonString(root, "status"), &parsed.status);
    cJSON_Delete(root);
    if (parsed.id.empty() || !type_ok || !status_ok ||
        !journal_period::IsValidKey(ToStd(parsed.period))) {
        return false;
    }
    if (!journal_period::IsValidKey(ToStd(parsed.planned))) {
        parsed.planned = parsed.period;
    }
    *item = std::move(parsed);
    return true;
}

// One JSON object per line. The local-only `dirty` flag rides along so an unsynced change
// survives a reboot.
bool AppendLine(const Item& item, std::string* out)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "id", item.id.c_str());
    cJSON_AddStringToObject(root, "type", TypeName(item.type));
    cJSON_AddStringToObject(root, "text", item.text.c_str());
    cJSON_AddStringToObject(root, "period", item.period.c_str());
    cJSON_AddStringToObject(root, "planned", item.planned.c_str());
    cJSON_AddStringToObject(root, "status", StatusName(item.status));
    cJSON_AddStringToObject(root, "recordingId", item.recording_id.c_str());
    cJSON_AddStringToObject(root, "origin", item.origin.c_str());
    cJSON_AddNumberToObject(root, "createdAt", static_cast<double>(item.created_at));
    cJSON_AddNumberToObject(root, "updatedAt", static_cast<double>(item.updated_at));
    cJSON_AddBoolToObject(root, "deleted", item.deleted);
    cJSON_AddBoolToObject(root, "dirty", item.dirty);
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (raw == nullptr) {
        return false;
    }
    out->append(raw);
    out->push_back('\n');
    cJSON_free(raw);
    return true;
}

struct LoadContext {
    ItemList* items = nullptr;
    bool found = false;
};

esp_err_t LoadOnMountedFilesystem(const char* mount_point, void* raw_context)
{
    LoadContext* context = static_cast<LoadContext*>(raw_context);
    const std::string path = JoinPath(JoinPath(mount_point, kDirectory).c_str(), kItemsFile);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    context->found = true;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0 || static_cast<size_t>(size) > kMaxFileBytes) {
        std::fclose(file);
        return size == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    char* buffer = static_cast<char*>(
        heap_caps_malloc(static_cast<size_t>(size) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        std::fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const size_t read = std::fread(buffer, 1, static_cast<size_t>(size), file);
    std::fclose(file);
    buffer[read] = '\0';

    int skipped = 0;
    size_t start = 0;
    while (start < read) {
        size_t end = start;
        while (end < read && buffer[end] != '\n') {
            ++end;
        }
        if (end > start) {
            Item item = {};
            if (ParseLine(buffer + start, end - start, &item)) {
                context->items->push_back(std::move(item));
            } else {
                ++skipped;
            }
        }
        start = end + 1;
    }
    heap_caps_free(buffer);
    if (skipped > 0) {
        ESP_LOGW(kTag, "Skipped %d unreadable journal line(s)", skipped);
    }
    return ESP_OK;
}

struct SaveContext {
    const std::string* content = nullptr;
};

esp_err_t SaveOnMountedFilesystem(const char* mount_point, void* raw_context)
{
    const SaveContext* context = static_cast<const SaveContext*>(raw_context);
    const std::string directory = JoinPath(mount_point, kDirectory);
    if (mkdir(directory.c_str(), 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(kTag, "mkdir %s failed: errno=%d", directory.c_str(), errno);
        return ESP_FAIL;
    }
    const std::string temp_path = JoinPath(directory.c_str(), kItemsTempFile);
    const std::string final_path = JoinPath(directory.c_str(), kItemsFile);
    FILE* file = std::fopen(temp_path.c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGW(kTag, "open %s failed: errno=%d", temp_path.c_str(), errno);
        return ESP_FAIL;
    }
    const size_t written = std::fwrite(context->content->data(), 1, context->content->size(), file);
    const bool flushed = std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    std::fclose(file);
    if (written != context->content->size() || !flushed) {
        std::remove(temp_path.c_str());
        return ESP_FAIL;
    }
    // FAT cannot rename over an existing file.
    std::remove(final_path.c_str());
    if (std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        ESP_LOGW(kTag, "rename journal file failed: errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool EnsureLoadedLocked()
{
    if (s_loaded) {
        return true;
    }
    ItemList items;
    LoadContext context = {.items = &items};
    const esp_err_t err =
        storage_service::RunWithMountedFilesystem(LoadOnMountedFilesystem, &context);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Journal load failed: %s", esp_err_to_name(err));
        return false;
    }
    s_items = std::move(items);
    s_loaded = true;
    ESP_LOGI(kTag, "Journal loaded: %u item(s)", static_cast<unsigned>(s_items.size()));
    return true;
}

bool PersistLocked()
{
    const int64_t now = NowSeconds();
    std::string content;
    content.reserve(s_items.size() * 200);
    for (auto it = s_items.begin(); it != s_items.end();) {
        // Prune synced tombstones once every device had a month to see them.
        if (it->deleted && !it->dirty && now > 0 && it->updated_at > 0 &&
            now - it->updated_at > kTombstoneRetentionSeconds) {
            it = s_items.erase(it);
            continue;
        }
        if (!AppendLine(*it, &content)) {
            return false;
        }
        ++it;
    }
    SaveContext context = {.content = &content};
    const esp_err_t err =
        storage_service::RunWithMountedFilesystem(SaveOnMountedFilesystem, &context);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Journal save failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

Item* FindLocked(const std::string& id)
{
    for (Item& item : s_items) {
        if (item.id.size() == id.size() && std::memcmp(item.id.data(), id.data(), id.size()) == 0) {
            return &item;
        }
    }
    return nullptr;
}

void Notify(const Event& event)
{
    std::array<ListenerSlot, kMaxListeners> listeners = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        listeners = s_listeners;
    }
    for (const ListenerSlot& slot : listeners) {
        if (slot.listener != nullptr) {
            slot.listener(event, slot.context);
        }
    }
}

// Applies `mutate` to a live item, persists, and rolls back on a failed write.
template <typename Mutation>
bool MutateItem(const std::string& id, Mutation mutate)
{
    Event event = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!EnsureLoadedLocked()) {
            return false;
        }
        Item* item = FindLocked(id);
        if (item == nullptr || item->deleted) {
            return false;
        }
        const Item before = *item;
        if (!mutate(*item)) {
            return false;
        }
        item->updated_at = std::max(NowSeconds(), before.updated_at + 1);
        item->dirty = true;
        if (!PersistLocked()) {
            Item* again = FindLocked(id);
            if (again != nullptr) {
                *again = before;
            }
            return false;
        }
        event.revision = ++s_revision;
        event.local_change = true;
    }
    Notify(event);
    return true;
}

// Width order used to keep `planned` as the widest period an item was filed under.
int LevelWidth(journal_period::Level level)
{
    switch (level) {
        case journal_period::Level::kYear:
            return 4;
        case journal_period::Level::kMonth:
            return 3;
        case journal_period::Level::kWeek:
            return 2;
        case journal_period::Level::kDay:
            return 1;
        case journal_period::Level::kNone:
        default:
            return 0;
    }
}

}  // namespace

esp_err_t Init()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    (void)EnsureLoadedLocked();
    return ESP_OK;
}

bool AddListener(Listener listener, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    for (ListenerSlot& slot : s_listeners) {
        if (slot.listener == nullptr) {
            slot = {listener, context};
            return true;
        }
    }
    return false;
}

Snapshot GetSnapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    Snapshot snapshot = {};
    snapshot.loaded = s_loaded;
    snapshot.revision = s_revision;
    for (const Item& item : s_items) {
        if (!item.deleted) {
            ++snapshot.item_count;
        }
        if (item.dirty) {
            ++snapshot.dirty_count;
        }
    }
    return snapshot;
}

void Reload()
{
    Event event = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_items.clear();
        s_loaded = false;
        (void)EnsureLoadedLocked();
        event.revision = ++s_revision;
        event.local_change = false;
    }
    Notify(event);
}

ItemList ListItems()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    ItemList result;
    if (!EnsureLoadedLocked()) {
        return result;
    }
    result.reserve(s_items.size());
    for (const Item& item : s_items) {
        if (!item.deleted) {
            result.push_back(item);
        }
    }
    return result;
}

bool FindItem(const std::string& id, Item* item)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!EnsureLoadedLocked()) {
        return false;
    }
    const Item* found = FindLocked(id);
    if (found == nullptr || found->deleted) {
        return false;
    }
    if (item != nullptr) {
        *item = *found;
    }
    return true;
}

std::string CreateItem(ItemType type, const std::string& text, const std::string& period,
                       const std::string& recording_id)
{
    const std::string normalized = NormalizeText(text);
    if (normalized.empty() || !journal_period::IsValidKey(period)) {
        return {};
    }
    Event event = {};
    std::string id;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!EnsureLoadedLocked()) {
            return {};
        }
        do {
            id = GenerateId();
        } while (FindLocked(id) != nullptr);
        Item item = {};
        item.id = ToP(id);
        item.type = type;
        item.text = ToP(normalized);
        item.period = ToP(period);
        item.planned = ToP(period);
        item.status = ItemStatus::kOpen;
        item.recording_id = ToP(recording_id);
        item.origin = ToP("device");
        item.created_at = NowSeconds();
        item.updated_at = item.created_at;
        item.dirty = true;
        s_items.push_back(std::move(item));
        if (!PersistLocked()) {
            s_items.pop_back();
            return {};
        }
        event.revision = ++s_revision;
    }
    ESP_LOGI(kTag, "Created %s %s in %s", TypeName(type), id.c_str(), period.c_str());
    Notify(event);
    return id;
}

bool SetStatus(const std::string& id, ItemStatus status)
{
    return MutateItem(id, [status](Item& item) {
        if (item.status == status) {
            return false;
        }
        item.status = status;
        return true;
    });
}

bool MoveTo(const std::string& id, const std::string& period)
{
    if (!journal_period::IsValidKey(period)) {
        return false;
    }
    return MutateItem(id, [&period](Item& item) {
        if (ToStd(item.period) == period) {
            return false;
        }
        item.period = ToP(period);
        const int new_width = LevelWidth(journal_period::LevelOf(period));
        const int planned_width = LevelWidth(journal_period::LevelOf(ToStd(item.planned)));
        if (new_width > planned_width) {
            item.planned = item.period;
        }
        return true;
    });
}

bool SetType(const std::string& id, ItemType type)
{
    return MutateItem(id, [type](Item& item) {
        if (item.type == type) {
            return false;
        }
        item.type = type;
        return true;
    });
}

bool SetText(const std::string& id, const std::string& text)
{
    const std::string normalized = NormalizeText(text);
    if (normalized.empty()) {
        return false;
    }
    return MutateItem(id, [&normalized](Item& item) {
        item.text = ToP(normalized);
        return true;
    });
}

bool Delete(const std::string& id)
{
    return MutateItem(id, [](Item& item) {
        item.deleted = true;
        return true;
    });
}

ItemList DirtyItems()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    ItemList result;
    if (!EnsureLoadedLocked()) {
        return result;
    }
    for (const Item& item : s_items) {
        if (item.dirty) {
            result.push_back(item);
        }
    }
    return result;
}

void MarkPushed(const std::string& id, int64_t updated_at)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    Item* item = FindLocked(id);
    if (item == nullptr || !item->dirty || item->updated_at != updated_at) {
        return;
    }
    item->dirty = false;
    (void)PersistLocked();
}

bool ApplyRemote(const Item& remote)
{
    if (remote.id.empty() || !journal_period::IsValidKey(ToStd(remote.period))) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!EnsureLoadedLocked()) {
        return false;
    }
    Item* local = FindLocked(ToStd(remote.id));
    if (local != nullptr && local->dirty && local->updated_at > remote.updated_at) {
        return false;  // our unsynced edit is newer; it will be pushed and win
    }
    if (local != nullptr && !local->dirty && local->updated_at == remote.updated_at &&
        local->deleted == remote.deleted && local->status == remote.status &&
        local->period == remote.period && local->text == remote.text &&
        local->type == remote.type) {
        return false;
    }
    Item merged = remote;
    merged.dirty = false;
    if (!journal_period::IsValidKey(ToStd(merged.planned))) {
        merged.planned = merged.period;
    }
    if (local != nullptr) {
        *local = std::move(merged);
    } else {
        if (remote.deleted) {
            return false;  // never seen and already gone
        }
        s_items.push_back(std::move(merged));
    }
    if (!PersistLocked()) {
        return false;
    }
    s_remote_batch_changed = true;
    return true;
}

void EndRemoteBatch()
{
    Event event = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_remote_batch_changed) {
            return;
        }
        s_remote_batch_changed = false;
        event.revision = ++s_revision;
        event.local_change = false;
    }
    Notify(event);
}

const char* TypeName(ItemType type)
{
    switch (type) {
        case ItemType::kNote:
            return "note";
        case ItemType::kEvent:
            return "event";
        case ItemType::kTask:
        default:
            return "task";
    }
}

const char* StatusName(ItemStatus status)
{
    switch (status) {
        case ItemStatus::kDone:
            return "done";
        case ItemStatus::kCancelled:
            return "cancelled";
        case ItemStatus::kOpen:
        default:
            return "open";
    }
}

bool ParseType(const std::string& text, ItemType* type)
{
    if (text == "task") {
        *type = ItemType::kTask;
    } else if (text == "note") {
        *type = ItemType::kNote;
    } else if (text == "event") {
        *type = ItemType::kEvent;
    } else {
        return false;
    }
    return true;
}

bool ParseStatus(const std::string& text, ItemStatus* status)
{
    if (text == "open") {
        *status = ItemStatus::kOpen;
    } else if (text == "done") {
        *status = ItemStatus::kDone;
    } else if (text == "cancelled") {
        *status = ItemStatus::kCancelled;
    } else {
        return false;
    }
    return true;
}

const char* TypeLabel(ItemType type)
{
    switch (type) {
        case ItemType::kNote:
            return "Nota";
        case ItemType::kEvent:
            return "Evento";
        case ItemType::kTask:
        default:
            return "Tarefa";
    }
}

bool IsPending(const Item& item, const journal_period::Date& today)
{
    return !item.deleted && item.status == ItemStatus::kOpen &&
           journal_period::HasEnded(ToStd(item.period), today);
}

}  // namespace journal_service

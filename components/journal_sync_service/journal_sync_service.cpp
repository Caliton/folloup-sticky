#include "journal_sync_service.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "journal_service.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "transcription_service.h"

namespace journal_sync_service {
namespace {

constexpr const char* kTag = "JournalSync";

// Public web-app config of the Firebase project (the same values ship in the web bundle).
constexpr const char* kApiKey = CONFIG_FOLLOWUP_FIREBASE_API_KEY;
constexpr const char* kProjectId = CONFIG_FOLLOWUP_FIREBASE_PROJECT_ID;

constexpr const char* kNvsNamespace = "jsync";
constexpr const char* kNvsDeviceUid = "dev_uid";
constexpr const char* kNvsRefreshToken = "refresh";
constexpr const char* kNvsOwnerUid = "owner";
constexpr const char* kNvsCursor = "cursor";

constexpr uint32_t kTaskStackBytes = 8192;
// TLS + JSON need headroom beyond the task stack; below this the round is postponed.
constexpr size_t kMinFreeInternalBytes = 20 * 1024;
constexpr int kHttpTimeoutMs = 20000;
constexpr size_t kMaxResponseBytes = 512 * 1024;
constexpr int64_t kPeriodicSyncUs = 15LL * 60 * 1000 * 1000;
constexpr int64_t kDebounceUs = 8LL * 1000 * 1000;
constexpr int64_t kRetryUs = 60LL * 1000 * 1000;
constexpr TickType_t kPairPollDelay = pdMS_TO_TICKS(4000);
constexpr int64_t kPairTimeoutUs = 15LL * 60 * 1000 * 1000;
constexpr int kPullPageSize = 25;
constexpr int kMaxPullPages = 40;
constexpr size_t kPushBatchSize = 15;
constexpr const char* kCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr const char* kEpochCursor = "1970-01-01T00:00:00Z";

struct Credentials {
    std::string device_uid = {};
    std::string refresh_token = {};
    std::string owner_uid = {};
    std::string cursor = {};
};

std::mutex s_mutex;
bool s_initialized = false;
EventHandler s_event_handler = nullptr;
void* s_event_context = nullptr;
Snapshot s_snapshot = {};
Credentials s_credentials = {};
std::string s_id_token = {};
int64_t s_id_token_expiry_us = 0;
int64_t s_pair_started_us = 0;
bool s_task_running = false;
bool s_sync_requested = false;
bool s_pair_requested = false;
esp_timer_handle_t s_debounce_timer = nullptr;
esp_timer_handle_t s_periodic_timer = nullptr;

// --- small helpers ----------------------------------------------------------------------------

void Notify()
{
    EventHandler handler = nullptr;
    void* context = nullptr;
    Event event = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        handler = s_event_handler;
        context = s_event_context;
        event.snapshot = s_snapshot;
    }
    if (handler != nullptr) {
        handler(event, context);
    }
}

std::string NvsGetString(nvs_handle_t handle, const char* key)
{
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) {
        return {};
    }
    std::string value(length, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) {
        return {};
    }
    value.resize(length > 0 ? length - 1 : 0);
    return value;
}

Credentials LoadCredentials()
{
    Credentials credentials = {};
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return credentials;
    }
    credentials.device_uid = NvsGetString(handle, kNvsDeviceUid);
    credentials.refresh_token = NvsGetString(handle, kNvsRefreshToken);
    credentials.owner_uid = NvsGetString(handle, kNvsOwnerUid);
    credentials.cursor = NvsGetString(handle, kNvsCursor);
    nvs_close(handle);
    return credentials;
}

// Called from the worker (internal-RAM stack, so flash writes are allowed).
void SaveCredentials(const Credentials& credentials)
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(kTag, "NVS open for write failed");
        return;
    }
    auto put = [handle](const char* key, const std::string& value) {
        if (value.empty()) {
            (void)nvs_erase_key(handle, key);
        } else {
            (void)nvs_set_str(handle, key, value.c_str());
        }
    };
    put(kNvsDeviceUid, credentials.device_uid);
    put(kNvsRefreshToken, credentials.refresh_token);
    put(kNvsOwnerUid, credentials.owner_uid);
    put(kNvsCursor, credentials.cursor);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

std::string DeviceName()
{
    uint8_t mac[6] = {};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[32] = {};
    std::snprintf(name, sizeof(name), "Followup %02X%02X", mac[4], mac[5]);
    return name;
}

std::string GenerateCode()
{
    const size_t alphabet = std::strlen(kCodeAlphabet);
    std::string code;
    for (int index = 0; index < 6; ++index) {
        code += kCodeAlphabet[esp_random() % alphabet];
    }
    return code;
}

std::string JsonString(const cJSON* object, const char* name)
{
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(field) && field->valuestring != nullptr ? field->valuestring
                                                                  : std::string();
}

std::string Base()
{
    return std::string("https://firestore.googleapis.com/v1/projects/") + kProjectId +
           "/databases/(default)/documents";
}

std::string DocName(const std::string& path)
{
    return std::string("projects/") + kProjectId + "/databases/(default)/documents/" + path;
}

// --- HTTP -------------------------------------------------------------------------------------

struct HttpResult {
    bool transport_ok = false;
    int status = 0;
    std::string body = {};
};

HttpResult Http(esp_http_client_method_t method, const std::string& url, const std::string& body,
                const char* content_type, const std::string& bearer)
{
    HttpResult result = {};
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = method;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return result;
    }
    if (content_type != nullptr) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "folloup-sticky");
    std::string authorization;
    if (!bearer.empty()) {
        authorization = "Bearer " + bearer;
        esp_http_client_set_header(client, "Authorization", authorization.c_str());
    }

    esp_err_t err = esp_http_client_open(client, static_cast<int>(body.size()));
    if (err == ESP_OK && !body.empty()) {
        size_t sent = 0;
        while (sent < body.size()) {
            const int written = esp_http_client_write(client, body.data() + sent,
                                                      static_cast<int>(body.size() - sent));
            if (written <= 0) {
                err = ESP_FAIL;
                break;
            }
            sent += static_cast<size_t>(written);
        }
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        result.status = esp_http_client_get_status_code(client);
        std::array<char, 512> buffer = {};
        result.transport_ok = true;
        while (true) {
            const int read = esp_http_client_read(client, buffer.data(), buffer.size());
            if (read < 0) {
                result.transport_ok = false;
                break;
            }
            if (read == 0) {
                break;
            }
            if (result.body.size() + static_cast<size_t>(read) > kMaxResponseBytes) {
                result.transport_ok = false;
                break;
            }
            result.body.append(buffer.data(), static_cast<size_t>(read));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!result.transport_ok) {
        ESP_LOGW(kTag, "HTTP transport failed: %s", url.c_str());
    }
    return result;
}

// --- auth -------------------------------------------------------------------------------------

enum class AuthResult : uint8_t {
    kOk,
    kNetworkError,
    kAccountGone,  // refresh token rejected: start over with a new anonymous account
};

AuthResult SignUpAnonymously(Credentials* credentials)
{
    const std::string url =
        std::string("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=") + kApiKey;
    const HttpResult response =
        Http(HTTP_METHOD_POST, url, "{\"returnSecureToken\":true}", "application/json", {});
    if (!response.transport_ok || response.status != 200) {
        ESP_LOGW(kTag, "Anonymous sign-up failed: http=%d %.160s", response.status,
                 response.body.c_str());
        return AuthResult::kNetworkError;
    }
    cJSON* root = cJSON_ParseWithLength(response.body.c_str(), response.body.size());
    if (root == nullptr) {
        return AuthResult::kNetworkError;
    }
    credentials->device_uid = JsonString(root, "localId");
    credentials->refresh_token = JsonString(root, "refreshToken");
    const std::string id_token = JsonString(root, "idToken");
    const std::string expires = JsonString(root, "expiresIn");
    cJSON_Delete(root);
    if (credentials->device_uid.empty() || credentials->refresh_token.empty() || id_token.empty()) {
        return AuthResult::kNetworkError;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_id_token = id_token;
    s_id_token_expiry_us = esp_timer_get_time() + (std::atoll(expires.c_str()) - 120) * 1000000LL;
    ESP_LOGI(kTag, "Anonymous device account created: %s", credentials->device_uid.c_str());
    return AuthResult::kOk;
}

AuthResult RefreshIdToken(Credentials* credentials)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_id_token.empty() && esp_timer_get_time() < s_id_token_expiry_us) {
            return AuthResult::kOk;
        }
    }
    const std::string url =
        std::string("https://securetoken.googleapis.com/v1/token?key=") + kApiKey;
    const std::string body = "grant_type=refresh_token&refresh_token=" + credentials->refresh_token;
    const HttpResult response =
        Http(HTTP_METHOD_POST, url, body, "application/x-www-form-urlencoded", {});
    if (!response.transport_ok) {
        return AuthResult::kNetworkError;
    }
    if (response.status == 400 || response.status == 403) {
        ESP_LOGW(kTag, "Refresh token rejected: %.160s", response.body.c_str());
        return AuthResult::kAccountGone;
    }
    if (response.status != 200) {
        return AuthResult::kNetworkError;
    }
    cJSON* root = cJSON_ParseWithLength(response.body.c_str(), response.body.size());
    if (root == nullptr) {
        return AuthResult::kNetworkError;
    }
    const std::string id_token = JsonString(root, "id_token");
    const std::string refresh_token = JsonString(root, "refresh_token");
    const std::string expires = JsonString(root, "expires_in");
    cJSON_Delete(root);
    if (id_token.empty()) {
        return AuthResult::kNetworkError;
    }
    if (!refresh_token.empty() && refresh_token != credentials->refresh_token) {
        credentials->refresh_token = refresh_token;
        SaveCredentials(*credentials);
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_id_token = id_token;
    s_id_token_expiry_us = esp_timer_get_time() + (std::atoll(expires.c_str()) - 120) * 1000000LL;
    return AuthResult::kOk;
}

std::string IdToken()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_id_token;
}

// Ensures an anonymous account plus a fresh ID token.
AuthResult EnsureAuth(Credentials* credentials)
{
    if (credentials->device_uid.empty() || credentials->refresh_token.empty()) {
        const AuthResult result = SignUpAnonymously(credentials);
        if (result == AuthResult::kOk) {
            SaveCredentials(*credentials);
        }
        return result;
    }
    const AuthResult result = RefreshIdToken(credentials);
    if (result == AuthResult::kAccountGone) {
        credentials->device_uid.clear();
        credentials->refresh_token.clear();
        credentials->owner_uid.clear();
        credentials->cursor.clear();
        SaveCredentials(*credentials);
    }
    return result;
}

// --- Firestore encoding -----------------------------------------------------------------------

void AddString(cJSON* fields, const char* name, const char* value)
{
    cJSON* field = cJSON_AddObjectToObject(fields, name);
    cJSON_AddStringToObject(field, "stringValue", value != nullptr ? value : "");
}

void AddInteger(cJSON* fields, const char* name, int64_t value)
{
    char text[24] = {};
    std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
    cJSON* field = cJSON_AddObjectToObject(fields, name);
    cJSON_AddStringToObject(field, "integerValue", text);
}

void AddBool(cJSON* fields, const char* name, bool value)
{
    cJSON* field = cJSON_AddObjectToObject(fields, name);
    cJSON_AddBoolToObject(field, "booleanValue", value);
}

void AddServerTimestampTransform(cJSON* write, const char* field_path)
{
    cJSON* transforms = cJSON_AddArrayToObject(write, "updateTransforms");
    cJSON* transform = cJSON_CreateObject();
    cJSON_AddStringToObject(transform, "fieldPath", field_path);
    cJSON_AddStringToObject(transform, "setToServerValue", "REQUEST_TIME");
    cJSON_AddItemToArray(transforms, transform);
}

void AddItemWrite(cJSON* writes, const std::string& owner, const journal_service::Item& item)
{
    cJSON* write = cJSON_CreateObject();
    cJSON* update = cJSON_AddObjectToObject(write, "update");
    cJSON_AddStringToObject(update, "name",
                            DocName("users/" + owner + "/items/" + item.id.c_str()).c_str());
    cJSON* fields = cJSON_AddObjectToObject(update, "fields");
    AddString(fields, "type", journal_service::TypeName(item.type));
    AddString(fields, "text", item.text.c_str());
    AddString(fields, "period", item.period.c_str());
    AddString(fields, "planned", item.planned.c_str());
    AddString(fields, "status", journal_service::StatusName(item.status));
    AddString(fields, "recordingId", item.recording_id.c_str());
    AddString(fields, "origin", item.origin.empty() ? "device" : item.origin.c_str());
    AddInteger(fields, "createdAt", item.created_at);
    AddInteger(fields, "updatedAt", item.updated_at);
    AddBool(fields, "deleted", item.deleted);
    AddServerTimestampTransform(write, "serverUpdatedAt");
    cJSON_AddItemToArray(writes, write);
}

std::string FieldString(const cJSON* fields, const char* name, const char* kind)
{
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(fields, name);
    return field != nullptr ? JsonString(field, kind) : std::string();
}

int64_t FieldInteger(const cJSON* fields, const char* name)
{
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(fields, name);
    if (field == nullptr) {
        return 0;
    }
    const cJSON* integer = cJSON_GetObjectItemCaseSensitive(field, "integerValue");
    if (cJSON_IsString(integer) && integer->valuestring != nullptr) {
        return std::atoll(integer->valuestring);
    }
    const cJSON* number = cJSON_GetObjectItemCaseSensitive(field, "doubleValue");
    return cJSON_IsNumber(number) ? static_cast<int64_t>(number->valuedouble) : 0;
}

bool FieldBool(const cJSON* fields, const char* name)
{
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(fields, name);
    return field != nullptr &&
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(field, "booleanValue"));
}

journal_service::PString ToP(const std::string& text)
{
    return journal_service::PString(text.data(), text.size());
}

bool DecodeItem(const cJSON* document, journal_service::Item* item, std::string* server_time)
{
    const std::string name = JsonString(document, "name");
    const size_t slash = name.rfind('/');
    const cJSON* fields = cJSON_GetObjectItemCaseSensitive(document, "fields");
    if (slash == std::string::npos || fields == nullptr) {
        return false;
    }
    journal_service::Item decoded = {};
    decoded.id = ToP(name.substr(slash + 1));
    if (!journal_service::ParseType(FieldString(fields, "type", "stringValue"), &decoded.type) ||
        !journal_service::ParseStatus(FieldString(fields, "status", "stringValue"),
                                      &decoded.status)) {
        return false;
    }
    decoded.text = ToP(FieldString(fields, "text", "stringValue"));
    decoded.period = ToP(FieldString(fields, "period", "stringValue"));
    decoded.planned = ToP(FieldString(fields, "planned", "stringValue"));
    decoded.recording_id = ToP(FieldString(fields, "recordingId", "stringValue"));
    decoded.origin = ToP(FieldString(fields, "origin", "stringValue"));
    decoded.created_at = FieldInteger(fields, "createdAt");
    decoded.updated_at = FieldInteger(fields, "updatedAt");
    decoded.deleted = FieldBool(fields, "deleted");
    *server_time = FieldString(fields, "serverUpdatedAt", "timestampValue");
    *item = std::move(decoded);
    return true;
}

// --- pairing ----------------------------------------------------------------------------------

enum class RoundResult : uint8_t {
    kOk,
    kRetryLater,
    kUnlinked,  // the web app removed this device
};

RoundResult CreatePairing(const Credentials& credentials, std::string* code_out)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::string code = GenerateCode();
        cJSON* root = cJSON_CreateObject();
        cJSON* writes = cJSON_AddArrayToObject(root, "writes");
        cJSON* write = cJSON_CreateObject();
        cJSON* update = cJSON_AddObjectToObject(write, "update");
        cJSON_AddStringToObject(update, "name", DocName("pairings/" + code).c_str());
        cJSON* fields = cJSON_AddObjectToObject(update, "fields");
        AddString(fields, "deviceUid", credentials.device_uid.c_str());
        AddString(fields, "deviceName", DeviceName().c_str());
        cJSON* owner = cJSON_AddObjectToObject(fields, "ownerUid");
        cJSON_AddNullToObject(owner, "nullValue");
        cJSON* precondition = cJSON_AddObjectToObject(write, "currentDocument");
        cJSON_AddBoolToObject(precondition, "exists", false);
        AddServerTimestampTransform(write, "createdAt");
        cJSON_AddItemToArray(writes, write);
        char* raw = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        const std::string body = raw != nullptr ? raw : "";
        cJSON_free(raw);

        const HttpResult response = Http(HTTP_METHOD_POST, Base() + ":commit", body,
                                         "application/json", IdToken());
        if (response.transport_ok && response.status == 200) {
            *code_out = code;
            return RoundResult::kOk;
        }
        ESP_LOGW(kTag, "Create pairing failed: http=%d %.200s", response.status,
                 response.body.c_str());
        if (!response.transport_ok || response.status >= 500) {
            return RoundResult::kRetryLater;
        }
        // 409/400 on a code collision: try another code.
    }
    return RoundResult::kRetryLater;
}

// Returns the owner uid once the web app claimed the code ("" while waiting).
RoundResult PollPairing(const std::string& code, std::string* owner_out)
{
    const HttpResult response =
        Http(HTTP_METHOD_GET, Base() + "/pairings/" + code, {}, nullptr, IdToken());
    if (!response.transport_ok || response.status != 200) {
        return RoundResult::kRetryLater;
    }
    cJSON* root = cJSON_ParseWithLength(response.body.c_str(), response.body.size());
    if (root == nullptr) {
        return RoundResult::kRetryLater;
    }
    const cJSON* fields = cJSON_GetObjectItemCaseSensitive(root, "fields");
    *owner_out = fields != nullptr ? FieldString(fields, "ownerUid", "stringValue") : std::string();
    cJSON_Delete(root);
    return RoundResult::kOk;
}

// --- sync round -------------------------------------------------------------------------------

std::string BuildPullQuery(const std::string& cursor)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* query = cJSON_AddObjectToObject(root, "structuredQuery");
    cJSON* from = cJSON_AddArrayToObject(query, "from");
    cJSON* collection = cJSON_CreateObject();
    cJSON_AddStringToObject(collection, "collectionId", "items");
    cJSON_AddItemToArray(from, collection);
    cJSON* where = cJSON_AddObjectToObject(query, "where");
    cJSON* filter = cJSON_AddObjectToObject(where, "fieldFilter");
    cJSON* field = cJSON_AddObjectToObject(filter, "field");
    cJSON_AddStringToObject(field, "fieldPath", "serverUpdatedAt");
    cJSON_AddStringToObject(filter, "op", "GREATER_THAN");
    cJSON* value = cJSON_AddObjectToObject(filter, "value");
    cJSON_AddStringToObject(value, "timestampValue", cursor.empty() ? kEpochCursor : cursor.c_str());
    cJSON* order_by = cJSON_AddArrayToObject(query, "orderBy");
    cJSON* order = cJSON_CreateObject();
    cJSON* order_field = cJSON_AddObjectToObject(order, "field");
    cJSON_AddStringToObject(order_field, "fieldPath", "serverUpdatedAt");
    cJSON_AddStringToObject(order, "direction", "ASCENDING");
    cJSON_AddItemToArray(order_by, order);
    cJSON_AddNumberToObject(query, "limit", kPullPageSize);
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string body = raw != nullptr ? raw : "";
    cJSON_free(raw);
    return body;
}

RoundResult Pull(Credentials* credentials)
{
    const std::string url = Base() + "/users/" + credentials->owner_uid + ":runQuery";
    const std::string starting_cursor = credentials->cursor;
    int applied = 0;
    for (int page = 0; page < kMaxPullPages; ++page) {
        const HttpResult response = Http(HTTP_METHOD_POST, url, BuildPullQuery(credentials->cursor),
                                         "application/json", IdToken());
        if (!response.transport_ok) {
            return RoundResult::kRetryLater;
        }
        if (response.status == 403) {
            return RoundResult::kUnlinked;
        }
        if (response.status != 200) {
            ESP_LOGW(kTag, "Pull failed: http=%d %.200s", response.status, response.body.c_str());
            return RoundResult::kRetryLater;
        }
        cJSON* root = cJSON_ParseWithLength(response.body.c_str(), response.body.size());
        if (!cJSON_IsArray(root)) {
            cJSON_Delete(root);
            return RoundResult::kRetryLater;
        }
        int documents = 0;
        const cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, root)
        {
            const cJSON* document = cJSON_GetObjectItemCaseSensitive(entry, "document");
            if (document == nullptr) {
                continue;
            }
            ++documents;
            journal_service::Item item = {};
            std::string server_time;
            if (DecodeItem(document, &item, &server_time)) {
                if (journal_service::ApplyRemote(item)) {
                    ++applied;
                }
            }
            // Results are ordered by serverUpdatedAt, so the last one is the new cursor.
            if (!server_time.empty()) {
                credentials->cursor = server_time;
            }
        }
        cJSON_Delete(root);
        if (documents < kPullPageSize) {
            break;
        }
    }
    journal_service::EndRemoteBatch();
    if (credentials->cursor != starting_cursor) {
        SaveCredentials(*credentials);  // spare the flash when nothing new arrived
    }
    if (applied > 0) {
        ESP_LOGI(kTag, "Pulled %d change(s)", applied);
    }
    return RoundResult::kOk;
}

HttpResult CommitItems(const std::string& owner, const journal_service::ItemList& items,
                       size_t first, size_t count)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* writes = cJSON_AddArrayToObject(root, "writes");
    for (size_t index = first; index < first + count; ++index) {
        AddItemWrite(writes, owner, items[index]);
    }
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    const std::string body = raw != nullptr ? raw : "";
    cJSON_free(raw);
    return Http(HTTP_METHOD_POST, Base() + ":commit", body, "application/json", IdToken());
}

RoundResult Push(const Credentials& credentials)
{
    const journal_service::ItemList dirty = journal_service::DirtyItems();
    int pushed = 0;
    for (size_t first = 0; first < dirty.size(); first += kPushBatchSize) {
        const size_t count = std::min(kPushBatchSize, dirty.size() - first);
        HttpResult response = CommitItems(credentials.owner_uid, dirty, first, count);
        if (!response.transport_ok || response.status >= 500) {
            return RoundResult::kRetryLater;
        }
        if (response.status == 200) {
            for (size_t index = first; index < first + count; ++index) {
                journal_service::MarkPushed(dirty[index].id.c_str(), dirty[index].updated_at);
            }
            pushed += static_cast<int>(count);
            continue;
        }
        // The batch was rejected (a single invalid document fails the whole commit): retry one
        // by one and drop the ones the rules refuse, so one bad item can't block the rest.
        ESP_LOGW(kTag, "Batch push rejected: http=%d %.200s", response.status,
                 response.body.c_str());
        for (size_t index = first; index < first + count; ++index) {
            response = CommitItems(credentials.owner_uid, dirty, index, 1);
            if (!response.transport_ok || response.status >= 500) {
                return RoundResult::kRetryLater;
            }
            if (response.status != 200) {
                ESP_LOGW(kTag, "Dropping unsyncable item %s: http=%d", dirty[index].id.c_str(),
                         response.status);
            } else {
                ++pushed;
            }
            journal_service::MarkPushed(dirty[index].id.c_str(), dirty[index].updated_at);
        }
    }
    if (pushed > 0) {
        ESP_LOGI(kTag, "Pushed %d item(s)", pushed);
    }
    return RoundResult::kOk;
}

// --- worker -----------------------------------------------------------------------------------

bool CanRunNow()
{
    if (transcription_service::GetSnapshot().request_in_flight) {
        return false;  // a Gemini upload owns the network and most of the heap right now
    }
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= kMinFreeInternalBytes &&
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= kTaskStackBytes + 1024;
}

void ScheduleIn(int64_t delay_us)
{
    if (s_debounce_timer == nullptr) {
        return;
    }
    (void)esp_timer_stop(s_debounce_timer);
    (void)esp_timer_start_once(s_debounce_timer, delay_us);
}

void MarkUnlinked(Credentials* credentials, const char* message)
{
    credentials->owner_uid.clear();
    credentials->cursor.clear();
    SaveCredentials(*credentials);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_credentials = *credentials;
        s_snapshot.state = LinkState::kUnlinked;
        s_snapshot.pair_code.clear();
        s_snapshot.last_error = message != nullptr ? message : "";
    }
    Notify();
}

void RunPairing(Credentials* credentials)
{
    std::string code;
    if (CreatePairing(*credentials, &code) != RoundResult::kOk) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_snapshot.state = LinkState::kUnlinked;
            s_snapshot.last_error = "Não foi possível gerar o código";
        }
        Notify();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.pair_code = code;
        s_snapshot.last_error.clear();
        s_pair_started_us = esp_timer_get_time();
    }
    ESP_LOGI(kTag, "Pairing code ready: %s", code.c_str());
    Notify();

    while (true) {
        vTaskDelay(kPairPollDelay);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_snapshot.state != LinkState::kPairing || s_snapshot.pair_code != code) {
                return;  // cancelled
            }
            if (esp_timer_get_time() - s_pair_started_us > kPairTimeoutUs) {
                s_snapshot.state = LinkState::kUnlinked;
                s_snapshot.pair_code.clear();
                s_snapshot.last_error = "O código expirou";
                break;
            }
        }
        if (EnsureAuth(credentials) != AuthResult::kOk) {
            continue;
        }
        std::string owner;
        if (PollPairing(code, &owner) == RoundResult::kOk && !owner.empty()) {
            credentials->owner_uid = owner;
            credentials->cursor.clear();
            SaveCredentials(*credentials);
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_credentials = *credentials;
                s_snapshot.state = LinkState::kLinked;
                s_snapshot.pair_code.clear();
                s_snapshot.last_error.clear();
                s_sync_requested = true;
            }
            ESP_LOGI(kTag, "Paired with web account");
            Notify();
            return;
        }
    }
    Notify();
}

void RunSync(Credentials* credentials)
{
    RoundResult result = Pull(credentials);
    if (result == RoundResult::kOk) {
        result = Push(*credentials);
    }
    if (result == RoundResult::kUnlinked) {
        MarkUnlinked(credentials, "O app web desconectou este aparelho");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_credentials = *credentials;
        if (result == RoundResult::kOk) {
            const time_t now = time(nullptr);
            s_snapshot.last_sync_unix = static_cast<int64_t>(now);
            s_snapshot.last_error.clear();
        } else {
            s_snapshot.last_error = "Falha ao sincronizar";
        }
    }
    if (result != RoundResult::kOk) {
        ScheduleIn(kRetryUs);
    }
}

void WorkerTask(void*)
{
    while (true) {
        bool pair = false;
        bool sync = false;
        Credentials credentials = {};
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            pair = s_pair_requested;
            sync = s_sync_requested && s_snapshot.state == LinkState::kLinked;
            s_pair_requested = false;
            s_sync_requested = false;
            if (!pair && !sync) {
                s_task_running = false;
                s_snapshot.busy = false;
                break;
            }
            s_snapshot.busy = true;
            credentials = s_credentials;
        }
        Notify();

        const AuthResult auth = EnsureAuth(&credentials);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_credentials = credentials;
        }
        if (auth == AuthResult::kAccountGone) {
            MarkUnlinked(&credentials, "Conecte o aparelho de novo");
            continue;
        }
        if (auth != AuthResult::kOk) {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                if (pair) {
                    s_snapshot.state = LinkState::kUnlinked;
                }
                s_snapshot.last_error = "Sem conexão com o Firebase";
            }
            if (!pair) {
                ScheduleIn(kRetryUs);
            }
            continue;
        }
        if (pair) {
            RunPairing(&credentials);
        } else {
            RunSync(&credentials);
        }
    }
    Notify();
    vTaskDelete(nullptr);
}

// Starts the worker if it isn't running and conditions allow; otherwise retries later.
void KickWorker()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_task_running) {
            return;
        }
        if (!s_snapshot.network_connected) {
            return;  // SetNetworkConnected(true) kicks again
        }
        if (!s_pair_requested && !(s_sync_requested && s_snapshot.state == LinkState::kLinked)) {
            return;
        }
    }
    if (!CanRunNow()) {
        ScheduleIn(kDebounceUs * 2);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_task_running = true;
    }
    if (xTaskCreatePinnedToCore(WorkerTask, "journal_sync", kTaskStackBytes, nullptr,
                                followup_task_config::kPriorityStorage, nullptr,
                                followup_task_config::kSystemCore) != pdPASS) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_task_running = false;
        ESP_LOGW(kTag, "Failed to start sync worker");
    }
}

void DebounceTimerCallback(void*)
{
    KickWorker();
}

void PeriodicTimerCallback(void*)
{
    RequestSync();
}

void HandleJournalChange(const journal_service::Event& event, void*)
{
    if (event.local_change) {
        RequestSync();
    }
}

}  // namespace

esp_err_t Init()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_initialized) {
            return ESP_OK;
        }
        s_initialized = true;
        s_credentials = LoadCredentials();
        s_snapshot.state = s_credentials.owner_uid.empty() ? LinkState::kUnlinked
                                                           : LinkState::kLinked;
    }
    const esp_timer_create_args_t debounce_args = {
        .callback = &DebounceTimerCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "jsync_kick",
        .skip_unhandled_events = true,
    };
    const esp_timer_create_args_t periodic_args = {
        .callback = &PeriodicTimerCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "jsync_periodic",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&debounce_args, &s_debounce_timer) != ESP_OK ||
        esp_timer_create(&periodic_args, &s_periodic_timer) != ESP_OK) {
        ESP_LOGW(kTag, "Timer creation failed");
        return ESP_ERR_NO_MEM;
    }
    (void)esp_timer_start_periodic(s_periodic_timer, kPeriodicSyncUs);
    (void)journal_service::AddListener(HandleJournalChange, nullptr);
    ESP_LOGI(kTag, "Journal sync ready: %s",
             s_snapshot.state == LinkState::kLinked ? "linked" : "not linked");
    return ESP_OK;
}

void SetEventHandler(EventHandler handler, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_event_handler = handler;
    s_event_context = context;
}

Snapshot GetSnapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_snapshot;
}

void SetNetworkConnected(bool connected)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        changed = s_snapshot.network_connected != connected;
        s_snapshot.network_connected = connected;
    }
    if (changed && connected) {
        RequestSync();
    }
}

bool StartPairing()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized || !s_snapshot.network_connected) {
            s_snapshot.last_error = "Conecte o Wi-Fi para parear";
            return false;
        }
        s_snapshot.state = LinkState::kPairing;
        s_snapshot.pair_code.clear();
        s_snapshot.last_error.clear();
        s_pair_requested = true;
    }
    Notify();
    KickWorker();
    return true;
}

void CancelPairing()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_snapshot.state != LinkState::kPairing) {
            return;
        }
        s_snapshot.state = LinkState::kUnlinked;
        s_snapshot.pair_code.clear();
        s_pair_requested = false;
    }
    Notify();
}

void Unlink()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_credentials.owner_uid.clear();
        s_credentials.cursor.clear();
        s_snapshot.state = LinkState::kUnlinked;
        s_snapshot.pair_code.clear();
        s_snapshot.last_error.clear();
        // Persisted from the worker's next run; do it here directly when idle (the caller is
        // an input task with an internal-RAM stack, where flash writes are fine).
        SaveCredentials(s_credentials);
    }
    Notify();
}

void RequestSync()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized || s_snapshot.state != LinkState::kLinked) {
            return;
        }
        s_sync_requested = true;
    }
    ScheduleIn(kDebounceUs);
}

}  // namespace journal_sync_service

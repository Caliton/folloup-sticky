#include "book_worker.h"

#include <mutex>
#include <utility>

#include "esp_log.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace book_worker {
namespace {

constexpr const char* kTag = "BookWorker";
// All book data lives in PSRAM (epub_reader allocates there explicitly); the stack only
// holds call frames for FATFS, inflate (ROM) and TJpgDec (ROM).
constexpr uint32_t kStackBytes = 6144;

std::mutex s_mutex;
std::function<void()> s_pending;
bool s_running = false;

void WorkerTask(void*)
{
    for (;;) {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_pending) {
                s_running = false;
                break;
            }
            job = std::move(s_pending);
            s_pending = nullptr;
        }
        job();
    }
    // Every RAII owner above is out of scope: vTaskDelete(nullptr) never returns.
    vTaskDelete(nullptr);
}

}  // namespace

bool Submit(std::function<void()> job)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_pending = std::move(job);
    if (s_running) {
        return true;
    }
    if (xTaskCreatePinnedToCore(WorkerTask, "book_worker", kStackBytes, nullptr,
                                followup_task_config::kPriorityStorage, nullptr,
                                followup_task_config::kAppCore) != pdPASS) {
        ESP_LOGW(kTag, "could not start book worker (internal RAM low?)");
        s_pending = nullptr;
        return false;
    }
    s_running = true;
    return true;
}

}  // namespace book_worker

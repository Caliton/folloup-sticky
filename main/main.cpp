#include "app_shell.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr const char* kHeapTag = "Heap";
constexpr uint64_t kHeapReportPeriodUs = 60ULL * 1000000ULL;

// Internal RAM (task stacks, Wi-Fi/lwIP, DMA) is the scarce pool on this board; PSRAM
// absorbs the rest. Any failed allocation is logged with its size, caps and task so a
// shortage shows up in the log instead of only as a later ESP_ERR_NO_MEM.
void OnAllocFailed(size_t size, uint32_t caps, const char* function_name)
{
    const char* task = pcTaskGetName(nullptr);
    esp_rom_printf("[Heap] alloc failed: size=%u caps=0x%08lx fn=%s task=%s "
                   "int_free=%u int_largest=%u\n",
                   static_cast<unsigned>(size), static_cast<unsigned long>(caps),
                   function_name != nullptr ? function_name : "?", task != nullptr ? task : "?",
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

void ReportHeap(void*)
{
    ESP_LOGI(kHeapTag, "internal free=%u min=%u largest=%u | psram free=%u min=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    // Unused stack per task (bytes): tells which internal-RAM stacks are oversized.
    constexpr UBaseType_t kMaxTasks = 40;
    auto* tasks = static_cast<TaskStatus_t*>(
        heap_caps_malloc(sizeof(TaskStatus_t) * kMaxTasks, MALLOC_CAP_SPIRAM));
    if (tasks == nullptr) {
        return;
    }
    const UBaseType_t count = uxTaskGetSystemState(tasks, kMaxTasks, nullptr);
    for (UBaseType_t i = 0; i < count; ++i) {
        ESP_LOGI(kHeapTag, "task %-16s prio=%2u stack_unused=%5u",
                 tasks[i].pcTaskName, static_cast<unsigned>(tasks[i].uxCurrentPriority),
                 static_cast<unsigned>(tasks[i].usStackHighWaterMark));
    }
    heap_caps_free(tasks);
#endif
}

void StartHeapReports()
{
    const esp_timer_create_args_t args = {
        .callback = ReportHeap,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "heap_report",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, kHeapReportPeriodUs);
    }
}

}  // namespace

extern "C" void app_main(void)
{
    heap_caps_register_failed_alloc_callback(OnAllocFailed);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    StartHeapReports();
    app_shell::Run();
    ReportHeap(nullptr);
}

#include "power_key_runtime.h"

#include <mutex>

#include "axp2101.h"
#include "device_sleep_runtime.h"
#include "device_sleep_service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "waveshare_board.h"

namespace power_key_runtime {
namespace {

constexpr const char* kTag = "PowerKeyRuntime";
constexpr uint64_t kDoubleTapWindowUs = 400 * 1000;

std::mutex s_mutex;
PressHandler s_handler = nullptr;
void* s_handler_context = nullptr;
// Armed by a first short tap; firing means no second tap came, so it was a single tap.
esp_timer_handle_t s_tap_timer = nullptr;
bool s_tap_pending = false;

// Long wins when both bits are set: one hold can latch the short IRQ on the way past the
// long threshold, and acting on both would fire two different actions for one press.
bool DecodePress(uint64_t irq_status, Press* press)
{
    if ((irq_status & XPOWERS_AXP2101_PKEY_LONG_IRQ) != 0) {
        *press = Press::kLong;
        return true;
    }
    if ((irq_status & XPOWERS_AXP2101_PKEY_SHORT_IRQ) != 0) {
        *press = Press::kShort;
        return true;
    }
    return false;
}

// A press that arrives while the device is display- or light-sleeping only wakes it.
// Without this the press that wakes the device would also run its action -- the same
// suppression device_sleep_runtime applies to the ACTION wake gesture.
bool ConsumeAsWake()
{
    const device_sleep_service::Stage stage =
        device_sleep_service::GetSnapshot().runtime.stage;
    device_sleep_runtime::NotifyUserActivity();
    if (stage == device_sleep_service::Stage::kAwake) {
        return false;
    }
    ESP_LOGI(kTag, "Power key consumed as wake from stage=%d", static_cast<int>(stage));
    return true;
}

const char* PressName(Press press)
{
    switch (press) {
        case Press::kDouble:
            return "double";
        case Press::kLong:
            return "long";
        case Press::kShort:
        default:
            return "short";
    }
}

void Deliver(Press press)
{
    PressHandler handler = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        handler = s_handler;
        context = s_handler_context;
    }
    if (handler == nullptr) {
        ESP_LOGW(kTag, "Power key press with no handler attached");
        return;
    }
    ESP_LOGI(kTag, "Power key %s press", PressName(press));
    handler(press, context);
}

// Runs on the esp_timer task when the window closes without a second tap.
void OnTapWindowClosed(void*)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_tap_pending) {
            return;
        }
        s_tap_pending = false;
    }
    Deliver(Press::kShort);
}

// Runs on the PMIC driver's own IRQ task, not in ISR context, and the driver has already
// read and cleared the status register before calling us.
void HandleInterrupt(const Axp2101::InterruptEvent& event)
{
    Press press = Press::kShort;
    if (!DecodePress(event.irq_status, &press)) {
        return;
    }
    if (ConsumeAsWake()) {
        return;
    }

    if (press == Press::kShort) {
        bool second_tap = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_tap_pending) {
                s_tap_pending = false;
                second_tap = true;
                (void)esp_timer_stop(s_tap_timer);
            } else if (s_tap_timer != nullptr) {
                s_tap_pending = true;
                (void)esp_timer_start_once(s_tap_timer, kDoubleTapWindowUs);
                return;
            }
        }
        // No timer (creation failed): fall back to acting on the tap immediately.
        Deliver(second_tap ? Press::kDouble : Press::kShort);
        return;
    }

    // A hold cancels a pending single tap rather than firing it afterwards.
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_tap_pending) {
            s_tap_pending = false;
            (void)esp_timer_stop(s_tap_timer);
        }
    }
    Deliver(press);
}

}  // namespace

void SetPressHandler(PressHandler handler, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_handler = handler;
    s_handler_context = context;
}

esp_err_t Init()
{
    Axp2101* pmic = waveshare_board::GetPmic();
    if (pmic == nullptr) {
        ESP_LOGE(kTag, "PMIC unavailable; power key is inert");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_tap_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &OnTapWindowClosed,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pwr_tap",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_tap_timer) != ESP_OK) {
            ESP_LOGW(kTag, "Double-tap timer unavailable; taps act immediately");
            s_tap_timer = nullptr;
        }
    }
    pmic->SetInterruptCallback(&HandleInterrupt);
    ESP_LOGI(kTag, "Power key handler attached");
    return ESP_OK;
}

}  // namespace power_key_runtime

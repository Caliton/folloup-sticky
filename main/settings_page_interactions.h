#ifndef SETTINGS_PAGE_INTERACTIONS_H_
#define SETTINGS_PAGE_INTERACTIONS_H_

#include <cstdint>
#include <functional>

#include "page_action_result.h"
#include "settings_page_coordinator.h"

namespace settings_page_interactions {

enum class ActivateIntent : uint8_t {
    kNone = 0,
    kShowHome,
    kShowToday,
    kForceRefresh,
    kToggleWifi,
    kToggleAccessPoint,
    kToggleSound,
    kEnableOtg,
    kShowFormatSdModal,
    kShowOnboarding,
    kOpenAppLink,
    kOpenWifiPage,
    kOpenTimePage,
};

struct ActivateResult {
    ActivateIntent intent = ActivateIntent::kNone;
    bool handled = false;
    bool play_activate_cue = false;
};

using FocusMoveResult = page_actions::FocusMoveOutcome;

struct ActivateCallbacks {
    std::function<void()> show_home;
    std::function<void()> show_today;
    std::function<void()> force_refresh;
    std::function<void()> toggle_wifi;
    std::function<void()> toggle_access_point;
    std::function<void()> toggle_sound;
    std::function<void()> enable_otg;
    std::function<void()> show_format_sd_modal;
    std::function<void()> show_onboarding;
    std::function<void()> open_app_link;
    std::function<void()> open_wifi_page;
    std::function<void()> open_time_page;
};

ActivateResult HandlePrimaryActivate(const SettingsPageCoordinator& coordinator);
void ApplyPrimaryActivateResult(const ActivateResult& result,
                               const ActivateCallbacks& callbacks);
FocusMoveResult HandleMoveFocus(SettingsPageCoordinator& coordinator, int delta);

}  // namespace settings_page_interactions

#endif  // SETTINGS_PAGE_INTERACTIONS_H_

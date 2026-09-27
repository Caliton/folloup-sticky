#include "settings_page_interactions.h"

#include "shared_page_interactions.h"

namespace settings_page_interactions {

ActivateResult HandlePrimaryActivate(const SettingsPageCoordinator& coordinator)
{
    const ActivateResult footer_result =
        shared_page_interactions::HandleFooterPrimaryActivate<ActivateResult>(
            coordinator,
            ActivateIntent::kShowHome,
            ActivateIntent::kForceRefresh,
            ActivateIntent::kShowToday);
    if (footer_result.handled) {
        return footer_result;
    }

    if (coordinator.IsRoleFocused(page_navigation::NavigationItemRole::kSettingsWifiToggle)) {
        return {
            .intent = ActivateIntent::kToggleWifi,
            .handled = true,
            .play_activate_cue = true,
        };
    }
    if (coordinator.IsRoleFocused(
            page_navigation::NavigationItemRole::kSettingsEnableApToggle)) {
        return {
            .intent = ActivateIntent::kToggleAccessPoint,
            .handled = true,
            .play_activate_cue = true,
        };
    }
    if (coordinator.IsRoleFocused(page_navigation::NavigationItemRole::kSettingsSoundToggle)) {
        return {
            .intent = ActivateIntent::kToggleSound,
            .handled = true,
            .play_activate_cue = true,
        };
    }
    if (coordinator.IsRoleFocused(
            page_navigation::NavigationItemRole::kSettingsEnableOtgButton)) {
        return {
            .intent = ActivateIntent::kEnableOtg,
            .handled = true,
            .play_activate_cue = true,
        };
    }
    if (coordinator.IsRoleFocused(
            page_navigation::NavigationItemRole::kSettingsFormatSdButton)) {
        return {
            .intent = ActivateIntent::kShowFormatSdModal,
            .handled = true,
            .play_activate_cue = true,
        };
    }
    if (coordinator.IsRoleFocused(
            page_navigation::NavigationItemRole::kSettingsManualOnboardingButton)) {
        return {
            .intent = ActivateIntent::kShowOnboarding,
            .handled = true,
            .play_activate_cue = true,
        };
    }
    if (coordinator.IsRoleFocused(page_navigation::NavigationItemRole::kSettingsWifiPageButton)) {
        return {
            .intent = ActivateIntent::kOpenWifiPage,
            .handled = true,
            .play_activate_cue = true,
        };
    }
    if (coordinator.IsRoleFocused(page_navigation::NavigationItemRole::kSettingsTimePageButton)) {
        return {
            .intent = ActivateIntent::kOpenTimePage,
            .handled = true,
            .play_activate_cue = true,
        };
    }
    if (coordinator.IsRoleFocused(page_navigation::NavigationItemRole::kSettingsAppLinkButton)) {
        return {
            .intent = ActivateIntent::kOpenAppLink,
            .handled = true,
            .play_activate_cue = true,
        };
    }

    return {};
}

void ApplyPrimaryActivateResult(const ActivateResult& result,
                                const ActivateCallbacks& callbacks)
{
    if (!result.handled) {
        return;
    }

    switch (result.intent) {
        case ActivateIntent::kShowHome:
            if (callbacks.show_home) {
                callbacks.show_home();
            }
            return;
        case ActivateIntent::kShowToday:
            if (callbacks.show_today) {
                callbacks.show_today();
            }
            return;
        case ActivateIntent::kForceRefresh:
            if (callbacks.force_refresh) {
                callbacks.force_refresh();
            }
            return;
        case ActivateIntent::kToggleWifi:
            if (callbacks.toggle_wifi) {
                callbacks.toggle_wifi();
            }
            return;
        case ActivateIntent::kToggleSound:
            if (callbacks.toggle_sound) {
                callbacks.toggle_sound();
            }
            break;
        case ActivateIntent::kToggleAccessPoint:
            if (callbacks.toggle_access_point) {
                callbacks.toggle_access_point();
            }
            return;
        case ActivateIntent::kEnableOtg:
            if (callbacks.enable_otg) {
                callbacks.enable_otg();
            }
            return;
        case ActivateIntent::kShowFormatSdModal:
            if (callbacks.show_format_sd_modal) {
                callbacks.show_format_sd_modal();
            }
            return;
        case ActivateIntent::kShowOnboarding:
            if (callbacks.show_onboarding) {
                callbacks.show_onboarding();
            }
            return;
        case ActivateIntent::kOpenAppLink:
            if (callbacks.open_app_link) {
                callbacks.open_app_link();
            }
            return;
        case ActivateIntent::kOpenWifiPage:
            if (callbacks.open_wifi_page) {
                callbacks.open_wifi_page();
            }
            return;
        case ActivateIntent::kOpenTimePage:
            if (callbacks.open_time_page) {
                callbacks.open_time_page();
            }
            return;
        case ActivateIntent::kNone:
        default:
            return;
    }
}

FocusMoveResult HandleMoveFocus(SettingsPageCoordinator& coordinator, int delta)
{
    return shared_page_interactions::HandleMoveFocus(coordinator, delta);
}

}  // namespace settings_page_interactions

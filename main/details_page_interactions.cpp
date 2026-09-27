#include "details_page_interactions.h"

namespace details_page_interactions {
namespace {

using page_navigation::NavigationItemRole;

}  // namespace

ActivateResult HandlePrimaryActivate(DetailsPageCoordinator& coordinator)
{
    ActivateResult result = {};

    if (coordinator.IsRoleFocused(NavigationItemRole::kDetailsPageScrollContainer)) {
        result.handled = true;
        if (coordinator.scroll_container_active()) {
            // Already scrolling; OK is inert (DOWN double-click exits).
            return result;
        }
        coordinator.EnterScrollContainer();
        result.play_activate_cue = true;
        result.apply_page_state = true;
        return result;
    }

    if (coordinator.IsRoleFocused(NavigationItemRole::kDetailsPageBackButton)) {
        result.intent = ActivateIntent::kShowPreviousPage;
        result.handled = true;
        result.play_activate_cue = true;
        return result;
    }

    if (coordinator.IsRoleFocused(NavigationItemRole::kDetailsPageTranscribeButton)) {
        result.intent = coordinator.has_transcript() ? ActivateIntent::kPlayRecording
                                                     : ActivateIntent::kTranscribe;
        result.handled = true;
        result.play_activate_cue = true;
        return result;
    }

    result.handled = true;
    result.play_activate_cue = true;
    if (coordinator.IsRoleFocused(NavigationItemRole::kFooterHome)) {
        result.intent = ActivateIntent::kShowHome;
    } else if (coordinator.IsRoleFocused(NavigationItemRole::kFooterSettings)) {
        result.intent = ActivateIntent::kShowSettings;
    } else if (coordinator.IsRoleFocused(NavigationItemRole::kFooterToday)) {
        result.intent = ActivateIntent::kShowToday;
    } else {
        result.handled = false;
        result.play_activate_cue = false;
    }
    return result;
}

void ApplyPrimaryActivateResult(const ActivateResult& result, const ActivateCallbacks& callbacks)
{
    switch (result.intent) {
        case ActivateIntent::kShowHome:
            if (callbacks.show_home) {
                callbacks.show_home();
            }
            break;
        case ActivateIntent::kShowSettings:
            if (callbacks.show_settings) {
                callbacks.show_settings();
            }
            break;
        case ActivateIntent::kShowToday:
            if (callbacks.show_today) {
                callbacks.show_today();
            }
            break;
        case ActivateIntent::kShowPreviousPage:
            if (callbacks.show_previous_page) {
                callbacks.show_previous_page();
            }
            break;
        case ActivateIntent::kTranscribe:
            if (callbacks.transcribe) {
                callbacks.transcribe();
            }
            break;
        case ActivateIntent::kPlayRecording:
            if (callbacks.play) {
                callbacks.play();
            }
            break;
        case ActivateIntent::kNone:
        default:
            break;
    }
}

FocusMoveResult HandleMoveFocus(DetailsPageCoordinator& coordinator, int delta)
{
    FocusMoveResult result = {};
    if (!coordinator.MoveFocus(delta)) {
        return result;
    }
    result.handled = true;
    result.play_navigation_cue = true;
    result.apply_page_state = true;
    result.sync_footer_projection = true;
    return result;
}

}  // namespace details_page_interactions

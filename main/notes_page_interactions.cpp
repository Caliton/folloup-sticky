#include "notes_page_interactions.h"

namespace notes_page_interactions {
namespace {

using page_navigation::NavigationItemRole;

}  // namespace

ActivateResult HandlePrimaryActivate(NotesPageCoordinator& coordinator)
{
    ActivateResult result = {};

    if (coordinator.IsRoleFocused(NavigationItemRole::kNotesPageTimelineGroup)) {
        result.handled = true;
        if (coordinator.item_list_active()) {
            // An item is selected -> open its actions modal.
            result.intent = ActivateIntent::kOpenItemActions;
            result.play_activate_cue = true;
        } else if (coordinator.EnterFocusedGroup()) {
            // Entered the group's item list; repaint to show the active state.
            result.play_activate_cue = true;
            result.apply_page_state = true;
        }
        return result;
    }

    result.handled = true;
    result.play_activate_cue = true;
    if (coordinator.IsRoleFocused(NavigationItemRole::kNotesPageVibeCheckButton)) {
        result.intent = ActivateIntent::kShowVibeCheck;
    } else if (coordinator.IsRoleFocused(NavigationItemRole::kNotesPageSummarizeButton)) {
        result.intent = ActivateIntent::kShowSummarize;
    } else if (coordinator.IsRoleFocused(NavigationItemRole::kFooterHome)) {
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
        case ActivateIntent::kOpenItemActions:
            if (callbacks.open_item_actions) {
                callbacks.open_item_actions();
            }
            break;
        case ActivateIntent::kShowVibeCheck:
            if (callbacks.show_vibe_check) {
                callbacks.show_vibe_check();
            }
            break;
        case ActivateIntent::kShowSummarize:
            if (callbacks.show_summarize) {
                callbacks.show_summarize();
            }
            break;
        case ActivateIntent::kNone:
        default:
            break;
    }
}

FocusMoveResult HandleMoveFocus(NotesPageCoordinator& coordinator, int delta)
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

}  // namespace notes_page_interactions

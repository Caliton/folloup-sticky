#ifndef JOURNAL_PAGE_RUNTIME_H_
#define JOURNAL_PAGE_RUNTIME_H_

#include <cstdint>
#include <string>
#include <vector>

#include "display_service.h"
#include "esp_err.h"
#include "footer_runtime.h"
#include "overlay_runtime.h"
#include "page_action_result.h"

// The "Diário" (bullet journal) page: level switcher + timeline over journal_service items.
namespace journal_page_runtime {

enum class ActivateIntent : uint8_t {
    kNone = 0,
    kShowHome,
    kShowSettings,
    kShowToday,
    kOpenItemActions,
};

struct ActivateResult {
    ActivateIntent intent = ActivateIntent::kNone;
    bool handled = false;
    bool play_activate_cue = false;
    bool apply_page_state = false;
};

esp_err_t UpdateDisplayState();
esp_err_t UpdateDisplayStateAndRequestRefresh(
    display_service::RefreshMode refresh_mode = display_service::RefreshMode::kPartial);
esp_err_t UpdateDisplayStateAndRequestRefresh(
    const display_service::RefreshRequest& refresh_request);

page_actions::FocusMoveOutcome MoveFocus(int delta);
ActivateResult ActivateFocusedItem();

footer_runtime::ProjectionState BuildFooterProjectionState();
void ResetFocus();

// Reload the items (SD read on first use, then the PSRAM cache) and rebuild the page.
esp_err_t SyncFromStore(bool request_refresh_if_active);
// Page entry from the dashboard: open on the day view (review group first when needed).
void PrepareForShow();

// Leave an entered control (item list or level switcher). True if one was active.
bool ExitActiveControl();

// Item actions modal for the selected item; its submission (and the "Mudar tipo" follow-up
// modal) is dispatched by HandleItemActionSelection. Returns true when it handled the submit.
bool ShowItemActionsModal();
bool HandleItemActionSelection(int selected_index);
// Drop the page's rows (coordinator + display copy) once it is off screen: their strings
// live in internal RAM. The next PrepareForShow rebuilds them from the PSRAM store.
void Release();
// Recording id whose details were requested from the modal ("" when none).
std::string ConsumePendingViewDetails();

// Period key a take recorded on this screen is filed under ("" while the clock is unset).
std::string RecordingPeriod();
struct Summary {
    int today_tasks = 0;       // tasks filed under today (any status but cancelled)
    int today_tasks_done = 0;
    int pending = 0;           // open items whose period already ended
};
// Straight from the store (not the page), so the dashboard can use it before the page opens.
Summary ComputeSummary();
// The footer Sticky: today's journal on the e-paper -- a review reminder when something is
// pending, then today's open events, tasks and notes, one card each. Empty when nothing is due.
std::vector<overlay_runtime::StickyNoteItem> BuildTodayStickyItems();

}  // namespace journal_page_runtime

#endif  // JOURNAL_PAGE_RUNTIME_H_

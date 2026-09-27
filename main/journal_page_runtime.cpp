#include "journal_page_runtime.h"

#include <climits>
#include <mutex>
#include <vector>

#include "clip_playback_runtime.h"
#include "epaper_ui/select_modal.h"
#include "esp_log.h"
#include "journal_page_coordinator.h"
#include "journal_service.h"
#include "journal_view.h"
#include "overlay_runtime.h"
#include "page_navigation/page_focus_projection.h"
#include "project_assets.h"
#include "recording_archive_service.h"
#include "ui_refresh_runtime.h"

namespace journal_page_runtime {
namespace {

using page_navigation::NavigationItemRole;

constexpr const char* kTag = "JournalPageRuntime";
constexpr size_t kModalTitleMaxBytes = 30;
// A row shows one line; keep only that much of the text (see journal_view::ViewItem).
constexpr size_t kViewTextMaxBytes = 64;

enum class PendingModal : uint8_t {
    kNone = 0,
    kItemActions,
    kChangeType,
};

constexpr journal_service::ItemType kTypeChoices[] = {
    journal_service::ItemType::kTask,
    journal_service::ItemType::kNote,
    journal_service::ItemType::kEvent,
};

std::mutex s_mutex;
JournalPageCoordinator s_coordinator = {};
PendingModal s_pending_modal = PendingModal::kNone;
journal_view::ViewItem s_modal_item = {};
std::vector<journal_view::ActionOption> s_modal_actions = {};
std::string s_pending_view_details_id = {};

footer_runtime::FooterFocusItem FooterItemForSelectedIndex(int selected_index)
{
    switch (selected_index) {
        case 0:
            return footer_runtime::FooterFocusItem::kHome;
        case 1:
            return footer_runtime::FooterFocusItem::kSettings;
        case 2:
            return footer_runtime::FooterFocusItem::kToday;
        case 4:
            return footer_runtime::FooterFocusItem::kSticky;
        default:
            return footer_runtime::FooterFocusItem::kNone;
    }
}

footer_runtime::ProjectionState BuildFooterProjectionStateLocked()
{
    const page_navigation::PageFocusProjection projection = page_navigation::ProjectPageFocus(
        s_coordinator.navigation_model(),
        page_navigation::NavigationItemSection::kJournalPageTimelineGroups,
        s_coordinator.focus().index(), -1, -1);
    footer_runtime::ProjectionState state = {};
    state.focused_item = FooterItemForSelectedIndex(projection.footer_selected_index);
    return state;
}

std::string Excerpt(const journal_service::PString& text, size_t max_bytes)
{
    if (text.size() <= max_bytes) {
        return std::string(text.data(), text.size());
    }
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(text.data(), cut) + "...";
}

// Only what any level of this year's journal can show: this year's items plus the review
// queue. Other years never appear on the device (the web app plans ahead further).
std::vector<journal_view::ViewItem> LoadViewItems(const journal_period::Date& today)
{
    std::vector<journal_view::ViewItem> view_items;
    if (!journal_period::IsValid(today)) {
        return view_items;
    }
    const std::string year = journal_period::YearKey(today);
    for (const journal_service::Item& item : journal_service::ListItems()) {
        const std::string period(item.period.data(), item.period.size());
        const std::string planned(item.planned.data(), item.planned.size());
        const bool relevant = journal_service::IsPending(item, today) ||
                              journal_period::Contains(year, period) ||
                              journal_period::Contains(year, planned);
        if (!relevant) {
            continue;
        }
        view_items.push_back({
            .id = std::string(item.id.data(), item.id.size()),
            .type = item.type,
            .status = item.status,
            .text = Excerpt(item.text, kViewTextMaxBytes),
            .period = period,
            .planned = planned,
            .has_recording = !item.recording_id.empty(),
            .created_at = item.created_at,
        });
    }
    return view_items;
}

std::string RecordingIdOf(const std::string& item_id)
{
    journal_service::Item item = {};
    if (!journal_service::FindItem(item_id, &item)) {
        return {};
    }
    return std::string(item.recording_id.data(), item.recording_id.size());
}

journal_period::Date TodayOrInvalid()
{
    journal_period::Date today = {};
    if (!journal_period::Today(&today)) {
        return {};
    }
    return today;
}

// The modal title: the item's own words, cut to fit one line.
std::string ModalTitle(const std::string& text)
{
    if (text.size() <= kModalTitleMaxBytes) {
        return text;
    }
    size_t cut = kModalTitleMaxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut) + "...";
}

bool ShowTypeModal()
{
    epaper_ui::SelectModalState modal = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        modal.title_text = "Mudar para";
        for (journal_service::ItemType type : kTypeChoices) {
            modal.items.push_back({journal_service::TypeLabel(type)});
        }
        modal.items.push_back({"Fechar"});
        modal.selected_index = 0;
        s_pending_modal = PendingModal::kChangeType;
    }
    const esp_err_t err = overlay_runtime::ShowSelectModal(modal);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pending_modal = PendingModal::kNone;
        ESP_LOGW(kTag, "Show type modal failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void RunAction(const journal_view::ActionOption& option, const journal_view::ViewItem& item)
{
    using journal_view::Action;
    bool ok = true;
    switch (option.action) {
        case Action::kComplete:
            ok = journal_service::SetStatus(item.id, journal_service::ItemStatus::kDone);
            break;
        case Action::kReopen:
            ok = journal_service::SetStatus(item.id, journal_service::ItemStatus::kOpen);
            break;
        case Action::kCancel:
            ok = journal_service::SetStatus(item.id, journal_service::ItemStatus::kCancelled);
            break;
        case Action::kMoveTo:
            ok = journal_service::MoveTo(item.id, option.target_period);
            break;
        case Action::kDelete:
            ok = journal_service::Delete(item.id);
            break;
        case Action::kChangeType:
            (void)ShowTypeModal();
            break;
        case Action::kPlayRecording: {
            // Streams on a worker; the modal closes right away instead of blocking the input
            // chain for the length of the clip.
            const std::string path =
                recording_archive_service::ResolveRecordingPath(RecordingIdOf(item.id));
            ok = !path.empty() && clip_playback_runtime::PlayFileAsync(path);
            break;
        }
        case Action::kViewDetails: {
            const std::string recording_id = RecordingIdOf(item.id);
            std::lock_guard<std::mutex> lock(s_mutex);
            s_pending_view_details_id = recording_id;
            break;
        }
        case Action::kClose:
        default:
            break;
    }
    if (!ok) {
        ESP_LOGW(kTag, "Journal action %d failed for %s", static_cast<int>(option.action),
                 item.id.c_str());
    }
    // Store mutations notify app_shell's journal listener, which re-syncs this page.
}

}  // namespace

esp_err_t UpdateDisplayState()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return display_service::SetJournalPageState(s_coordinator.BuildState());
}

esp_err_t UpdateDisplayStateAndRequestRefresh(display_service::RefreshMode refresh_mode)
{
    return UpdateDisplayStateAndRequestRefresh(display_service::RefreshRequest{
        .refresh_mode = refresh_mode,
    });
}

esp_err_t UpdateDisplayStateAndRequestRefresh(
    const display_service::RefreshRequest& refresh_request)
{
    return ui_refresh_runtime::Schedule(ui_refresh_runtime::SurfaceKey::kJournalPage,
                                        &UpdateDisplayState, refresh_request);
}

page_actions::FocusMoveOutcome MoveFocus(int delta)
{
    page_actions::FocusMoveOutcome result = {};
    std::lock_guard<std::mutex> lock(s_mutex);
    const footer_runtime::FooterFocusItem old_footer =
        BuildFooterProjectionStateLocked().focused_item;
    if (!s_coordinator.MoveFocus(delta)) {
        return result;
    }
    result.handled = true;
    result.play_navigation_cue = true;
    result.apply_page_state = true;
    result.sync_footer_projection =
        BuildFooterProjectionStateLocked().focused_item != old_footer;
    return result;
}

ActivateResult ActivateFocusedItem()
{
    ActivateResult result = {};
    std::lock_guard<std::mutex> lock(s_mutex);
    result.handled = true;
    result.play_activate_cue = true;

    switch (s_coordinator.FocusedRole()) {
        case NavigationItemRole::kJournalPageSegmentControl:
            // OK enters the switcher (UP/DOWN then flips the level); OK again leaves it.
            if (s_coordinator.segment_control_active()) {
                s_coordinator.ExitSegmentControl();
            } else {
                s_coordinator.EnterSegmentControl();
            }
            result.apply_page_state = true;
            return result;
        case NavigationItemRole::kJournalPageTimelineGroup:
            if (s_coordinator.item_list_active()) {
                result.intent = ActivateIntent::kOpenItemActions;
            } else if (s_coordinator.EnterFocusedGroup()) {
                result.apply_page_state = true;
            } else {
                result.play_activate_cue = false;
            }
            return result;
        case NavigationItemRole::kFooterHome:
            result.intent = ActivateIntent::kShowHome;
            return result;
        case NavigationItemRole::kFooterSettings:
            result.intent = ActivateIntent::kShowSettings;
            return result;
        case NavigationItemRole::kFooterToday:
            result.intent = ActivateIntent::kShowToday;
            return result;
        default:
            result.handled = false;
            result.play_activate_cue = false;
            return result;
    }
}

footer_runtime::ProjectionState BuildFooterProjectionState()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return BuildFooterProjectionStateLocked();
}

void ResetFocus()
{
    footer_runtime::ProjectionState projection = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        projection = BuildFooterProjectionStateLocked();
    }
    footer_runtime::SetProjectionState(projection);
}

esp_err_t SyncFromStore(bool request_refresh_if_active)
{
    const journal_period::Date today = TodayOrInvalid();
    std::vector<journal_view::ViewItem> items = LoadViewItems(today);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_coordinator.SetItems(std::move(items), today);
    }
    const esp_err_t err =
        request_refresh_if_active
            ? UpdateDisplayStateAndRequestRefresh(display_service::RefreshMode::kPartial)
            : UpdateDisplayState();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Journal sync failed: %s", esp_err_to_name(err));
    }
    return err;
}

void PrepareForShow()
{
    const journal_period::Date today = TodayOrInvalid();
    std::vector<journal_view::ViewItem> items = LoadViewItems(today);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_coordinator.SetItems(std::move(items), today);
    s_coordinator.PrepareForShow(true);
}

bool ExitActiveControl()
{
    bool exited = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        exited = s_coordinator.ExitItemList() || s_coordinator.ExitSegmentControl();
    }
    if (exited) {
        (void)UpdateDisplayStateAndRequestRefresh(display_service::RefreshMode::kPartial);
    }
    return exited;
}

bool ShowItemActionsModal()
{
    epaper_ui::SelectModalState modal = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const journal_view::ViewItem* item = s_coordinator.SelectedItem();
        if (item == nullptr) {
            return false;
        }
        s_modal_item = *item;
        s_modal_actions = journal_view::BuildActions(*item, s_coordinator.today());
        modal.title_text = ModalTitle(item->text);
        for (const journal_view::ActionOption& option : s_modal_actions) {
            modal.items.push_back({option.label});
        }
        modal.selected_index = 0;
        s_pending_modal = PendingModal::kItemActions;
    }
    const esp_err_t err = overlay_runtime::ShowSelectModal(modal);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pending_modal = PendingModal::kNone;
        ESP_LOGW(kTag, "Show item-actions modal failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool HandleItemActionSelection(int selected_index)
{
    PendingModal pending = PendingModal::kNone;
    journal_view::ViewItem item = {};
    journal_view::ActionOption option = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        pending = s_pending_modal;
        if (pending == PendingModal::kNone) {
            return false;
        }
        s_pending_modal = PendingModal::kNone;
        item = s_modal_item;
        if (pending == PendingModal::kItemActions) {
            if (selected_index < 0 || selected_index >= static_cast<int>(s_modal_actions.size())) {
                return true;  // dismissed
            }
            option = s_modal_actions[static_cast<size_t>(selected_index)];
        }
    }

    if (pending == PendingModal::kChangeType) {
        constexpr int kTypeCount = static_cast<int>(sizeof(kTypeChoices) / sizeof(kTypeChoices[0]));
        if (selected_index >= 0 && selected_index < kTypeCount) {
            (void)journal_service::SetType(item.id, kTypeChoices[selected_index]);
        }
        return true;
    }
    RunAction(option, item);
    return true;
}

void Release()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_coordinator = JournalPageCoordinator();
        s_modal_actions.clear();
        s_modal_actions.shrink_to_fit();
        s_modal_item = {};
    }
    (void)display_service::SetJournalPageState({});
}

std::string ConsumePendingViewDetails()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    std::string id;
    id.swap(s_pending_view_details_id);
    return id;
}

std::string RecordingPeriod()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_coordinator.RecordingPeriod();
}

Summary ComputeSummary()
{
    Summary summary = {};
    journal_period::Date today = {};
    if (!journal_period::Today(&today)) {
        return summary;
    }
    const std::string today_key = journal_period::DayKey(today);
    for (const journal_service::Item& item : journal_service::ListItems()) {
        if (journal_service::IsPending(item, today)) {
            ++summary.pending;
        }
        const bool today_task = item.type == journal_service::ItemType::kTask &&
                                item.status != journal_service::ItemStatus::kCancelled &&
                                item.period.size() == today_key.size() &&
                                today_key.compare(0, today_key.size(), item.period.data(),
                                                  item.period.size()) == 0;
        if (today_task) {
            ++summary.today_tasks;
            if (item.status == journal_service::ItemStatus::kDone) {
                ++summary.today_tasks_done;
            }
        }
    }
    return summary;
}

std::vector<overlay_runtime::StickyNoteItem> BuildTodayStickyItems()
{
    std::vector<overlay_runtime::StickyNoteItem> cards;
    journal_period::Date today = {};
    if (!journal_period::Today(&today)) {
        return cards;
    }
    const std::string today_key = journal_period::DayKey(today);
    const std::string today_label = journal_period::Label(today_key, today);  // "Hoje"
    const journal_service::ItemList items = journal_service::ListItems();

    int pending = 0;
    for (const journal_service::Item& item : items) {
        if (journal_service::IsPending(item, today)) {
            ++pending;
        }
    }
    if (pending > 0) {
        overlay_runtime::StickyNoteItem card = {};
        card.date_text = today_label;
        card.header.tag_text = "Revisar";
        card.body_text = std::to_string(pending) +
                         (pending == 1 ? " item de dias anteriores espera"
                                       : " itens de dias anteriores esperam") +
                         " revisão em Diário > Pendentes.";
        cards.push_back(std::move(card));
    }

    // Events first (they happen at a time), then tasks, then notes; creation order within each.
    for (journal_service::ItemType type :
         {journal_service::ItemType::kEvent, journal_service::ItemType::kTask,
          journal_service::ItemType::kNote}) {
        for (const journal_service::Item& item : items) {
            const bool due_today =
                item.period.size() == today_key.size() &&
                today_key.compare(0, today_key.size(), item.period.data(), item.period.size()) == 0;
            if (!due_today || item.type != type || item.status != journal_service::ItemStatus::kOpen) {
                continue;
            }
            overlay_runtime::StickyNoteItem card = {};
            card.date_text = today_label;
            card.header.icon_asset = item.recording_id.empty()
                                         ? nullptr
                                         : project_assets::GetIcon(EmbeddedIconId::kAudio);
            card.header.tag_text = journal_service::TypeLabel(item.type);
            card.body_text = std::string(item.text.data(), item.text.size());
            cards.push_back(std::move(card));
        }
    }
    return cards;
}

}  // namespace journal_page_runtime

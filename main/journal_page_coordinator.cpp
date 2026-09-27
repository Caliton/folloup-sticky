#include "journal_page_coordinator.h"

#include <algorithm>

namespace {

using page_navigation::NavigationItemRole;
using page_navigation::NavigationItemSection;

constexpr int kSegmentFocusIndex = 0;  // the switcher is the first navigation item

// Stable identity of a group across rebuilds (the review group has no period key).
std::string GroupId(const journal_view::Group& group)
{
    return group.review ? std::string("#review") : group.key;
}

}  // namespace

JournalPageCoordinator::JournalPageCoordinator() = default;

void JournalPageCoordinator::SetItems(std::vector<journal_view::ViewItem> items,
                                      const journal_period::Date& today)
{
    items_ = std::move(items);
    today_ = today;
    Rebuild(true);
}

void JournalPageCoordinator::PrepareForShow(bool prefer_day_view)
{
    segment_control_active_ = false;
    item_list_active_ = false;
    active_group_index_ = -1;
    selected_item_id_.clear();
    if (prefer_day_view) {
        level_ = journal_view::Level::kDay;
    }
    Rebuild(false);
    visible_group_index_ = 0;
    // With something to review, land on the review group so OK goes straight into it.
    const bool review_first = level_ == journal_view::Level::kDay && model_.pending_count > 0 &&
                              !model_.groups.empty() && model_.groups.front().review;
    focus_.Configure(navigation_model_.item_count,
                     review_first ? kSegmentFocusIndex + 1 : kSegmentFocusIndex);
}

void JournalPageCoordinator::Rebuild(bool keep_selection)
{
    // Remember what had focus so a data refresh (sync, a completed task) doesn't reset it.
    const NavigationItemRole old_role = FocusedRole();
    const int old_group = FocusedGroupIndex();
    std::string old_group_id;
    if (old_group >= 0 && old_group < GroupCount()) {
        old_group_id = GroupId(model_.groups[static_cast<size_t>(old_group)]);
    }
    const std::string old_item = item_list_active_ ? selected_item_id_ : std::string();

    model_ = journal_view::Build(items_, level_, today_);
    navigation_model_ = page_navigation::BuildJournalPageNavigationModel(GroupCount());
    item_list_active_ = false;
    active_group_index_ = -1;
    item_focus_.Configure(0, 0);
    focus_.Configure(navigation_model_.item_count, kSegmentFocusIndex);
    visible_group_index_ = std::clamp(visible_group_index_, 0, std::max(0, GroupCount() - 1));

    if (!keep_selection) {
        selected_item_id_.clear();
        return;
    }
    if (!old_item.empty() && FocusItem(old_item)) {
        return;
    }
    selected_item_id_.clear();
    if (!old_group_id.empty()) {
        for (int index = 0; index < GroupCount(); ++index) {
            if (GroupId(model_.groups[static_cast<size_t>(index)]) == old_group_id) {
                SetFocusIndex(kSegmentFocusIndex + 1 + index);
                return;
            }
        }
    }
    if (old_role != NavigationItemRole::kUnknown &&
        old_role != NavigationItemRole::kJournalPageTimelineGroup) {
        const int index = navigation_model_.IndexOfRole(old_role);
        if (index >= 0) {
            focus_.SetIndex(index);
        }
    }
}

int JournalPageCoordinator::FocusedGroupIndex() const
{
    const page_navigation::NavigationItemDescriptor* item =
        navigation_model_.ItemAt(focus_.index());
    if (item == nullptr || item->role != NavigationItemRole::kJournalPageTimelineGroup) {
        return -1;
    }
    return item->item_index >= 0 && item->item_index < GroupCount() ? item->item_index : -1;
}

bool JournalPageCoordinator::FocusItem(const std::string& item_id)
{
    for (int group = 0; group < GroupCount(); ++group) {
        const std::vector<journal_view::Entry>& entries =
            model_.groups[static_cast<size_t>(group)].entries;
        for (size_t entry = 0; entry < entries.size(); ++entry) {
            if (entries[entry].item_id == item_id) {
                focus_.SetIndex(kSegmentFocusIndex + 1 + group);
                visible_group_index_ = group;
                item_list_active_ = true;
                active_group_index_ = group;
                item_focus_.Configure(static_cast<int>(entries.size()), static_cast<int>(entry));
                selected_item_id_ = item_id;
                return true;
            }
        }
    }
    return false;
}

bool JournalPageCoordinator::MoveFocus(int delta)
{
    if (delta == 0) {
        return false;
    }
    if (segment_control_active_) {
        // UP/DOWN switches the level live, like flipping between the journal's sections.
        if (!segment_focus_.Move(delta)) {
            return false;
        }
        level_ = static_cast<journal_view::Level>(segment_focus_.index());
        visible_group_index_ = 0;
        Rebuild(false);
        return true;
    }
    if (item_list_active_) {
        if (!item_focus_.Move(delta)) {
            return false;
        }
        const std::vector<journal_view::Entry>& entries =
            model_.groups[static_cast<size_t>(active_group_index_)].entries;
        selected_item_id_ = entries[static_cast<size_t>(item_focus_.index())].item_id;
        visible_group_index_ = active_group_index_;
        return true;
    }
    if (!focus_.Move(delta)) {
        return false;
    }
    const int group = FocusedGroupIndex();
    if (group >= 0) {
        visible_group_index_ = group;
    }
    return true;
}

bool JournalPageCoordinator::SetFocusIndex(int index)
{
    if (!focus_.SetIndex(index)) {
        return false;
    }
    const int group = FocusedGroupIndex();
    if (group >= 0) {
        visible_group_index_ = group;
    }
    return true;
}

bool JournalPageCoordinator::IsRoleFocused(NavigationItemRole role) const
{
    return navigation_model_.IsRoleSelected(focus_.index(), role);
}

NavigationItemRole JournalPageCoordinator::FocusedRole() const
{
    const page_navigation::NavigationItemDescriptor* item =
        navigation_model_.ItemAt(focus_.index());
    return item != nullptr ? item->role : NavigationItemRole::kUnknown;
}

bool JournalPageCoordinator::EnterSegmentControl()
{
    if (segment_control_active_) {
        return false;
    }
    segment_control_active_ = true;
    segment_focus_.Configure(journal_view::kLevelCount, static_cast<int>(level_));
    return true;
}

bool JournalPageCoordinator::ExitSegmentControl()
{
    if (!segment_control_active_) {
        return false;
    }
    segment_control_active_ = false;
    return true;
}

bool JournalPageCoordinator::EnterFocusedGroup()
{
    if (item_list_active_) {
        return false;
    }
    const int group = FocusedGroupIndex();
    if (group < 0 || model_.groups[static_cast<size_t>(group)].entries.empty()) {
        return false;
    }
    item_list_active_ = true;
    active_group_index_ = group;
    visible_group_index_ = group;
    const std::vector<journal_view::Entry>& entries =
        model_.groups[static_cast<size_t>(group)].entries;
    item_focus_.Configure(static_cast<int>(entries.size()), 0);
    selected_item_id_ = entries.front().item_id;
    return true;
}

bool JournalPageCoordinator::ExitItemList()
{
    if (!item_list_active_) {
        return false;
    }
    item_list_active_ = false;
    active_group_index_ = -1;
    item_focus_.Configure(0, 0);
    selected_item_id_.clear();
    return true;
}

const journal_view::ViewItem* JournalPageCoordinator::SelectedItem() const
{
    if (!item_list_active_ || selected_item_id_.empty()) {
        return nullptr;
    }
    for (const journal_view::ViewItem& item : items_) {
        if (item.id == selected_item_id_) {
            return &item;
        }
    }
    return nullptr;
}

std::string JournalPageCoordinator::RecordingPeriod() const
{
    if (!journal_period::IsValid(today_)) {
        return {};
    }
    return journal_view::CurrentKey(level_, today_);
}

epaper_ui::JournalPageState JournalPageCoordinator::BuildState() const
{
    journal_view::FocusState focus = {};
    focus.navigation_focus_index = focus_.index();
    focus.segment_focused = IsRoleFocused(NavigationItemRole::kJournalPageSegmentControl);
    focus.segment_active = segment_control_active_;
    focus.visible_group = GroupCount() > 0 ? visible_group_index_ : -1;
    focus.focused_group = FocusedGroupIndex();
    focus.active_group = item_list_active_ ? active_group_index_ : -1;
    focus.selected_item = item_list_active_ ? item_focus_.index() : -1;
    epaper_ui::JournalPageState state = journal_view::ToPageState(model_, level_, focus);
    if (!journal_period::IsValid(today_)) {
        state.subtitle_text = "Acerte a data para usar o diário";
    }
    return state;
}

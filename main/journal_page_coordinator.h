#ifndef JOURNAL_PAGE_COORDINATOR_H_
#define JOURNAL_PAGE_COORDINATOR_H_

#include <string>
#include <vector>

#include "epaper_ui/journal_page.h"
#include "journal_view.h"
#include "page_navigation/navigation_model.h"
#include "page_navigation/roving_focus.h"

// Owns the Diário page's data + focus. Top-level focus roves the level switcher, the timeline
// group chips and the footer. Entering the switcher lets UP/DOWN change the level live;
// entering a group activates a second focus level over its items (same model as Notes).
class JournalPageCoordinator {
public:
    JournalPageCoordinator();

    // Replace the items and rebuild the current level, keeping the selection when possible.
    void SetItems(std::vector<journal_view::ViewItem> items, const journal_period::Date& today);
    // (Re)enter the page: focus the review group when there are pending items, else the
    // switcher; collapse any entered control.
    void PrepareForShow(bool prefer_day_view);

    bool MoveFocus(int delta);
    bool SetFocusIndex(int index);
    bool IsRoleFocused(page_navigation::NavigationItemRole role) const;
    page_navigation::NavigationItemRole FocusedRole() const;

    bool EnterSegmentControl();
    bool ExitSegmentControl();
    bool EnterFocusedGroup();
    bool ExitItemList();

    bool segment_control_active() const { return segment_control_active_; }
    bool item_list_active() const { return item_list_active_; }
    journal_view::Level level() const { return level_; }
    int pending_count() const { return model_.pending_count; }
    const journal_period::Date& today() const { return today_; }

    // The item under the cursor in an entered group (nullptr otherwise).
    const journal_view::ViewItem* SelectedItem() const;
    // Period a take recorded on this screen is filed under.
    std::string RecordingPeriod() const;

    epaper_ui::JournalPageState BuildState() const;

    const page_navigation::NavigationModel& navigation_model() const { return navigation_model_; }
    const page_navigation::RovingFocus& focus() const { return focus_; }

private:
    void Rebuild(bool keep_selection);
    int FocusedGroupIndex() const;
    int GroupCount() const { return static_cast<int>(model_.groups.size()); }
    bool FocusItem(const std::string& item_id);

    std::vector<journal_view::ViewItem> items_ = {};
    journal_period::Date today_ = {};
    journal_view::Level level_ = journal_view::Level::kDay;
    journal_view::Model model_ = {};
    page_navigation::NavigationModel navigation_model_ =
        page_navigation::BuildJournalPageNavigationModel(0);
    page_navigation::RovingFocus focus_{navigation_model_.item_count, 0};
    page_navigation::RovingFocus item_focus_{0, 0};
    page_navigation::RovingFocus segment_focus_{journal_view::kLevelCount, 3};
    bool segment_control_active_ = false;
    bool item_list_active_ = false;
    int active_group_index_ = -1;
    int visible_group_index_ = 0;
    std::string selected_item_id_ = {};
};

#endif  // JOURNAL_PAGE_COORDINATOR_H_

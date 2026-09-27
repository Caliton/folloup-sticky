#ifndef JOURNAL_VIEW_H_
#define JOURNAL_VIEW_H_

#include <cstdint>
#include <string>
#include <vector>

#include "epaper_ui/journal_page.h"
#include "epaper_ui/list_item.h"
#include "journal_period.h"
#include "journal_types.h"

// View model of the "Diário" page: turns journal items into the timeline groups of one level
// (Ano / Mês / Semana / Dia) and the actions offered for an item. Pure C++ (no ESP-IDF), so
// scripts/preview_screens.py renders exactly what the device shows.
namespace journal_view {

// Segment order on screen.
enum class Level : uint8_t {
    kYear = 0,
    kMonth,
    kWeek,
    kDay,
};
inline constexpr int kLevelCount = 4;

// What the page needs of an item. Kept small on purpose: every string longer than the SSO
// buffer lands in scarce internal RAM, so the text is a one-line excerpt (the row shows one
// line anyway) and the recording is only flagged -- actions look the item up in the store.
struct ViewItem {
    std::string id = {};
    journal_service::ItemType type = journal_service::ItemType::kTask;
    journal_service::ItemStatus status = journal_service::ItemStatus::kOpen;
    std::string text = {};
    std::string period = {};
    std::string planned = {};
    bool has_recording = false;
    int64_t created_at = 0;
};

// Rows materialized per group (the rest stays in the store; see ViewItem).
inline constexpr size_t kMaxRowsPerGroup = 20;

struct Entry {
    std::string item_id = {};
    epaper_ui::ListItemState ui = {};
};

struct Group {
    // Period key the group stands for ("" for the review group).
    std::string key = {};
    std::string label = {};
    bool review = false;
    std::vector<Entry> entries = {};
};

struct Model {
    std::string subtitle = {};
    std::vector<Group> groups = {};
    int pending_count = 0;
};

const char* LevelName(Level level);  // "Ano" | "Mês" | "Semana" | "Dia"
// The period a new item recorded on this level's screen is filed under.
std::string CurrentKey(Level level, const journal_period::Date& today);
Model Build(const std::vector<ViewItem>& items, Level level, const journal_period::Date& today);

enum class Action : uint8_t {
    kComplete,
    kReopen,
    kMoveTo,  // target_period
    kChangeType,
    kPlayRecording,
    kViewDetails,
    kCancel,
    kDelete,
    kClose,
};

struct ActionOption {
    Action action = Action::kClose;
    std::string label = {};
    std::string target_period = {};
};

std::vector<ActionOption> BuildActions(const ViewItem& item, const journal_period::Date& today);

// Focus as tracked by the page coordinator, projected onto the drawable page state.
struct FocusState {
    int navigation_focus_index = -1;
    bool segment_focused = false;
    bool segment_active = false;
    int visible_group = -1;
    int focused_group = -1;
    int active_group = -1;
    int selected_item = -1;
};

epaper_ui::JournalPageState ToPageState(const Model& model, Level level, const FocusState& focus);

}  // namespace journal_view

#endif  // JOURNAL_VIEW_H_

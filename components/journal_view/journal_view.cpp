#include "journal_view.h"

#include <algorithm>

#include "project_assets.h"

namespace journal_view {
namespace {

using journal_period::Date;
using PeriodLevel = journal_period::Level;
using journal_service::ItemStatus;
using journal_service::ItemType;

// Latin-1 only (the e-paper fonts stop at U+00FF): "»" stands in for the BuJo ">" migration mark.
constexpr const char* kMigratedMark = "» ";

constexpr const char* kWeekdayNames[7] = {"Segunda", "Terça", "Quarta", "Quinta",
                                          "Sexta",   "Sábado", "Domingo"};
constexpr const char* kMonthNamesLower[12] = {
    "janeiro", "fevereiro", "março",    "abril",   "maio",     "junho",
    "julho",   "agosto",    "setembro", "outubro", "novembro", "dezembro",
};

const char* TypeLabel(ItemType type)
{
    switch (type) {
        case ItemType::kNote:
            return "Nota";
        case ItemType::kEvent:
            return "Evento";
        case ItemType::kTask:
        default:
            return "Tarefa";
    }
}

Date FirstDay(const std::string& key)
{
    Date first = {};
    journal_period::Range(key, &first, nullptr);
    return first;
}

bool IsOpen(const ViewItem& item)
{
    return item.status == ItemStatus::kOpen;
}

bool IsPending(const ViewItem& item, const Date& today)
{
    return IsOpen(item) && journal_period::HasEnded(item.period, today);
}

epaper_ui::ListItemState MakeRow(const ViewItem& item, const std::string& mark)
{
    epaper_ui::ListItemState row = {};
    row.header.icon_asset =
        item.has_recording ? project_assets::GetIcon(EmbeddedIconId::kAudio) : nullptr;
    row.header.time_text = item.status == ItemStatus::kCancelled ? "Cancelada" : mark;
    row.header.tag_text = TypeLabel(item.type);
    row.body_text = item.text;
    switch (item.type) {
        case ItemType::kTask:
            row.accessory.kind = epaper_ui::ListItemAccessoryKind::kCheckbox;
            row.accessory.checked = item.status == ItemStatus::kDone;
            break;
        case ItemType::kEvent:
            row.accessory.kind = epaper_ui::ListItemAccessoryKind::kIcon;
            row.accessory.icon_asset = project_assets::GetIcon(EmbeddedIconId::kTime);
            break;
        case ItemType::kNote:
        default:
            row.accessory.kind = epaper_ui::ListItemAccessoryKind::kIcon;
            row.accessory.icon_asset = project_assets::GetIcon(EmbeddedIconId::kFileMd);
            break;
    }
    return row;
}

// Chronological by creation inside a group.
std::vector<const ViewItem*> Sorted(std::vector<const ViewItem*> items)
{
    std::stable_sort(items.begin(), items.end(), [](const ViewItem* a, const ViewItem* b) {
        return a->created_at < b->created_at;
    });
    return items;
}

// Items living in `key`, plus (with `show_migrated`) the ones first planned there and since
// pulled into a narrower period -- shown with a "» S40" mark like a migrated BuJo bullet.
Group PeriodGroup(const std::vector<ViewItem>& items, const std::string& key,
                  const std::string& label, bool show_migrated, const Date& today)
{
    Group group = {};
    group.key = key;
    group.label = label;
    std::vector<const ViewItem*> members;
    for (const ViewItem& item : items) {
        const bool lives_here = item.period == key;
        const bool migrated_from_here = show_migrated && item.planned == key && item.period != key;
        if (lives_here || migrated_from_here) {
            members.push_back(&item);
        }
    }
    for (const ViewItem* item : Sorted(members)) {
        if (group.entries.size() >= kMaxRowsPerGroup) {
            break;
        }
        std::string mark;
        if (item->period != key) {
            mark = kMigratedMark + journal_period::ShortLabel(item->period, today);
        }
        group.entries.push_back({item->id, MakeRow(*item, mark)});
    }
    return group;
}

Group ReviewGroup(const std::vector<ViewItem>& items, const Date& today)
{
    Group group = {};
    group.review = true;
    std::vector<const ViewItem*> members;
    for (const ViewItem& item : items) {
        if (IsPending(item, today)) {
            members.push_back(&item);
        }
    }
    std::stable_sort(members.begin(), members.end(), [](const ViewItem* a, const ViewItem* b) {
        const Date first_a = FirstDay(a->period);
        const Date first_b = FirstDay(b->period);
        if (!(first_a == first_b)) {
            return journal_period::Before(first_a, first_b);
        }
        return a->created_at < b->created_at;
    });
    group.label = "Pendentes";
    if (members.size() > kMaxRowsPerGroup) {
        members.resize(kMaxRowsPerGroup);
    }
    for (const ViewItem* item : members) {
        group.entries.push_back(
            {item->id, MakeRow(*item, journal_period::ShortLabel(item->period, today))});
    }
    return group;
}

// Distinct narrower periods (weeks / days) that hold items and fall inside `outer`,
// in calendar order.
std::vector<std::string> InnerPeriods(const std::vector<ViewItem>& items, const std::string& outer,
                                      bool include_weeks)
{
    std::vector<std::string> keys;
    for (const ViewItem& item : items) {
        const PeriodLevel level = journal_period::LevelOf(item.period);
        const bool candidate =
            level == PeriodLevel::kDay || (include_weeks && level == PeriodLevel::kWeek);
        if (!candidate) {
            continue;
        }
        const bool inside = level == PeriodLevel::kWeek
                                ? journal_period::ParentOf(item.period) == outer
                                : journal_period::Contains(outer, item.period);
        if (inside && std::find(keys.begin(), keys.end(), item.period) == keys.end()) {
            keys.push_back(item.period);
        }
    }
    std::stable_sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) {
        const Date first_a = FirstDay(a);
        const Date first_b = FirstDay(b);
        if (!(first_a == first_b)) {
            return journal_period::Before(first_a, first_b);
        }
        // Same start: the week before its Monday.
        return journal_period::LevelOf(a) == PeriodLevel::kWeek && journal_period::LevelOf(b) == PeriodLevel::kDay;
    });
    return keys;
}

std::string DaySubtitle(const Date& today)
{
    return std::string(kWeekdayNames[journal_period::Weekday(today)]) + ", " +
           std::to_string(today.day) + " de " + kMonthNamesLower[today.month - 1];
}

std::string MoveLabel(const std::string& prefix, const std::string& target, const Date& today)
{
    return prefix + journal_period::ShortLabel(target, today);
}

}  // namespace

const char* LevelName(Level level)
{
    switch (level) {
        case Level::kYear:
            return "Ano";
        case Level::kMonth:
            return "Mês";
        case Level::kWeek:
            return "Semana";
        case Level::kDay:
        default:
            return "Dia";
    }
}

std::string CurrentKey(Level level, const Date& today)
{
    switch (level) {
        case Level::kYear:
            return journal_period::YearKey(today);
        case Level::kMonth:
            return journal_period::MonthKey(today);
        case Level::kWeek:
            return journal_period::WeekKey(today);
        case Level::kDay:
        default:
            return journal_period::DayKey(today);
    }
}

Model Build(const std::vector<ViewItem>& items, Level level, const Date& today)
{
    Model model = {};
    if (!journal_period::IsValid(today)) {
        return model;  // clock never set: nothing can be placed in time yet
    }
    for (const ViewItem& item : items) {
        if (IsPending(item, today)) {
            ++model.pending_count;
        }
    }

    switch (level) {
        case Level::kDay: {
            model.subtitle = DaySubtitle(today);
            if (model.pending_count > 0) {
                model.groups.push_back(ReviewGroup(items, today));
            }
            model.groups.push_back(
                PeriodGroup(items, journal_period::DayKey(today), "Hoje", false, today));
            break;
        }
        case Level::kWeek: {
            const std::string week = journal_period::WeekKey(today);
            model.subtitle = journal_period::Label(week, today) + " · " +
                             journal_period::WeekRangeLabel(week);
            model.groups.push_back(PeriodGroup(items, week, "Esta semana", true, today));
            Date monday = {};
            journal_period::Range(week, &monday, nullptr);
            for (int offset = 0; offset < 7; ++offset) {
                const std::string day = journal_period::DayKey(journal_period::AddDays(monday, offset));
                Group group =
                    PeriodGroup(items, day, journal_period::Label(day, today), false, today);
                if (!group.entries.empty()) {
                    model.groups.push_back(std::move(group));
                }
            }
            break;
        }
        case Level::kMonth: {
            const std::string month = journal_period::MonthKey(today);
            model.subtitle = std::string(journal_period::MonthName(today.month)) + " " +
                             std::to_string(today.year);
            model.groups.push_back(PeriodGroup(items, month, "Este mês", true, today));
            for (const std::string& key : InnerPeriods(items, month, true)) {
                model.groups.push_back(
                    PeriodGroup(items, key, journal_period::Label(key, today), true, today));
            }
            break;
        }
        case Level::kYear:
        default: {
            const std::string year = journal_period::YearKey(today);
            model.subtitle = year;
            model.groups.push_back(PeriodGroup(items, year, "Este ano", true, today));
            for (int month = 1; month <= 12; ++month) {
                const std::string key = journal_period::MonthKey({today.year, month, 1});
                Group group = PeriodGroup(items, key, journal_period::MonthName(month), true, today);
                // The future log keeps every month still ahead, even empty, so there is
                // somewhere to file plans; past months only when they hold something.
                if (!group.entries.empty() || month >= today.month) {
                    model.groups.push_back(std::move(group));
                }
            }
            break;
        }
    }
    return model;
}

std::vector<ActionOption> BuildActions(const ViewItem& item, const Date& today)
{
    std::vector<ActionOption> actions;
    const std::string today_key = journal_period::DayKey(today);
    const std::string week_key = journal_period::WeekKey(today);
    const std::string month_key = journal_period::MonthKey(today);
    const PeriodLevel level = journal_period::LevelOf(item.period);
    const bool ended = journal_period::HasEnded(item.period, today);

    if (item.type == ItemType::kTask && item.status == ItemStatus::kOpen) {
        actions.push_back({Action::kComplete, "Concluir", {}});
    }
    if (item.status != ItemStatus::kOpen) {
        actions.push_back({Action::kReopen, "Reabrir", {}});
    }

    if (item.status == ItemStatus::kOpen) {
        if (item.period != today_key) {
            actions.push_back({Action::kMoveTo, "Fazer hoje", today_key});
        }
        const bool in_this_week =
            item.period == week_key ||
            (level == PeriodLevel::kDay && journal_period::Contains(week_key, item.period) && !ended);
        if (!in_this_week) {
            actions.push_back({Action::kMoveTo, "Fazer nesta semana", week_key});
        }
        const bool offer_month = item.period != month_key &&
                                 (level == PeriodLevel::kYear || ended ||
                                  !journal_period::Contains(month_key, item.period));
        if (offer_month) {
            actions.push_back({Action::kMoveTo, "Deixar para este mês", month_key});
        }
        if (!ended) {
            const std::string next = journal_period::NextOf(item.period);
            if (!next.empty()) {
                actions.push_back({Action::kMoveTo, MoveLabel("Adiar p/ ", next, today), next});
            }
        }
        if (!item.planned.empty() && item.planned != item.period &&
            journal_period::IsValidKey(item.planned) &&
            !journal_period::HasEnded(item.planned, today)) {
            actions.push_back(
                {Action::kMoveTo, MoveLabel("Devolver p/ ", item.planned, today), item.planned});
        }
    }

    actions.push_back({Action::kChangeType, "Mudar tipo", {}});
    if (item.has_recording) {
        actions.push_back({Action::kPlayRecording, "Ouvir gravação", {}});
        actions.push_back({Action::kViewDetails, "Ver detalhes", {}});
    }
    if (item.status == ItemStatus::kOpen) {
        actions.push_back({Action::kCancel, "Cancelar", {}});
    }
    actions.push_back({Action::kDelete, "Excluir", {}});
    actions.push_back({Action::kClose, "Fechar", {}});
    return actions;
}

epaper_ui::JournalPageState ToPageState(const Model& model, Level level, const FocusState& focus)
{
    epaper_ui::JournalPageState state = {};
    state.navigation_focus_index = focus.navigation_focus_index;
    state.subtitle_text = model.subtitle;

    state.segment_control.labels = {LevelName(Level::kYear), LevelName(Level::kMonth),
                                    LevelName(Level::kWeek), LevelName(Level::kDay)};
    state.segment_control.segment_count = kLevelCount;
    state.segment_control.selected_index = static_cast<int>(level);
    state.segment_control.focused = focus.segment_focused || focus.segment_active;
    state.segment_control.active = focus.segment_active;

    epaper_ui::TimelineListState& timeline = state.timeline;
    timeline.item_label_singular = "item";
    timeline.item_label_plural = "itens";
    timeline.empty_state_text = "Nada por aqui ainda";
    timeline.empty_state_icon_asset = project_assets::GetIcon(EmbeddedIconId::kTaskStart);
    timeline.visible_group_index = focus.visible_group;
    timeline.focused_group_index = focus.focused_group;
    timeline.active_group_index = focus.active_group;
    timeline.selected_item_index = focus.selected_item;
    timeline.groups.reserve(model.groups.size());
    for (const Group& group : model.groups) {
        epaper_ui::TimelineGroupState group_state = {};
        group_state.label_text = group.label;
        group_state.items.reserve(group.entries.size());
        for (const Entry& entry : group.entries) {
            group_state.items.push_back(entry.ui);
        }
        timeline.groups.push_back(std::move(group_state));
    }
    return state;
}

}  // namespace journal_view

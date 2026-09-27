#ifndef PAGE_NAVIGATION_NAVIGATION_MODEL_H_
#define PAGE_NAVIGATION_NAVIGATION_MODEL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace page_navigation {

enum class NavigationScope : uint8_t {
    kSettings = 0,
    kWifi,
    kTime,
    kDashboard,
    kVibeCheck,
    kSummarize,
    kNotes,
    kFollowUp,
    kDetails,
    kOnboarding,
    kBooks,
    kJournal,
};

enum class NavigationItemSection : uint8_t {
    kNone = 0,
    kFooter,
    kSettingsPageMenu,
    kWifiPageControls,
    kTimePageControls,
    kDashboardPageMenu,
    kVibeCheckPageControls,
    kSummarizePageControls,
    kNotesPageTimelineGroups,
    kFollowUpPageTimelineGroups,
    kDetailsPageControls,
    kOnboardingPageControls,
    kBooksPageList,
    kJournalPageControls,
    kJournalPageTimelineGroups,
};

enum class NavigationItemRole : uint8_t {
    kUnknown = 0,
    kFooterHome,
    kFooterSettings,
    kFooterWifi,
    kFooterTime,
    kFooterSticky,
    kSettingsWifiToggle,
    kSettingsEnableApToggle,
    kSettingsSoundToggle,
    kSettingsEnableOtgButton,
    kSettingsFormatSdButton,
    kSettingsManualOnboardingButton,
    kSettingsAppLinkButton,
    kWifiPageNetworkList,
    kWifiPagePasswordInput,
    kWifiPagePasswordVisibilityButton,
    kWifiPageScanButton,
    kWifiPageConnectButton,
    kTimePageTimezone,
    kTimePageHour,
    kTimePageMinute,
    kTimePageMeridiem,
    kTimePageMonth,
    kTimePageDay,
    kTimePageYear,
    kTimePageSave,
    kDashboardMenuItem,
    kVibeCheckPageCard,
    kSummarizePageSegmentControl,
    kSummarizePageScrollContainer,
    kSummarizePageGetSummaryButton,
    kNotesPageTimelineGroup,
    kNotesPageVibeCheckButton,
    kNotesPageSummarizeButton,
    kFollowUpPageTimelineGroup,
    kDetailsPageScrollContainer,
    kDetailsPageBackButton,
    kDetailsPageTranscribeButton,
    kOnboardingPageClose,
    kOnboardingPagePrev,
    kOnboardingPageNext,
    kBooksPageItem,
    kJournalPageSegmentControl,
    kJournalPageTimelineGroup,
};

struct NavigationItemDescriptor {
    NavigationItemSection section = NavigationItemSection::kNone;
    NavigationItemRole role = NavigationItemRole::kUnknown;
    int item_index = -1;
};

struct NavigationModel {
    NavigationScope scope = NavigationScope::kSettings;
    std::vector<NavigationItemDescriptor> items = {};
    int item_count = 0;

    const NavigationItemDescriptor* ItemAt(int index) const;
    int IndexOfRole(NavigationItemRole role) const;
    bool IsRoleSelected(int selected_index, NavigationItemRole role) const;
};

NavigationModel BuildSettingsPageNavigationModel();
NavigationModel BuildWifiPageNavigationModel();
NavigationModel BuildTimePageNavigationModel();
NavigationModel BuildDashboardPageNavigationModel();
NavigationModel BuildVibeCheckPageNavigationModel();
NavigationModel BuildSummarizePageNavigationModel();
NavigationModel BuildNotesPageNavigationModel(int timeline_group_count);
NavigationModel BuildFollowUpPageNavigationModel(int timeline_group_count);
// with_transcribe adds a focusable Transcribe button (shown only for audio-only recordings that
// have no transcript yet); when false the page has just the Back button.
NavigationModel BuildDetailsPageNavigationModel(bool with_transcribe = false);
NavigationModel BuildOnboardingPageNavigationModel();
// One item per book (item_index = library index) followed by the footer.
NavigationModel BuildBooksPageNavigationModel(int book_count);
// The level switcher (Ano / Mês / Semana / Dia), one item per timeline group, then the footer.
NavigationModel BuildJournalPageNavigationModel(int timeline_group_count);

}  // namespace page_navigation

#endif  // PAGE_NAVIGATION_NAVIGATION_MODEL_H_

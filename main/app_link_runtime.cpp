#include "app_link_runtime.h"

#include <mutex>
#include <string>

#include "display_service.h"
#include "epaper_ui/select_modal.h"
#include "epaper_ui/toast.h"
#include "esp_log.h"
#include "overlay_runtime.h"
#include "project_assets.h"
#include "settings_page_runtime.h"

namespace app_link_runtime {
namespace {

constexpr const char* kTag = "AppLinkRuntime";
constexpr uint32_t kToastMs = 3000;

enum class PendingModal : uint8_t {
    kNone = 0,
    kCode,
    kLinked,
};

std::mutex s_mutex;
PendingModal s_pending = PendingModal::kNone;
// Last snapshot seen, to react to transitions only (not to every busy flip).
journal_sync_service::LinkState s_last_state = journal_sync_service::LinkState::kUnlinked;
std::string s_last_code = {};
std::string s_last_error = {};

void ShowToast(const char* text, EmbeddedIconId icon)
{
    epaper_ui::ToastState toast = {};
    toast.visible = true;
    toast.body_text = text;
    toast.leading_icon = project_assets::GetIcon(icon);
    const esp_err_t err = overlay_runtime::ShowToastForDuration(toast, kToastMs);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Toast failed: %s", esp_err_to_name(err));
    }
}

// "K7P 2QX" reads better on the e-paper than six glued characters.
std::string SpacedCode(const std::string& code)
{
    return code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
}

void ShowModal(PendingModal kind, const std::string& title,
               std::initializer_list<const char*> items)
{
    epaper_ui::SelectModalState modal = {};
    modal.title_text = title;
    for (const char* item : items) {
        modal.items.push_back({item});
    }
    modal.selected_index = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pending = kind;
    }
    const esp_err_t err = overlay_runtime::ShowSelectModal(modal);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pending = PendingModal::kNone;
        ESP_LOGW(kTag, "Modal failed: %s", esp_err_to_name(err));
    }
}

void ShowCodeModal(const std::string& code)
{
    ShowModal(PendingModal::kCode, "No app, digite: " + SpacedCode(code),
              {"Fechar", "Cancelar pareamento"});
}

void RefreshSettingsPage()
{
    if (display_service::GetCurrentScreen() == display_service::ScreenId::kSettings) {
        (void)settings_page_runtime::UpdateDisplayStateAndRequestRefresh(
            display_service::RefreshMode::kPartial);
    }
}

}  // namespace

void OpenMenu()
{
    const journal_sync_service::Snapshot snapshot = journal_sync_service::GetSnapshot();
    switch (snapshot.state) {
        case journal_sync_service::LinkState::kLinked:
            ShowModal(PendingModal::kLinked, "App web conectado",
                      {"Sincronizar agora", "Desconectar", "Fechar"});
            return;
        case journal_sync_service::LinkState::kPairing:
            if (!snapshot.pair_code.empty()) {
                ShowCodeModal(snapshot.pair_code);
            } else {
                ShowToast("Gerando código...", EmbeddedIconId::kRefresh);
            }
            return;
        case journal_sync_service::LinkState::kUnlinked:
        default:
            if (!journal_sync_service::StartPairing()) {
                ShowToast("Conecte o Wi-Fi para parear", EmbeddedIconId::kWifiConfig);
                return;
            }
            ShowToast("Gerando código...", EmbeddedIconId::kRefresh);
            RefreshSettingsPage();
            return;
    }
}

bool HandleModalSelection(int selected_index)
{
    PendingModal pending = PendingModal::kNone;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        pending = s_pending;
        s_pending = PendingModal::kNone;
    }
    switch (pending) {
        case PendingModal::kCode:
            if (selected_index == 1) {
                journal_sync_service::CancelPairing();
            }
            return true;
        case PendingModal::kLinked:
            if (selected_index == 0) {
                journal_sync_service::RequestSync();
                ShowToast("Sincronizando...", EmbeddedIconId::kRefresh);
            } else if (selected_index == 1) {
                journal_sync_service::Unlink();
                ShowToast("App desconectado", EmbeddedIconId::kCheck);
            }
            return true;
        case PendingModal::kNone:
        default:
            return false;
    }
}

void HandleSyncEvent(const journal_sync_service::Event& event, void*)
{
    const journal_sync_service::Snapshot& snapshot = event.snapshot;
    bool state_changed = false;
    bool code_arrived = false;
    bool linked_now = false;
    bool new_error = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        state_changed = snapshot.state != s_last_state;
        code_arrived = snapshot.state == journal_sync_service::LinkState::kPairing &&
                       !snapshot.pair_code.empty() && snapshot.pair_code != s_last_code;
        linked_now = state_changed && snapshot.state == journal_sync_service::LinkState::kLinked &&
                     s_last_state == journal_sync_service::LinkState::kPairing;
        new_error = !snapshot.last_error.empty() && snapshot.last_error != s_last_error;
        s_last_state = snapshot.state;
        s_last_code = snapshot.pair_code;
        s_last_error = snapshot.last_error;
    }

    if (code_arrived) {
        ShowCodeModal(snapshot.pair_code);
    } else if (linked_now) {
        ShowToast("App conectado!", EmbeddedIconId::kCheck);
    } else if (new_error && snapshot.state != journal_sync_service::LinkState::kLinked) {
        // Background sync errors stay silent (they retry); pairing problems need the user.
        ShowToast(snapshot.last_error.c_str(), EmbeddedIconId::kClose);
    }
    if (state_changed || code_arrived) {
        RefreshSettingsPage();
    }
}

}  // namespace app_link_runtime

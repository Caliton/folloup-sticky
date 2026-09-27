#ifndef APP_LINK_RUNTIME_H_
#define APP_LINK_RUNTIME_H_

#include "journal_sync_service.h"

// UI glue for pairing the device with the web app (Settings -> "Conectar app"): the code and
// "connected" modals plus the toasts that follow journal_sync_service's progress.
namespace app_link_runtime {

// Settings button: start pairing, show the code, or offer sync / disconnect when linked.
void OpenMenu();
// Select-modal submit chain (app_shell). True when one of our modals was pending.
bool HandleModalSelection(int selected_index);
// journal_sync_service event handler.
void HandleSyncEvent(const journal_sync_service::Event& event, void* context);

}  // namespace app_link_runtime

#endif  // APP_LINK_RUNTIME_H_

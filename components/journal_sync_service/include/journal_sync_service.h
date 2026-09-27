#ifndef JOURNAL_SYNC_SERVICE_H_
#define JOURNAL_SYNC_SERVICE_H_

#include <cstdint>
#include <string>

#include "esp_err.h"

// Two-way sync of the bullet journal (journal_service) with the Followup web app's Firestore.
// The device signs in anonymously (Firebase Auth REST), pairs through a 6-character code the
// user types in the web app, then pulls documents newer than its cursor and pushes its dirty
// items (last writer wins on updatedAt). See docs/journal.md for the data contract.
//
// Network work runs on a short-lived worker task (TLS needs an 8 KB stack in internal RAM), so
// it only starts while Wi-Fi is up, no transcription is in flight and the heap has room.
namespace journal_sync_service {

enum class LinkState : uint8_t {
    kUnlinked = 0,
    kPairing,  // code shown, waiting for the web app to claim it
    kLinked,
};

struct Snapshot {
    LinkState state = LinkState::kUnlinked;
    std::string pair_code = {};  // "K7P2QX" while pairing
    bool busy = false;           // a network round is running
    bool network_connected = false;
    int64_t last_sync_unix = 0;
    std::string last_error = {};  // pt-BR, empty when the last round succeeded
};

struct Event {
    Snapshot snapshot = {};
};

using EventHandler = void (*)(const Event& event, void* context);

esp_err_t Init();
void SetEventHandler(EventHandler handler, void* context);
Snapshot GetSnapshot();

void SetNetworkConnected(bool connected);
// Get a code for the web app (async: the snapshot moves to kPairing with the code, then to
// kLinked once the web app claims it). Needs Wi-Fi.
bool StartPairing();
void CancelPairing();
// Forget the web account (the device keeps its anonymous identity for a later pairing).
void Unlink();
// Run a sync round soon (debounced). No-op unless linked.
void RequestSync();

}  // namespace journal_sync_service

#endif  // JOURNAL_SYNC_SERVICE_H_

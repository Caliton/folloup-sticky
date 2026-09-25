#ifndef FEEDBACK_SERVICE_H_
#define FEEDBACK_SERVICE_H_

#include "esp_err.h"

namespace feedback_service {

enum class FeedbackEvent {
    kStartup,
    kGeminiConnected,
    kLock,
    kUnlock,
    kRecordingStart,
    kModalOpen,
    kButtonClick,
    kButtonDoubleClick,
    kButtonLongPress,
    kTouchContact,
    kShutdown,
    kError,
};

esp_err_t Init();
esp_err_t Play(FeedbackEvent event);

// "Sons" setting (persisted in NVS, default on). Off keeps the audio output path, PA
// included, powered down: cues and clip playback are skipped. Recording is unaffected.
bool IsSoundEnabled();
void SetSoundEnabled(bool enabled);

}  // namespace feedback_service

#endif  // FEEDBACK_SERVICE_H_

#include "feedback_service.h"

#include "esp_log.h"
#include "nvs.h"
#include "waveshare_board.h"
#include "system_sound_service.h"

namespace feedback_service {
namespace {

constexpr const char* kTag = "FeedbackService";
constexpr const char* kSettingsNamespace = "app_state";
constexpr const char* kSoundEnabledKey = "sound_on";

bool LoadSoundEnabled()
{
    nvs_handle_t handle = 0;
    uint8_t value = 1;
    if (nvs_open(kSettingsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, kSoundEnabledKey, &value);
        nvs_close(handle);
    }
    return value != 0;
}

void StoreSoundEnabled(bool enabled)
{
    nvs_handle_t handle = 0;
    if (nvs_open(kSettingsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(kTag, "could not open NVS to save the sound setting");
        return;
    }
    if (nvs_set_u8(handle, kSoundEnabledKey, enabled ? 1 : 0) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

// Maps a feedback event onto the closest system-sound cue (played through the
// ES8311 codec). There is no dedicated buzzer on this board.
SoundCue CueForEvent(FeedbackEvent event)
{
    switch (event) {
        case FeedbackEvent::kStartup:
            return SoundCue::kStartup;
        case FeedbackEvent::kGeminiConnected:
            return SoundCue::kOnline;
        case FeedbackEvent::kLock:
            return SoundCue::kLock;
        case FeedbackEvent::kUnlock:
            return SoundCue::kUnlock;
        case FeedbackEvent::kRecordingStart:
            return SoundCue::kButtonActivate;
        case FeedbackEvent::kModalOpen:
            return SoundCue::kModalNotification;
        case FeedbackEvent::kButtonClick:
        case FeedbackEvent::kButtonDoubleClick:
        case FeedbackEvent::kButtonLongPress:
            return SoundCue::kButtonActivate;
        case FeedbackEvent::kTouchContact:
            return SoundCue::kNavigationMove;
        case FeedbackEvent::kShutdown:
            return SoundCue::kModalNotification;
        case FeedbackEvent::kError:
        default:
            return SoundCue::kInterrupt;
    }
}

}  // namespace

esp_err_t Init()
{
    AudioCodec* codec = waveshare_board::GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGW(kTag, "Audio codec unavailable; feedback cues disabled");
        return ESP_ERR_NOT_FOUND;
    }

    codec->EnableOutput(LoadSoundEnabled());
    // Starts the sound-service playback task and warms (decodes) the cue cache.
    SystemSoundService::GetInstance().Initialize(codec);
    ESP_LOGI(kTag, "Feedback service initialized (audio cues via ES8311)");
    return ESP_OK;
}

esp_err_t Play(FeedbackEvent event)
{
    SystemSoundService::GetInstance().PlayCue(CueForEvent(event));
    return ESP_OK;
}

bool IsSoundEnabled()
{
    AudioCodec* codec = waveshare_board::GetAudioCodec();
    return codec != nullptr ? codec->output_enabled() : LoadSoundEnabled();
}

void SetSoundEnabled(bool enabled)
{
    AudioCodec* codec = waveshare_board::GetAudioCodec();
    if (codec != nullptr) {
        codec->EnableOutput(enabled);
    }
    StoreSoundEnabled(enabled);
    ESP_LOGI(kTag, "Sounds %s", enabled ? "on" : "off");
}

}  // namespace feedback_service

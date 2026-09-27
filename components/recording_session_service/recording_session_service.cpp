#include "recording_session_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "esp_log.h"
#include "sdkconfig.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gemini_service.h"
#include "journal_period.h"
#include "journal_service.h"
#include "playback_service.h"
#include "storage_service.h"
#include "system_sound_service.h"

namespace recording_session_service {
namespace {

constexpr const char* kTag = "RecordingSession";
constexpr const char* kIdleStatus = "Segure BOOT para gravar";
constexpr const char* kArmedStatus = "Continue segurando para gravar";
constexpr const char* kRecordingStatus = "Gravando";
constexpr const char* kStartCueStatus = "Iniciando gravação";
constexpr const char* kStopCueStatus = "Finalizando gravação";
constexpr const char* kPlayingBackStatus = "Reproduzindo";
constexpr const char* kChooseTagStatus = "Escolha o tipo de gravação";
constexpr const char* kSavingStatus = "Salvando gravação";
constexpr const char* kTranscribingStatus = "Transcrevendo gravação";
constexpr const char* kSavedWithoutTranscriptStatus =
    "Gravação salva sem transcrição";
constexpr const char* kDiscardedStatus = "Gravação descartada";
constexpr uint32_t kMinTranscriptionDurationMs = 500;
constexpr uint32_t kFallbackAudioSampleRateHz = 24000;
constexpr size_t kSignalWindowSamples = 240;
constexpr int32_t kSpeechPeakThreshold = 700;
constexpr size_t kMinSpeechWindows = 3;

constexpr std::array<TagOption, 4> kTagOptions = {{
    {.tag = recording_archive_service::RecordingTag::kNote, .label_text = "Nota"},
    {.tag = recording_archive_service::RecordingTag::kTask, .label_text = "Tarefa"},
    {.tag = recording_archive_service::RecordingTag::kIdea, .label_text = "Ideia"},
    {.label_text = "Descartar", .is_discard = true},
}};

struct GuardrailResult {
    bool accepted = false;
    const char* error_code = "";
    const char* status_message = "";
};

std::mutex s_mutex;
EventHandler s_event_handler = nullptr;
void* s_event_context = nullptr;
bool s_initialized = false;
Snapshot s_snapshot = {};
std::string s_pending_recording_id = {};
// Journal period of the screen the current take was started on (see Context::journal_period).
std::string s_journal_context = {};
// Journal item created for a take tagged "Tarefa" by hand (no Gemini classification); the
// transcript, if one arrives, becomes its text.
std::string s_manual_journal_item = {};
constexpr const char* kAudioOnlyTaskText = "Tarefa em áudio";

// Cue tokens make a late callback harmless: every cue we queue bumps the token, so a
// result that arrives after the session has moved on (cancel, failure, a new recording)
// no longer matches and is dropped instead of driving a stale transition.
uint32_t s_cue_token = 0;
std::atomic<bool> s_playback_worker_active{false};
// True while AutoSaveWorker still owns the in-memory take; a new press must not re-arm the
// recorder underneath it.
std::atomic<bool> s_auto_save_active{false};
// Set when BOOT is released before the start cue finishes; consumed by HandleStartCueResult.
bool s_finish_pending_after_start_cue = false;

uint32_t ResolveClipDurationMs(const recording_service::RecordedClip& clip, uint32_t duration_ms)
{
    if (duration_ms > 0) {
        return duration_ms;
    }
    return clip.sample_rate_hz() > 0
               ? clip.duration_ms()
               : static_cast<uint32_t>((clip.sample_count() * 1000ULL) /
                                       kFallbackAudioSampleRateHz);
}

GuardrailResult ValidateClip(const recording_service::RecordedClip& clip, uint32_t duration_ms)
{
    if (clip.empty()) {
        return {
            .accepted = false,
            .error_code = "empty_audio",
            .status_message = "Gravação muito curta",
        };
    }

    const uint32_t resolved_duration_ms = ResolveClipDurationMs(clip, duration_ms);
    if (resolved_duration_ms < kMinTranscriptionDurationMs) {
        return {
            .accepted = false,
            .error_code = "recording_too_short",
            .status_message = "Gravação muito curta",
        };
    }

    size_t speech_windows = 0;
    size_t window_fill = 0;
    int32_t window_peak = 0;
    auto finish_window = [&]() -> bool {
        if (window_fill == 0) {
            return false;
        }
        if (window_peak >= kSpeechPeakThreshold) {
            ++speech_windows;
            if (speech_windows >= kMinSpeechWindows) {
                return true;
            }
        }
        window_fill = 0;
        window_peak = 0;
        return false;
    };

    bool accepted = false;
    clip.ForEachChunk([&](const int16_t* chunk_data, size_t chunk_size) {
        if (accepted || chunk_data == nullptr) {
            return;
        }
        for (size_t index = 0; index < chunk_size; ++index) {
            int32_t amplitude = chunk_data[index];
            if (amplitude < 0) {
                amplitude = -amplitude;
            }
            window_peak = std::max(window_peak, amplitude);
            ++window_fill;
            if (window_fill >= kSignalWindowSamples && finish_window()) {
                accepted = true;
                break;
            }
        }
    });
    if (!accepted && finish_window()) {
        accepted = true;
    }

    if (accepted) {
        return {
            .accepted = true,
            .error_code = "",
            .status_message = "",
        };
    }

    return {
        .accepted = false,
        .error_code = "recording_too_quiet",
        .status_message = "Nenhuma fala detectada",
    };
}

BlockedReason EvaluateBlockedReason(const Context& context)
{
    if (context.lock_screen_active) {
        return BlockedReason::kLockScreenActive;
    }
    if (context.overlay_visible) {
        return BlockedReason::kOverlayVisible;
    }
    if (storage_service::IsWriteBusy()) {
        return BlockedReason::kStorageBusy;
    }
    if (!recording_service::IsInitialized()) {
        return BlockedReason::kRecorderUnavailable;
    }
    if (transcription_service::GetSnapshot().request_in_flight) {
        return BlockedReason::kTranscriptionInFlight;
    }
    return BlockedReason::kNone;
}

const char* BlockedReasonStatusMessage(BlockedReason reason)
{
    switch (reason) {
        case BlockedReason::kLockScreenActive:
            return "Desbloqueie para gravar";
        case BlockedReason::kOverlayVisible:
            return "Feche a janela atual";
        case BlockedReason::kStorageBusy:
            return "Aguarde o cartão SD terminar";
        case BlockedReason::kRecorderUnavailable:
            return "Gravador indisponível";
        case BlockedReason::kTranscriptionInFlight:
            return "Aguarde a transcrição terminar";
        case BlockedReason::kNone:
        default:
            return "";
    }
}

void ResetToIdleLocked()
{
    s_snapshot.allowed = true;
    s_snapshot.blocked_reason = BlockedReason::kNone;
    s_snapshot.phase = Phase::kIdle;
    s_snapshot.has_clip = false;
    s_snapshot.clip_saved = false;
    s_snapshot.transcript_saved = false;
    s_snapshot.request_in_flight = false;
    s_snapshot.recorded_samples = 0;
    s_snapshot.duration_ms = 0;
    s_snapshot.input_level_percent = 0;
    s_snapshot.stop_reason = recording_service::StopReason::kNone;
    s_snapshot.last_saved_recording_id.clear();
    s_snapshot.last_saved_recording_path.clear();
    s_snapshot.last_saved_transcript_path.clear();
    s_snapshot.last_transcript.clear();
    s_snapshot.auto_tagged = false;
    s_snapshot.auto_tag = recording_archive_service::RecordingTag::kNote;
    s_snapshot.journal_filed = false;
    s_snapshot.journal_type.clear();
    s_snapshot.journal_period.clear();
    s_snapshot.last_error_code.clear();
    s_snapshot.last_error_message.clear();
    s_snapshot.last_status_message = kIdleStatus;
    s_pending_recording_id.clear();
    // Invalidate any cue callback still in flight, so a late result cannot resurrect a
    // session that was just cancelled.
    ++s_cue_token;
    s_finish_pending_after_start_cue = false;
}

void SyncRecordingStateLocked(const recording_service::UiState& state)
{
    s_snapshot.initialized = state.initialized;
    s_snapshot.recorded_samples = state.recorded_samples;
    s_snapshot.duration_ms = state.duration_ms;
    s_snapshot.max_recording_ms = state.max_recording_ms;
    s_snapshot.input_level_percent = state.input_level_percent;
    s_snapshot.stop_reason = state.stop_reason;
    s_snapshot.has_clip = state.has_clip;
}

void NotifyLocked()
{
    EventHandler handler = s_event_handler;
    void* context = s_event_context;
    if (handler == nullptr) {
        return;
    }

    const Event event = {
        .snapshot = s_snapshot,
    };
    handler(event, context);
}

// Playback blocks for the length of the clip, so it cannot run on the sound-cue callback
// task. This mirrors the details page's short-lived worker: the shared_ptr keeps the PSRAM
// clip alive for the duration even though nothing else references it yet.
constexpr uint32_t kPlaybackWorkerStackWords = 4096;

void AdvanceToTagSelection(const char* reason);
bool SaveAndTranscribe(recording_archive_service::RecordingTag tag, bool classify);

void PlaybackWorker(void* arg)
{
    std::unique_ptr<recording_service::RecordedClipPtr> clip(
        static_cast<recording_service::RecordedClipPtr*>(arg));
    if (clip != nullptr && *clip != nullptr) {
        const esp_err_t err = playback_service::PlayClip(*clip);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Clip playback failed: %s", esp_err_to_name(err));
        }
    }
    // Drop our clip reference before self-deleting: vTaskDelete(nullptr) never returns, so
    // the unique_ptr destructor would never run and the whole take would leak in PSRAM.
    clip.reset();
    s_playback_worker_active.store(false, std::memory_order_release);
    AdvanceToTagSelection("playback finished");
    vTaskDelete(nullptr);
}

// With Gemini available the tag menu is skipped: the take is saved as a Note and the
// transcription request itself decides the tag ("adiciona uma tarefa ..." -> Task), which
// HandleTranscriptionEvent applies. Saving does SD I/O, and the callers here run on the
// sound-cue callback or the self-deleting playback worker, so it gets its own short task
// (same budget as the input task that runs a manual SubmitTagSelection).
constexpr uint32_t kAutoSaveWorkerStackWords = 4096;

void AutoSaveWorker(void*)
{
    (void)SaveAndTranscribe(recording_archive_service::RecordingTag::kNote, true);
    s_auto_save_active.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

bool StartAutoSave()
{
    if (!gemini_service::GetSnapshot().runtime.ready) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_snapshot.phase != Phase::kStopCue && s_snapshot.phase != Phase::kPlayingBack) {
            return true;  // session moved on (cancelled/failed); nothing left to route
        }
        s_snapshot.phase = Phase::kSaving;
        s_snapshot.last_status_message = kSavingStatus;
        s_snapshot.last_error_code.clear();
        s_snapshot.last_error_message.clear();
        NotifyLocked();
    }
    s_auto_save_active.store(true, std::memory_order_release);
    if (xTaskCreate(&AutoSaveWorker, "rec_autosave", kAutoSaveWorkerStackWords, nullptr,
                    followup_task_config::kPriorityStorage, nullptr) == pdPASS) {
        return true;
    }
    s_auto_save_active.store(false, std::memory_order_release);
    ESP_LOGW(kTag, "Failed to start auto-save worker; falling back to the tag menu");
    std::lock_guard<std::mutex> lock(s_mutex);
    s_snapshot.phase = Phase::kStopCue;  // re-enter AdvanceToTagSelection's accepted phases
    return false;
}

// Every route out of the stop cue lands here: playback done, playback refused to start, or
// the stop cue itself failed. The clip is still unsaved, so the tag menu's Discard option
// is what throws away a bad take (only shown when Gemini can't auto-tag it).
void AdvanceToTagSelection(const char* reason)
{
    if (StartAutoSave()) {
        ESP_LOGI(kTag, "Auto-tagging take via Gemini (%s)", reason);
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_snapshot.phase != Phase::kStopCue && s_snapshot.phase != Phase::kPlayingBack) {
        return;
    }
    ESP_LOGI(kTag, "Advancing to tag selection (%s)", reason);
    s_snapshot.phase = Phase::kAwaitingTagSelection;
    s_snapshot.last_status_message = kChooseTagStatus;
    s_snapshot.last_error_code.clear();
    s_snapshot.last_error_message.clear();
    NotifyLocked();
}

// Replays the take the user just recorded. Returns false when playback could not be
// started, so the caller can fall through to tag selection rather than stranding the
// session in kStopCue.
bool StartClipPlayback(const recording_service::RecordedClipPtr& clip)
{
    if (clip == nullptr || clip->empty()) {
        return false;
    }
    bool expected = false;
    if (!s_playback_worker_active.compare_exchange_strong(expected, true)) {
        return false;
    }

    // Publish kPlayingBack before the worker can exist. The worker advances to tag
    // selection the moment it finishes, and a very short clip would otherwise beat this
    // update -- leaving the phase stuck on kPlayingBack after the menu had already opened.
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.phase = Phase::kPlayingBack;
        s_snapshot.last_status_message = kPlayingBackStatus;
        NotifyLocked();
    }

    auto* clip_copy = new recording_service::RecordedClipPtr(clip);
    if (xTaskCreate(&PlaybackWorker, "clip_playback", kPlaybackWorkerStackWords, clip_copy,
                    followup_task_config::kPriorityStorage, nullptr) != pdPASS) {
        delete clip_copy;
        s_playback_worker_active.store(false, std::memory_order_release);
        ESP_LOGW(kTag, "Failed to start clip playback worker");
        // Phase is kPlayingBack here, which AdvanceToTagSelection accepts, so the caller's
        // fallback still lands correctly.
        return false;
    }
    return true;
}

// Fires once the stop cue has finished (or failed). Playing the clip under the cue would
// overlap two streams on the same codec output, so the replay waits for it.
void HandleStopCueResult(uint32_t token, SoundCuePlaybackResult result)
{
    recording_service::RecordedClipPtr clip = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (token != s_cue_token || s_snapshot.phase != Phase::kStopCue) {
            return;
        }
    }
    if (result != SoundCuePlaybackResult::kCompleted) {
        AdvanceToTagSelection("stop cue did not complete");
        return;
    }

    clip = recording_service::GetRecordedClip();
    if (clip != nullptr &&
        clip->duration_ms() > CONFIG_FOLLOWUP_AUTO_REPLAY_MAX_SECONDS * 1000U) {
        AdvanceToTagSelection("take too long for replay");
        return;
    }
    if (!StartClipPlayback(clip)) {
        AdvanceToTagSelection("playback unavailable");
    }
}

void HandleStartCueResult(uint32_t token, SoundCuePlaybackResult)
{
    bool finish_now = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        // The cue is cosmetic: capture is already running underneath it, so even a failed
        // cue just moves the UI on rather than aborting the take.
        if (token != s_cue_token || s_snapshot.phase != Phase::kStartCue) {
            return;
        }
        finish_now = s_finish_pending_after_start_cue;
        s_finish_pending_after_start_cue = false;
        if (finish_now) {
            s_snapshot.phase = Phase::kSaving;
            s_snapshot.last_status_message = "Preparando gravação";
        } else {
            s_snapshot.phase = Phase::kRecording;
            s_snapshot.last_status_message = kRecordingStatus;
        }
        NotifyLocked();
    }

    if (finish_now) {
        (void)recording_service::Finish();
    }
}

void MarkBlockedLocked(BlockedReason reason)
{
    s_snapshot.allowed = false;
    s_snapshot.blocked_reason = reason;
    s_snapshot.last_status_message = BlockedReasonStatusMessage(reason);
    s_snapshot.last_error_code.clear();
    s_snapshot.last_error_message.clear();
}

// Archives the in-memory take under `tag`, then starts its transcription. With `classify`
// the transcription also picks the final tag, applied in HandleTranscriptionEvent.
// Period for a take filed without a spoken date: the journal screen's, else today.
std::string DefaultJournalPeriod()
{
    std::string period;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        period = s_journal_context;
    }
    journal_period::Date today = {};
    if (!journal_period::IsValidKey(period) && journal_period::Today(&today)) {
        period = journal_period::DayKey(today);
    }
    return period;
}

bool SaveAndTranscribe(recording_archive_service::RecordingTag tag, bool classify)
{
    // Tasks live in the journal now: a take tagged "Tarefa" by hand is archived as a plain
    // recording and linked to a new journal task (filed like a classified one would be).
    const bool manual_task = !classify && tag == recording_archive_service::RecordingTag::kTask;
    if (manual_task) {
        tag = recording_archive_service::RecordingTag::kNote;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_manual_journal_item.clear();
    }
    recording_service::RecordedClipPtr clip = recording_service::GetRecordedClip();
    if (!clip || clip->empty()) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.phase = Phase::kFailed;
        s_snapshot.last_status_message = "Falha ao salvar";
        s_snapshot.last_error_code = "recording_missing";
        s_snapshot.last_error_message = "O clipe da gravação não estava disponível";
        NotifyLocked();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.phase = Phase::kSaving;
        s_snapshot.last_status_message = kSavingStatus;
        s_snapshot.last_error_code.clear();
        s_snapshot.last_error_message.clear();
        NotifyLocked();
    }

    recording_archive_service::SaveOptions options = {};
    options.tag = tag;
    ESP_LOGI(kTag,
             "Starting archive save: tag=%s samples=%u duration_ms=%lu",
             recording_archive_service::TagName(options.tag),
             static_cast<unsigned>(clip->sample_count()),
             static_cast<unsigned long>(clip->duration_ms()));
    const recording_archive_service::SaveResult save_result =
        recording_archive_service::SaveClip(*clip, options);
    ESP_LOGI(kTag,
             "Archive save result: success=%d clip_saved=%d metadata_saved=%d id=%s wav=%s metadata=%s error=%s",
             save_result.success ? 1 : 0,
             save_result.clip_saved ? 1 : 0,
             save_result.metadata_saved ? 1 : 0,
             save_result.recording_id.empty() ? "<none>" : save_result.recording_id.c_str(),
             save_result.recording_path.empty() ? "<none>" : save_result.recording_path.c_str(),
             save_result.metadata_path.empty() ? "<none>" : save_result.metadata_path.c_str(),
             save_result.error_code.empty() ? "<none>" : save_result.error_code.c_str());

    const bool should_transcribe = save_result.clip_saved && gemini_service::GetSnapshot().runtime.ready;
    ESP_LOGI(kTag,
             "Transcription decision: clip_saved=%d gemini_ready=%d should_transcribe=%d",
             save_result.clip_saved ? 1 : 0,
             gemini_service::GetSnapshot().runtime.ready ? 1 : 0,
             should_transcribe ? 1 : 0);

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.clip_saved = save_result.clip_saved;
        s_snapshot.transcript_saved = false;
        s_snapshot.last_saved_recording_id = save_result.recording_id;
        s_snapshot.last_saved_recording_path = save_result.recording_path;
        s_snapshot.last_saved_transcript_path = save_result.transcript_path;
        s_pending_recording_id = save_result.recording_id;
    }

    if (manual_task && save_result.clip_saved) {
        const std::string period = DefaultJournalPeriod();
        const std::string item_id = journal_service::CreateItem(
            journal_service::ItemType::kTask, kAudioOnlyTaskText, period, save_result.recording_id);
        if (!item_id.empty()) {
            (void)recording_archive_service::SetRecordingJournalItem(save_result.recording_id,
                                                                     item_id);
            std::lock_guard<std::mutex> lock(s_mutex);
            s_manual_journal_item = item_id;
            s_snapshot.journal_filed = true;
            s_snapshot.journal_type = journal_service::TypeName(journal_service::ItemType::kTask);
            s_snapshot.journal_period = period;
        }
    }

    if (should_transcribe && transcription_service::BeginTranscription(clip, classify)) {
        recording_service::DiscardClip();
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.phase = Phase::kTranscribing;
        s_snapshot.request_in_flight = true;
        s_snapshot.last_status_message = kTranscribingStatus;
        s_snapshot.last_error_code.clear();
        s_snapshot.last_error_message.clear();
        NotifyLocked();
        return true;
    }

    if (should_transcribe) {
        ESP_LOGW(kTag,
                 "Transcription did not start after save: id=%s",
                 save_result.recording_id.empty() ? "<none>" : save_result.recording_id.c_str());
    }

    recording_service::DiscardClip();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.request_in_flight = false;
        s_snapshot.phase = save_result.clip_saved ? Phase::kComplete : Phase::kFailed;
        s_snapshot.last_status_message = save_result.clip_saved
                                             ? kSavedWithoutTranscriptStatus
                                             : (save_result.status_message.empty()
                                                    ? "Falha ao salvar"
                                                    : save_result.status_message);
        s_snapshot.last_error_code = save_result.error_code;
        s_snapshot.last_error_message = save_result.error_message;
        if (save_result.clip_saved && !save_result.error_code.empty()) {
            // Keep the saved result but surface the archive warning.
            s_snapshot.last_error_message = save_result.error_message;
        } else if (save_result.clip_saved) {
            s_snapshot.last_error_code.clear();
            s_snapshot.last_error_message.clear();
        }
        NotifyLocked();
    }
    return save_result.clip_saved;
}


// Where a classified take goes in the bullet journal. Ideas always stay in the collection and
// tasks always go to the journal (there is no separate task list any more). Notes and events
// are filed when recorded on the journal screen or when the speech said when; events always.
struct JournalFiling {
    bool file = false;
    journal_service::ItemType type = journal_service::ItemType::kTask;
    std::string period = {};
};

std::string ResolveSpokenWhen(const std::string& when, const journal_period::Date& today)
{
    using journal_period::AddDays;
    if (when.empty()) {
        return {};
    }
    if (when == "today") {
        return journal_period::DayKey(today);
    }
    if (when == "tomorrow") {
        return journal_period::DayKey(AddDays(today, 1));
    }
    if (when == "this_week") {
        return journal_period::WeekKey(today);
    }
    if (when == "next_week") {
        return journal_period::WeekKey(AddDays(today, 7));
    }
    if (when == "this_month") {
        return journal_period::MonthKey(today);
    }
    if (when == "next_month") {
        return journal_period::NextOf(journal_period::MonthKey(today));
    }
    // A date the model worked out itself; drop it if it is already in the past (a misheard
    // weekday would otherwise file the item straight into the review queue).
    if (journal_period::IsValidKey(when) && !journal_period::HasEnded(when, today)) {
        return when;
    }
    return {};
}

JournalFiling DecideJournalFiling(const std::string& tag, const std::string& when,
                                  const std::string& screen_period)
{
    JournalFiling filing = {};
    journal_period::Date today = {};
    if (tag.empty() || tag == "idea" || !journal_period::Today(&today)) {
        return filing;
    }
    const std::string spoken = ResolveSpokenWhen(when, today);
    if (screen_period.empty() && tag == "note" && spoken.empty()) {
        return filing;
    }
    filing.file = true;
    filing.type = tag == "event"  ? journal_service::ItemType::kEvent
                  : tag == "note" ? journal_service::ItemType::kNote
                                  : journal_service::ItemType::kTask;
    if (!spoken.empty()) {
        filing.period = spoken;
    } else if (journal_period::IsValidKey(screen_period)) {
        filing.period = screen_period;
    } else {
        filing.period = journal_period::DayKey(today);
    }
    return filing;
}

}  // namespace

esp_err_t Init()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_initialized) {
        return ESP_OK;
    }

    s_initialized = true;
    s_snapshot.initialized = recording_service::IsInitialized();
    s_snapshot.max_recording_ms = recording_service::GetUiState().max_recording_ms;
    ResetToIdleLocked();
    return ESP_OK;
}

void SetEventHandler(EventHandler handler, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_event_handler = handler;
    s_event_context = context;
}

Snapshot GetSnapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_snapshot;
}

const std::array<TagOption, 4>& TagOptions()
{
    return kTagOptions;
}

bool BeginArchivedTranscription(const std::string& recording_id)
{
    if (Init() != ESP_OK || recording_id.empty()) {
        return false;
    }

    // Reuse the recording->transcription pipeline for an already-archived clip: load its WAV
    // back into memory, hand it to the same transcription service, and let the existing
    // HandleTranscriptionEvent path save the transcript and drive the toasts.
    if (transcription_service::GetSnapshot().request_in_flight) {
        return false;  // a recording or another re-transcribe is already running
    }

    recording_service::RecordedClipPtr clip = recording_archive_service::LoadClip(recording_id);
    if (!clip || clip->empty()) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.phase = Phase::kFailed;
        s_snapshot.request_in_flight = false;
        s_snapshot.last_status_message = "Não foi possível carregar o áudio";
        s_snapshot.last_error_code = "clip_load_failed";
        s_snapshot.last_error_message = "Falha ao ler o WAV do cartão SD";
        NotifyLocked();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.clip_saved = true;  // the recording already lives on SD
        s_snapshot.transcript_saved = false;
        s_pending_recording_id = recording_id;
        s_journal_context.clear();
        s_manual_journal_item.clear();
    }

    if (transcription_service::BeginTranscription(clip)) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.phase = Phase::kTranscribing;
        s_snapshot.request_in_flight = true;
        s_snapshot.last_status_message = kTranscribingStatus;
        s_snapshot.last_error_code.clear();
        s_snapshot.last_error_message.clear();
        NotifyLocked();
        return true;
    }

    // Couldn't start (e.g. Gemini not ready): surface the transcription error as a failure.
    const transcription_service::Snapshot ts = transcription_service::GetSnapshot();
    std::lock_guard<std::mutex> lock(s_mutex);
    s_snapshot.phase = Phase::kFailed;
    s_snapshot.request_in_flight = false;
    s_snapshot.last_status_message =
        ts.last_status_message.empty() ? "Transcrição indisponível" : ts.last_status_message;
    s_snapshot.last_error_code = ts.last_error_code;
    s_snapshot.last_error_message = ts.last_error_message;
    s_pending_recording_id.clear();
    NotifyLocked();
    return false;
}

bool HandlePowerPressDown(const Context& context)
{
    if (Init() != ESP_OK) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        // No new take while the previous one is still being cued, replayed, or resolved.
        if (s_auto_save_active.load(std::memory_order_acquire) ||
            s_snapshot.phase == Phase::kStartCue || s_snapshot.phase == Phase::kStopCue ||
            s_snapshot.phase == Phase::kPlayingBack ||
            s_snapshot.phase == Phase::kAwaitingTagSelection ||
            s_snapshot.phase == Phase::kTranscribing) {
            return false;
        }

        const BlockedReason blocked_reason = EvaluateBlockedReason(context);
        if (blocked_reason != BlockedReason::kNone) {
            MarkBlockedLocked(blocked_reason);
            NotifyLocked();
            return false;
        }
    }

    const esp_err_t err = recording_service::Arm();
    std::lock_guard<std::mutex> lock(s_mutex);
    if (err != ESP_OK) {
        s_snapshot.phase = Phase::kFailed;
        s_snapshot.allowed = false;
        s_snapshot.blocked_reason = BlockedReason::kRecorderUnavailable;
        s_snapshot.last_status_message = "Gravador indisponível";
        s_snapshot.last_error_code = "record_arm_failed";
        s_snapshot.last_error_message = esp_err_to_name(err);
        NotifyLocked();
        return false;
    }

    ResetToIdleLocked();
    s_journal_context = context.journal_period;
    s_snapshot.phase = Phase::kArmed;
    s_snapshot.allowed = true;
    s_snapshot.last_status_message = kArmedStatus;
    NotifyLocked();
    return true;
}

bool HandlePowerLongPressStart(const Context& context)
{
    if (Init() != ESP_OK) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const BlockedReason blocked_reason = EvaluateBlockedReason(context);
        if (blocked_reason != BlockedReason::kNone) {
            MarkBlockedLocked(blocked_reason);
            NotifyLocked();
            return false;
        }
        if (s_snapshot.phase != Phase::kArmed) {
            return false;
        }
    }

    // Capture starts before the cue, not after it: waiting for the cue to finish would
    // swallow the first word. The cue overlaps the opening moments of the take.
    const esp_err_t err = recording_service::Start(recording_service::StartMode::kFresh);
    uint32_t cue_token = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (err != ESP_OK) {
            s_snapshot.phase = Phase::kFailed;
            s_snapshot.last_status_message = "Falha ao iniciar a gravação";
            s_snapshot.last_error_code = "record_start_failed";
            s_snapshot.last_error_message = esp_err_to_name(err);
            NotifyLocked();
            return false;
        }

        cue_token = ++s_cue_token;
        s_snapshot.phase = Phase::kStartCue;
        s_snapshot.last_status_message = kStartCueStatus;
        s_snapshot.last_error_code.clear();
        s_snapshot.last_error_message.clear();
        NotifyLocked();
    }

    SystemSoundService::GetInstance().PlayCue(
        SoundCue::kSpeaking, [cue_token](SoundCuePlaybackResult result) {
            HandleStartCueResult(cue_token, result);
        });
    return true;
}

bool HandlePowerPressUp(const Context&)
{
    if (Init() != ESP_OK) {
        return false;
    }

    Phase phase = Phase::kIdle;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        phase = s_snapshot.phase;
    }

    if (phase == Phase::kArmed) {
        (void)recording_service::Cancel();
        std::lock_guard<std::mutex> lock(s_mutex);
        ResetToIdleLocked();
        NotifyLocked();
        return true;
    }

    // Released while the start cue is still playing: capture is already running, but the
    // cue owns the transition out of kStartCue. Defer the finish rather than dropping it,
    // otherwise a hold barely longer than the cue would record forever.
    if (phase == Phase::kStartCue) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_finish_pending_after_start_cue = true;
        return true;
    }

    if (phase != Phase::kRecording) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.phase = Phase::kSaving;
        s_snapshot.last_status_message = "Preparando gravação";
        NotifyLocked();
    }
    (void)recording_service::Finish();
    return true;
}

bool SubmitTagSelection(int selected_index)
{
    if (Init() != ESP_OK) {
        return false;
    }

    if (selected_index < 0 || selected_index >= static_cast<int>(kTagOptions.size())) {
        return false;
    }

    ESP_LOGI(kTag,
             "Recording tag selection submitted: index=%d label=%.*s",
             selected_index,
             static_cast<int>(kTagOptions[static_cast<size_t>(selected_index)].label_text.size()),
             kTagOptions[static_cast<size_t>(selected_index)].label_text.data());

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_snapshot.phase != Phase::kAwaitingTagSelection) {
            return false;
        }
    }

    if (kTagOptions[static_cast<size_t>(selected_index)].is_discard) {
        recording_service::DiscardClip();
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.phase = Phase::kComplete;
        s_snapshot.has_clip = false;
        s_snapshot.clip_saved = false;
        s_snapshot.transcript_saved = false;
        s_snapshot.last_status_message = kDiscardedStatus;
        s_snapshot.last_error_code.clear();
        s_snapshot.last_error_message.clear();
        NotifyLocked();
        return true;
    }

    return SaveAndTranscribe(kTagOptions[static_cast<size_t>(selected_index)].tag, false);
}

void HandleRecordingEvent(const recording_service::Event& event)
{
    if (Init() != ESP_OK) {
        return;
    }

    recording_service::RecordedClipPtr clip = nullptr;
    bool should_show_tag_selection = false;
    GuardrailResult guardrail = {};
    bool discard_invalid_clip = false;
    bool queue_stop_cue = false;
    uint32_t cue_token = 0;

    if (event.state == recording_service::State::kClipReady && event.ui_state.has_clip) {
        clip = recording_service::GetRecordedClip();
        if (clip) {
            guardrail = ValidateClip(*clip, event.ui_state.duration_ms);
            should_show_tag_selection = guardrail.accepted;
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        SyncRecordingStateLocked(event.ui_state);
        s_snapshot.request_in_flight = transcription_service::GetSnapshot().request_in_flight;

        if (event.state == recording_service::State::kArmed && s_snapshot.phase == Phase::kIdle) {
            s_snapshot.phase = Phase::kArmed;
            s_snapshot.last_status_message = kArmedStatus;
        } else if (event.state == recording_service::State::kRecording) {
            // Leave kStartCue alone -- HandleStartCueResult owns that transition, and
            // overwriting it here would drop the cue phase the moment capture engages.
            if (s_snapshot.phase != Phase::kStartCue) {
                s_snapshot.phase = Phase::kRecording;
                s_snapshot.last_status_message = kRecordingStatus;
            }
        } else if (event.state == recording_service::State::kClipReady) {
            if (should_show_tag_selection) {
                // Stop cue first, then playback, then the tag menu. HandleStopCueResult
                // drives the rest; queue_stop_cue defers the PlayCue call until the lock
                // is released so the callback cannot deadlock on a fast cue.
                cue_token = ++s_cue_token;
                queue_stop_cue = true;
                s_snapshot.phase = Phase::kStopCue;
                s_snapshot.last_status_message = kStopCueStatus;
                s_snapshot.last_error_code.clear();
                s_snapshot.last_error_message.clear();
            } else {
                discard_invalid_clip = true;
                s_snapshot.phase = Phase::kFailed;
                s_snapshot.has_clip = false;
                s_snapshot.last_status_message = guardrail.status_message;
                s_snapshot.last_error_code = guardrail.error_code;
                s_snapshot.last_error_message = guardrail.status_message;
            }
        }

        NotifyLocked();
    }
    if (discard_invalid_clip) {
        recording_service::DiscardClip();
    }
    if (queue_stop_cue) {
        SystemSoundService::GetInstance().PlayCue(
            SoundCue::kInterrupt, [cue_token](SoundCuePlaybackResult result) {
                HandleStopCueResult(cue_token, result);
            });
    }
}

void HandleTranscriptionEvent(const transcription_service::Event& event)
{
    if (Init() != ESP_OK) {
        return;
    }

    bool should_attach_transcript = false;
    std::string pending_recording_id;
    std::string transcript_text;
    std::string classified_tag;
    std::string classified_when;
    std::string journal_context;
    std::string manual_journal_item;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.request_in_flight = event.snapshot.request_in_flight;
        if (s_snapshot.phase == Phase::kTranscribing && !event.snapshot.request_in_flight &&
            !event.snapshot.last_transcript.empty() && !s_pending_recording_id.empty()) {
            should_attach_transcript = true;
            pending_recording_id = s_pending_recording_id;
            transcript_text = event.snapshot.last_transcript;
            classified_tag = event.snapshot.last_tag;
            classified_when = event.snapshot.last_when;
            journal_context = s_journal_context;
            manual_journal_item = s_manual_journal_item;
        }
    }

    if (should_attach_transcript) {
        // Auto-tagged takes were archived as Notes; move them before the transcript lands so
        // the archive event that SaveTranscript fires already shows them on the right page.
        bool retagged = false;
        auto tag = recording_archive_service::RecordingTag::kNote;
        const JournalFiling filing =
            DecideJournalFiling(classified_tag, classified_when, journal_context);
        std::string journal_item_id;
        if (!manual_journal_item.empty()) {
            // Tagged "Tarefa" by hand before Gemini came back: the transcript names it.
            (void)journal_service::SetText(manual_journal_item, transcript_text);
            journal_item_id = manual_journal_item;
        } else if (filing.file) {
            // The journal item owns the take from now on; the recording stays a Note on SD and
            // is hidden from the collections through its journal link.
            journal_item_id = journal_service::CreateItem(filing.type, transcript_text,
                                                          filing.period, pending_recording_id);
            if (!journal_item_id.empty()) {
                (void)recording_archive_service::SetRecordingJournalItem(pending_recording_id,
                                                                         journal_item_id);
            }
            ESP_LOGI(kTag, "Journal filing: id=%s type=%s period=%s item=%s",
                     pending_recording_id.c_str(), journal_service::TypeName(filing.type),
                     filing.period.c_str(),
                     journal_item_id.empty() ? "<failed>" : journal_item_id.c_str());
        } else if (classified_tag == "task") {
            tag = recording_archive_service::RecordingTag::kTask;
        } else if (classified_tag == "idea") {
            tag = recording_archive_service::RecordingTag::kIdea;
        }
        if (tag != recording_archive_service::RecordingTag::kNote) {
            retagged = recording_archive_service::UpdateRecordingTag(pending_recording_id, tag);
            ESP_LOGI(kTag, "Auto-tag: id=%s tag=%s applied=%d", pending_recording_id.c_str(),
                     recording_archive_service::TagName(tag), retagged ? 1 : 0);
        }
        ESP_LOGI(kTag,
                 "Attaching transcript to saved recording: id=%s chars=%u",
                 pending_recording_id.c_str(),
                 static_cast<unsigned>(transcript_text.size()));
        const recording_archive_service::SaveResult save_result =
            recording_archive_service::SaveTranscript(pending_recording_id, transcript_text);
        ESP_LOGI(kTag,
                 "Transcript save result: success=%d transcript_saved=%d metadata_saved=%d transcript=%s metadata=%s error=%s",
                 save_result.success ? 1 : 0,
                 save_result.transcript_saved ? 1 : 0,
                 save_result.metadata_saved ? 1 : 0,
                 save_result.transcript_path.empty() ? "<none>" : save_result.transcript_path.c_str(),
                 save_result.metadata_path.empty() ? "<none>" : save_result.metadata_path.c_str(),
                 save_result.error_code.empty() ? "<none>" : save_result.error_code.c_str());
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.transcript_saved = save_result.transcript_saved;
        s_snapshot.last_saved_transcript_path = save_result.transcript_path;
        s_snapshot.last_transcript = transcript_text;
        s_snapshot.auto_tagged = !classified_tag.empty();
        s_snapshot.auto_tag = retagged ? tag : recording_archive_service::RecordingTag::kNote;
        if (manual_journal_item.empty()) {
            s_snapshot.journal_filed = !journal_item_id.empty();
            s_snapshot.journal_type = s_snapshot.journal_filed
                                          ? journal_service::TypeName(filing.type)
                                          : std::string();
            s_snapshot.journal_period = s_snapshot.journal_filed ? filing.period : std::string();
        }
        s_manual_journal_item.clear();
        s_snapshot.phase = Phase::kComplete;
        s_snapshot.request_in_flight = false;
        s_snapshot.last_status_message = save_result.transcript_saved
                                             ? "Transcrição salva no cartão SD"
                                             : kSavedWithoutTranscriptStatus;
        if (save_result.transcript_saved) {
            s_snapshot.last_error_code = save_result.error_code;
            s_snapshot.last_error_message = save_result.error_message;
        } else {
            s_snapshot.last_error_code = event.snapshot.last_error_code;
            s_snapshot.last_error_message = event.snapshot.last_error_message;
        }
        s_pending_recording_id.clear();
        NotifyLocked();
        return;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_snapshot.phase == Phase::kTranscribing && !event.snapshot.request_in_flight) {
        s_snapshot.phase = s_snapshot.clip_saved ? Phase::kComplete : Phase::kFailed;
        s_snapshot.last_status_message = s_snapshot.clip_saved
                                             ? kSavedWithoutTranscriptStatus
                                             : "Falha na transcrição";
        s_snapshot.last_error_code = event.snapshot.last_error_code;
        s_snapshot.last_error_message = event.snapshot.last_error_message;
        s_pending_recording_id.clear();
    }
    NotifyLocked();
}

const char* PhaseName(Phase phase)
{
    switch (phase) {
        case Phase::kArmed:
            return "armed";
        case Phase::kStartCue:
            return "start_cue";
        case Phase::kRecording:
            return "recording";
        case Phase::kStopCue:
            return "stop_cue";
        case Phase::kPlayingBack:
            return "playing_back";
        case Phase::kAwaitingTagSelection:
            return "awaiting_tag_selection";
        case Phase::kSaving:
            return "saving";
        case Phase::kTranscribing:
            return "transcribing";
        case Phase::kComplete:
            return "complete";
        case Phase::kFailed:
            return "failed";
        case Phase::kIdle:
        default:
            return "idle";
    }
}

}  // namespace recording_session_service

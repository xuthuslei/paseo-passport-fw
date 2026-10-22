#pragma once

/* Phase-3 voice engine: hold-to-talk recording + ASR transcription.
 *
 * Owned by PaseoUi. A dedicated task does the blocking work so the app
 * task (LVGL + key handling) never stalls:
 *   record: ES8311 mic -> PCM 16 kHz mono -> /storage/rec.wav (SPIFFS),
 *           streamed chunk by chunk, never buffered whole in RAM
 *   upload: the file is handed to asr_client, which streams it out as
 *           multipart/form-data; the recording is deleted afterwards.
 * The API below is called from the app task only. Results are delivered
 * through a queue drained by PollResult() from PaseoUi::Tick(). */
#include <atomic>
#include <cstdint>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

enum PaseoVoiceState {
    PV_IDLE = 0,
    PV_RECORDING,
    PV_UPLOADING,
};

struct PaseoVoiceResult {
    bool ok = false;
    bool cancelled = false; /* user discarded it; stay silent */
    char text[1025];        /* recognized text (ok only), UTF-8 */
    char err[64];           /* short failure reason (!ok, !cancelled) */
};

class PaseoVoice {
public:
    /* Mount the "storage" SPIFFS partition and start the worker task.
     * Returns false when the partition is missing/unmountable. */
    bool Init();
    void SetAsrConfig(const std::string &endpoint, const std::string &model,
                      const std::string &key);
    bool HasAsrKey() const { return !asr_key_.empty(); }

    /* Begin recording (worker task). False when busy/not initialized. */
    bool StartRecording();
    /* Finish recording and transcribe (called on OK release). */
    void StopAndTranscribe();
    /* Recording: stop and throw the take away. Uploading: let the HTTP
     * call finish but discard its result (posted as cancelled). */
    void Cancel();

    PaseoVoiceState State() const { return state_.load(); }
    int ElapsedMs() const { return (int)(bytes_.load() / 32); }
    int Level() const { return level_.load(); } /* 0..100, recent peak */

    /* Drain one pending result. True when *out was filled. */
    bool PollResult(PaseoVoiceResult &out);

private:
    static void TaskEntry(void *arg);
    void Run();
    void DoRecordAndTranscribe();

    QueueHandle_t cmd_queue_ = nullptr;    /* int commands */
    QueueHandle_t result_queue_ = nullptr; /* PaseoVoiceResult */
    TaskHandle_t task_ = nullptr;
    bool mounted_ = false;

    std::string asr_endpoint_;
    std::string asr_model_;
    std::string asr_key_; /* never logged, never rendered */

    std::atomic<PaseoVoiceState> state_{PV_IDLE};
    std::atomic<bool> stop_req_{false};
    std::atomic<bool> cancel_req_{false};
    std::atomic<uint32_t> bytes_{0}; /* PCM payload bytes written */
    std::atomic<int> level_{0};
};

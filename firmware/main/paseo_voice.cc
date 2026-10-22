// Phase-3 voice engine implementation. See paseo_voice.h for the shape.
#include "paseo_voice.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <esp_log.h>
#include <esp_spiffs.h>

#include "asr_client.h"
#include "audio_codec.h"
#include "board.h"

#define TAG "paseo_voice"

namespace {
constexpr char kMountPoint[] = "/storage";
constexpr char kRecPath[] = "/storage/rec.wav";
/* PCM 16 kHz mono 16-bit = 32,000 B/s; 60 s cap (design doc section 8). */
constexpr uint32_t kMaxPcmBytes = 60 * 32000;
constexpr size_t kChunkSamples = 1600; /* 100 ms per read */
constexpr int kCmdStart = 1;

struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t chunk_size;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1; /* PCM */
    uint16_t channels = 1;
    uint32_t sample_rate = 16000;
    uint32_t byte_rate = 32000;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;
    char data_id[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size;
};
static_assert(sizeof(WavHeader) == 44, "WAV header must stay 44 bytes");
} // namespace

bool PaseoVoice::Init()
{
    if (task_) return mounted_;
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = kMountPoint;
    conf.partition_label = "storage";
    conf.max_files = 2;
    conf.format_if_mount_failed = true;
    esp_err_t e = esp_vfs_spiffs_register(&conf);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(e));
        return false;
    }
    mounted_ = true;
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "storage ready: %u KB total, %u KB used",
                 (unsigned)(total / 1024), (unsigned)(used / 1024));
    }
    cmd_queue_ = xQueueCreate(2, sizeof(int));
    result_queue_ = xQueueCreate(2, sizeof(PaseoVoiceResult));
    if (!cmd_queue_ || !result_queue_) return false;
    if (xTaskCreate(TaskEntry, "paseo_voice", 12288, this, 5, &task_) !=
        pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        task_ = nullptr;
        return false;
    }
    return true;
}

void PaseoVoice::SetAsrConfig(const std::string &endpoint,
                               const std::string &model,
                               const std::string &key)
{
    asr_endpoint_ = endpoint;
    asr_model_ = model;
    asr_key_ = key;
}

bool PaseoVoice::StartRecording()
{
    if (!task_ || state_.load() != PV_IDLE) return false;
    /* Drop results left over from a take the UI abandoned (e.g. Hide). */
    PaseoVoiceResult stale;
    while (xQueueReceive(result_queue_, &stale, 0) == pdTRUE) {
    }
    stop_req_ = false;
    cancel_req_ = false;
    bytes_ = 0;
    level_ = 0;
    int cmd = kCmdStart;
    return xQueueSend(cmd_queue_, &cmd, 0) == pdTRUE;
}

void PaseoVoice::StopAndTranscribe()
{
    if (state_.load() == PV_RECORDING) stop_req_ = true;
}

void PaseoVoice::Cancel()
{
    if (state_.load() != PV_IDLE) cancel_req_ = true;
}

bool PaseoVoice::PollResult(PaseoVoiceResult &out)
{
    if (!result_queue_) return false;
    return xQueueReceive(result_queue_, &out, 0) == pdTRUE;
}

void PaseoVoice::TaskEntry(void *arg)
{
    static_cast<PaseoVoice *>(arg)->Run();
}

void PaseoVoice::Run()
{
    while (true) {
        int cmd = 0;
        if (xQueueReceive(cmd_queue_, &cmd, portMAX_DELAY) != pdTRUE)
            continue;
        if (cmd == kCmdStart) DoRecordAndTranscribe();
    }
}

void PaseoVoice::DoRecordAndTranscribe()
{
    PaseoVoiceResult res;
    state_ = PV_RECORDING;

    AudioCodec *codec = Board::GetInstance().GetAudioCodec();
    codec->EnableInput(true);

    FILE *f = fopen(kRecPath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open recording file");
        codec->EnableInput(false);
        snprintf(res.err, sizeof(res.err), "录音文件创建失败");
        state_ = PV_IDLE;
        xQueueSend(result_queue_, &res, 0);
        return;
    }
    /* Reserve the header area; it is patched once the size is known. */
    WavHeader placeholder = {};
    fwrite(&placeholder, 1, sizeof(placeholder), f);

    bool write_failed = false;
    while (!stop_req_.load() && !cancel_req_.load() &&
           bytes_.load() < kMaxPcmBytes) {
        /* InputData resizes the vector; hand it a pre-sized one and the
         * codec's Read path fills exactly that many samples (100 ms),
         * blocking on the I2S DMA. Flags are re-checked each chunk. */
        std::vector<int16_t> buf(kChunkSamples);
        if (!codec->InputData(buf)) break;
        int peak = 0;
        for (int16_t s : buf) {
            int a = s < 0 ? -(int)s : (int)s;
            if (a > peak) peak = a;
        }
        int lvl = peak * 100 / 32767;
        int prev = level_.load();
        int decayed = prev * 3 / 4; /* peak-hold with gentle decay */
        level_ = lvl > decayed ? lvl : decayed;
        uint32_t room = kMaxPcmBytes - bytes_.load();
        size_t want = kChunkSamples * sizeof(int16_t);
        if (want > room) want = room;
        if (fwrite(buf.data(), 1, want, f) != want) {
            write_failed = true;
            break;
        }
        bytes_ += (uint32_t)want;
    }
    codec->EnableInput(false);

    /* Finalize the WAV header with the real payload size. */
    uint32_t data_bytes = bytes_.load();
    WavHeader hdr;
    hdr.chunk_size = 36 + data_bytes;
    hdr.data_size = data_bytes;
    fseek(f, 0, SEEK_SET);
    fwrite(&hdr, 1, sizeof(hdr), f);
    fclose(f);
    ESP_LOGI(TAG, "recorded %u bytes (%.1f s)", (unsigned)data_bytes,
             data_bytes / 32000.0);

    if (cancel_req_.load()) {
        remove(kRecPath);
        res.cancelled = true;
        state_ = PV_IDLE;
        xQueueSend(result_queue_, &res, 0);
        return;
    }
    if (write_failed) {
        remove(kRecPath);
        snprintf(res.err, sizeof(res.err), "录音写入失败");
        state_ = PV_IDLE;
        xQueueSend(result_queue_, &res, 0);
        return;
    }
    if (data_bytes < 3200) { /* < 100 ms: almost certainly a stray press */
        remove(kRecPath);
        snprintf(res.err, sizeof(res.err), "录音太短");
        state_ = PV_IDLE;
        xQueueSend(result_queue_, &res, 0);
        return;
    }

    state_ = PV_UPLOADING;
    asr_client_config_t cfg = {};
    cfg.endpoint = asr_endpoint_.c_str();
    cfg.model = asr_model_.c_str();
    cfg.api_key = asr_key_.c_str();
    cfg.timeout_ms = 20000;
    char text[sizeof(res.text)];
    char aerr[64] = "";
    int rc = asr_client_transcribe(&cfg, kRecPath, text, sizeof(text), aerr,
                                   sizeof(aerr));
    remove(kRecPath); /* design rule: delete the recording after upload */
    if (cancel_req_.load()) {
        res.cancelled = true;
    } else if (rc == 0) {
        res.ok = true;
        strncpy(res.text, text, sizeof(res.text) - 1);
        res.text[sizeof(res.text) - 1] = '\0';
    } else {
        snprintf(res.err, sizeof(res.err), "识别失败：%.40s", aerr);
    }
    state_ = PV_IDLE;
    xQueueSend(result_queue_, &res, 0);
}

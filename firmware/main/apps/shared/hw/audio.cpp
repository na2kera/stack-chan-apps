/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "audio.h"

#include <audio/audio_codec.h>
#include <board.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/board/config.h>
#include <mooncake_log.h>

#include "../wav.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace shared::hw {

namespace {

constexpr const char* kTag = "Audio";
constexpr int kSampleRate        = AUDIO_OUTPUT_SAMPLE_RATE;  // CoreS3AudioCodec の出力 (24 kHz)
constexpr size_t kChunkSamples   = kSampleRate / 50;          // 20 ms
constexpr int kTailSilenceChunks = 2;  // 鳴らし終わりに 40 ms の無音を流して DMA の残りを押し出す
constexpr uint32_t kTaskStack    = 4096;
constexpr UBaseType_t kTaskPrio  = 4;

}  // namespace

// 再生タスクと Audio が共有する状態。両方が shared_ptr で持ち、最後に手放した側が解放する。
struct AudioWorker {
    AudioCodec* codec = nullptr;
    TaskHandle_t task = nullptr;
    std::mutex mutex;
    Audio::Pcm pending;        // mutex で保護
    bool has_pending = false;  // mutex で保護
    std::atomic<bool> playing{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> quit{false};
    std::atomic<bool> running{false};
    // 出力の無効化 (EnableOutput(false)) をどちらがするかの取り決め。タスクは終わるとき、end() は
    // 待ちきれずに切り離すときに exchange(true) する。後から来た側が無効化する
    // (正常に止まった場合は end() が無効化する)。
    std::atomic<bool> handoff{false};

    void run();
};

namespace {

// 切り離したタスクの状態。Audio はアプリを開くたびに作り直されるので、ファイルスコープで覚える。
std::weak_ptr<AudioWorker> g_detached;

void taskEntry(void* arg)
{
    auto* holder = static_cast<std::shared_ptr<AudioWorker>*>(arg);
    auto& w      = **holder;
    w.run();
    if (w.handoff.exchange(true)) {
        // end() が先に切り離していった。OutputData() はもう呼ばないので、ここで出力を閉じる。
        w.codec->EnableOutput(false);
        mclog::tagWarn(kTag, "detached audio task exited; output disabled");
    }
    w.running = false;
    delete holder;  // 切り離されていれば、ここで AudioWorker が解放される
    vTaskDelete(nullptr);
}

}  // namespace

Audio::Pcm Audio::parseWav(const uint8_t* start, const uint8_t* end, const char* name)
{
    wav::Pcm parsed;
    const auto status =
        wav::parse(start, static_cast<size_t>(end - start), static_cast<uint32_t>(kSampleRate), parsed);
    switch (status) {
        case wav::Status::Ok: {
            Pcm pcm{parsed.data, parsed.samples, name};
            mclog::tagInfo(kTag, "{}: {} samples ({} ms)", name, pcm.samples, pcm.samples * 1000 / kSampleRate);
            return pcm;
        }
        case wav::Status::NotRiff:
            mclog::tagError(kTag, "{}: not a RIFF/WAVE file", name);
            break;
        case wav::Status::Unsupported: {
            const auto& f = parsed.format;
            mclog::tagError(kTag, "{}: unsupported format (fmt={} ch={} rate={} bits={}); need PCM mono 16-bit {} Hz",
                            name, f.format, f.channels, f.rate, f.bits, kSampleRate);
            break;
        }
        case wav::Status::NotFound:
            mclog::tagError(kTag, "{}: fmt/data chunk not found", name);
            break;
    }
    return Pcm{nullptr, 0, name};
}

bool Audio::begin()
{
    {
        auto old = g_detached.lock();
        if (old != nullptr && old->running.load()) {
            // 前のタスクが終わるときに出力を閉じるので、今出力を開くと途中で閉じられてしまう。
            mclog::tagError(kTag, "previous audio task is still running; audio disabled for this session");
            return false;
        }
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        mclog::tagError(kTag, "audio codec unavailable");
        return false;
    }
    codec->EnableOutput(true);

    worker_          = std::make_shared<AudioWorker>();
    worker_->codec   = codec;
    worker_->running = true;
    auto* holder     = new std::shared_ptr<AudioWorker>(worker_);
    TaskHandle_t handle = nullptr;
    if (xTaskCreate(taskEntry, "app_audio", kTaskStack, holder, kTaskPrio, &handle) != pdPASS) {
        mclog::tagError(kTag, "failed to create audio task");
        delete holder;
        worker_.reset();
        codec->EnableOutput(false);
        return false;
    }
    worker_->task = handle;
    mclog::tagInfo(kTag, "begin: output enabled ({} Hz, volume {})", kSampleRate, codec->output_volume());
    return true;
}

void Audio::end()
{
    if (!worker_) {
        return;
    }
    auto& w = *worker_;
    w.quit  = true;
    w.stop  = true;
    xTaskNotifyGive(w.task);
    // タスクが OutputData から抜けて終わるのを待つ (20 ms 単位で止まるので通常すぐ終わる)。
    for (int i = 0; i < 100 && w.running.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool disable_here = true;
    if (w.running.load()) {
        if (!w.handoff.exchange(true)) {
            // まだ OutputData() の中にいるかもしれないので、ここでは出力を閉じない。
            // タスクが終わるときに閉じる。
            mclog::tagWarn(kTag, "audio task did not stop in 1 s; detached (output is disabled when it exits)");
            g_detached   = worker_;
            disable_here = false;
        }
        // exchange が true: 待ちきった直後にタスクが run() を抜けた。出力はこちらで閉じる。
    }
    if (disable_here) {
        w.codec->EnableOutput(false);
        mclog::tagInfo(kTag, "end: output disabled");
    }
    worker_.reset();
}

bool Audio::isPlaying() const
{
    return worker_ != nullptr && worker_->playing.load();
}

bool Audio::play(const Pcm& pcm)
{
    if (!worker_ || pcm.data == nullptr || pcm.samples == 0) {
        mclog::tagWarn(kTag, "play {}: unavailable", pcm.name);
        return false;
    }
    {
        // playing は mutex の中で立てる。タスクは mutex の中で「要求なし」を確認したときだけ下ろすので、
        // 呼び出し直後の isPlaying() が false になることも、true のまま残ることもない。
        std::lock_guard<std::mutex> lock(worker_->mutex);
        worker_->pending     = pcm;
        worker_->has_pending = true;
        worker_->stop        = false;
        worker_->playing     = true;
    }
    xTaskNotifyGive(worker_->task);
    mclog::tagInfo(kTag, "play {}", pcm.name);
    return true;
}

void Audio::stop()
{
    if (!worker_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(worker_->mutex);
        worker_->has_pending = false;
    }
    worker_->stop = true;
}

void AudioWorker::run()
{
    std::vector<int16_t> chunk;
    chunk.reserve(kChunkSamples);

    while (!quit.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        Audio::Pcm pcm;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!has_pending) {
                playing = false;  // 要求が無い (stop() で取り消された場合も含む)
                continue;
            }
            pcm         = pending;
            has_pending = false;
        }

        size_t pos = 0;
        while (pos < pcm.samples && !quit.load() && !stop.load()) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (has_pending) {
                    break;  // 新しい再生要求に差し替える
                }
            }
            const size_t n = std::min(kChunkSamples, pcm.samples - pos);
            chunk.resize(n);
            std::memcpy(chunk.data(), pcm.data + pos * 2, n * 2);  // 埋め込みデータは 2 バイト境界とは限らない
            codec->OutputData(chunk);
            pos += n;
        }

        if (!quit.load()) {
            chunk.assign(kChunkSamples, 0);
            for (int i = 0; i < kTailSilenceChunks; ++i) {
                codec->OutputData(chunk);
            }
        }
        mclog::tagInfo(kTag, "{} {} ({} / {} samples)", pcm.name, pos >= pcm.samples ? "done" : "stopped", pos,
                       pcm.samples);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (has_pending) {
                xTaskNotifyGive(xTaskGetCurrentTaskHandle());  // 差し替え要求をすぐ拾う
            } else {
                playing = false;
            }
        }
    }
    playing = false;
}

}  // namespace shared::hw

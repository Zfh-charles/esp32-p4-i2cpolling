#include "face_route_v2.h"

#include <atomic>

#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "afe_fetch_gate.h"

#define TAG "FaceRouteV2"

namespace {

std::atomic<bool> g_a_worker{S1CN_A_WORKER != 0};
std::atomic<bool> g_b_emotion3{S1CN_B_EMOTION3 != 0};
std::atomic<bool> g_c_fs_gate{S1CN_C_FS_GATE != 0};
std::atomic<bool> g_d_quiet{S1CN_D_QUIET_LOG != 0};
std::atomic<bool> g_e_static_dialogue{S1CO_E_STATIC_DIALOGUE != 0};
std::atomic<bool> g_f_full_still{S1CP_F_FULL_STILL != 0};
std::atomic<bool> g_g_enter_arc{S1CP_G_ENTER_ARC != 0};

std::atomic<bool> g_fs_busy{false};
std::atomic<bool> g_fs_holds_afe{false};

FaceRouteV2TickFn g_tick_fn = nullptr;
void* g_tick_ctx = nullptr;
QueueHandle_t g_tick_q = nullptr;
TaskHandle_t g_worker = nullptr;
std::atomic<uint32_t> g_frame_log_n{0};

void FaceWorkerTask(void* /*arg*/) {
    for (;;) {
        uint32_t token = 0;
        if (xQueueReceive(g_tick_q, &token, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // Drain to latest (overwrite semantics).
        while (xQueueReceive(g_tick_q, &token, 0) == pdTRUE) {
        }
        FaceRouteV2TickFn fn = g_tick_fn;
        void* ctx = g_tick_ctx;
        if (fn != nullptr && ctx != nullptr) {
            fn(ctx);
        }
    }
}

}  // namespace

bool FaceRouteV2_WorkerEnabled(void) {
    return g_a_worker.load(std::memory_order_relaxed);
}
bool FaceRouteV2_Emotion3Enabled(void) {
    return g_b_emotion3.load(std::memory_order_relaxed);
}
bool FaceRouteV2_FsGateEnabled(void) {
    return g_c_fs_gate.load(std::memory_order_relaxed);
}
bool FaceRouteV2_QuietLogEnabled(void) {
    return g_d_quiet.load(std::memory_order_relaxed);
}
bool FaceRouteV2_StaticDialogueEnabled(void) {
    return g_e_static_dialogue.load(std::memory_order_relaxed);
}
bool FaceRouteV2_FullStillEnabled(void) {
    return g_f_full_still.load(std::memory_order_relaxed);
}
bool FaceRouteV2_EnterArcEnabled(void) {
    return g_g_enter_arc.load(std::memory_order_relaxed);
}

void FaceRouteV2_SetWorker(bool on) {
    g_a_worker.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetEmotion3(bool on) {
    g_b_emotion3.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetFsGate(bool on) {
    g_c_fs_gate.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetQuietLog(bool on) {
    g_d_quiet.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetStaticDialogue(bool on) {
    g_e_static_dialogue.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetFullStill(bool on) {
    g_f_full_still.store(on, std::memory_order_relaxed);
}
void FaceRouteV2_SetEnterArc(bool on) {
    g_g_enter_arc.store(on, std::memory_order_relaxed);
}

void FaceRouteV2_BootLog(void) {
    ESP_LOGW(TAG,
             "s1cp flags a=%d b=%d c=%d d=%d e_static=%d f_fullstill=%d g_enter=%d",
             FaceRouteV2_WorkerEnabled() ? 1 : 0, FaceRouteV2_Emotion3Enabled() ? 1 : 0,
             FaceRouteV2_FsGateEnabled() ? 1 : 0, FaceRouteV2_QuietLogEnabled() ? 1 : 0,
             FaceRouteV2_StaticDialogueEnabled() ? 1 : 0, FaceRouteV2_FullStillEnabled() ? 1 : 0,
             FaceRouteV2_EnterArcEnabled() ? 1 : 0);
    esp_rom_printf("!!FACE_S1CP a=%d b=%d c=%d d=%d e=%d f=%d g=%d\n",
                   FaceRouteV2_WorkerEnabled() ? 1 : 0, FaceRouteV2_Emotion3Enabled() ? 1 : 0,
                   FaceRouteV2_FsGateEnabled() ? 1 : 0, FaceRouteV2_QuietLogEnabled() ? 1 : 0,
                   FaceRouteV2_StaticDialogueEnabled() ? 1 : 0,
                   FaceRouteV2_FullStillEnabled() ? 1 : 0, FaceRouteV2_EnterArcEnabled() ? 1 : 0);
}

void FaceRouteV2_EnsureWorker(FaceRouteV2TickFn tick_fn, void* ctx) {
    g_tick_fn = tick_fn;
    g_tick_ctx = ctx;
    if (g_tick_q == nullptr) {
        g_tick_q = xQueueCreate(1, sizeof(uint32_t));
    }
    if (g_worker != nullptr || g_tick_q == nullptr || tick_fn == nullptr) {
        return;
    }
    // Core 1 with LVGL; prio 3 (< LVGL 4); stack 12288 (s1bi/s1bh: no tiny stacks).
    BaseType_t ok =
        xTaskCreatePinnedToCore(FaceWorkerTask, "face_worker", 12288, nullptr, 3, &g_worker, 1);
    if (ok != pdPASS) {
        g_worker = nullptr;
        ESP_LOGE(TAG, "s1cn-a face_worker create FAIL");
        esp_rom_printf("!!FACE_S1CN a=create_fail\n");
        return;
    }
    ESP_LOGW(TAG, "s1cn-a face_worker up stack=12288 core=1 prio=3");
    esp_rom_printf("!!FACE_S1CN a=worker_up\n");
}

void FaceRouteV2_PostTick(void) {
    if (g_tick_q == nullptr) {
        return;
    }
    uint32_t token = 1;
    // Overwrite: if full, drop old then send.
    if (xQueueSend(g_tick_q, &token, 0) != pdTRUE) {
        uint32_t dumped = 0;
        (void)xQueueReceive(g_tick_q, &dumped, 0);
        (void)xQueueSend(g_tick_q, &token, 0);
    }
}

bool FaceRouteV2_TryBeginFullscreen(const char* why, bool afe_already_held) {
    if (!FaceRouteV2_FsGateEnabled()) {
        return true;
    }
    bool expected = false;
    if (!g_fs_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "s1cn-c skip_busy why=%s", why ? why : "-");
        esp_rom_printf("!!FACE_S1CN c=skip_busy\n");
        return false;
    }
    if (!afe_already_held) {
        if (!AfeFetchGateTryLock(60)) {
            g_fs_busy.store(false, std::memory_order_release);
            ESP_LOGW(TAG, "s1cn-c skip_afe why=%s", why ? why : "-");
            esp_rom_printf("!!FACE_S1CN c=skip_afe\n");
            return false;
        }
        g_fs_holds_afe.store(true, std::memory_order_relaxed);
    } else {
        g_fs_holds_afe.store(false, std::memory_order_relaxed);
    }
    ESP_LOGW(TAG, "s1cn-c begin why=%s afe_held=%d", why ? why : "-", afe_already_held ? 1 : 0);
    return true;
}

void FaceRouteV2_EndFullscreen(void) {
    if (!g_fs_busy.load(std::memory_order_acquire)) {
        return;
    }
    if (g_fs_holds_afe.exchange(false, std::memory_order_acq_rel)) {
        AfeFetchGateUnlock();
    }
    g_fs_busy.store(false, std::memory_order_release);
}

bool FaceRouteV2_FullscreenBusy(void) {
    return g_fs_busy.load(std::memory_order_acquire);
}

bool FaceRouteV2_ShouldLogFrame(void) {
    if (!FaceRouteV2_QuietLogEnabled()) {
        return true;
    }
    // ~1/8 of per-frame rows/cost/breathe ticks.
    const uint32_t n = g_frame_log_n.fetch_add(1, std::memory_order_relaxed) + 1;
    return (n & 7u) == 1u;
}

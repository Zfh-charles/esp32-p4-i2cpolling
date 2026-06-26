#include <sdkconfig.h>
#include "reminder/boot_trace.h"

#if CONFIG_USE_REMINDER_POLL && CONFIG_REMINDER_BOOT_TRACE

#include <cstring>
#include <deque>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#define TAG "BootTrace"
#define FW_MARKER "boot_trace_v2_scheme_b"

namespace {

constexpr size_t kPhaseRingCap = 24;
constexpr size_t kBootCountRtcWords = 2;

RTC_NOINIT_ATTR uint32_t g_rtc_boot_magic;
RTC_NOINIT_ATTR uint32_t g_rtc_boot_count;
RTC_NOINIT_ATTR char g_rtc_last_phase[20];
RTC_NOINIT_ATTR uint32_t g_rtc_last_phase_ms;

struct PhaseEntry {
    char phase[20];
    char detail[32];
    uint32_t t_ms;
    uint32_t heap_int;
    uint32_t heap_psram;
    uint32_t heap_largest;
};

int64_t g_boot_us = 0;
std::deque<PhaseEntry> g_phases;
char g_last_phase[20] = "BOOT";

uint32_t BootMs() {
    if (g_boot_us <= 0) {
        return 0;
    }
    return static_cast<uint32_t>((esp_timer_get_time() - g_boot_us) / 1000LL);
}

void CopyField(char* dst, size_t len, const char* src) {
    if (dst == nullptr || len == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

const char* ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "POWERON";
        case ESP_RST_EXT:
            return "EXT";
        case ESP_RST_SW:
            return "SW";
        case ESP_RST_PANIC:
            return "PANIC";
        case ESP_RST_INT_WDT:
            return "INT_WDT";
        case ESP_RST_TASK_WDT:
            return "TASK_WDT";
        case ESP_RST_WDT:
            return "WDT";
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_SDIO:
            return "SDIO";
        case ESP_RST_USB:
            return "USB";
        case ESP_RST_JTAG:
            return "JTAG";
        default:
            return "UNKNOWN";
    }
}

void PersistCrashHint(const char* phase) {
    g_rtc_boot_magic = 0xB0070002;
    CopyField(g_rtc_last_phase, sizeof(g_rtc_last_phase), phase);
    g_rtc_last_phase_ms = BootMs();
}

void RecordPhase(const char* phase, const char* detail, bool log_heap) {
    PhaseEntry entry{};
    CopyField(entry.phase, sizeof(entry.phase), phase);
    CopyField(entry.detail, sizeof(entry.detail), detail);
    entry.t_ms = BootMs();
    entry.heap_int = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    entry.heap_psram = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    entry.heap_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    g_phases.push_back(entry);
    while (g_phases.size() > kPhaseRingCap) {
        g_phases.pop_front();
    }
    CopyField(g_last_phase, sizeof(g_last_phase), phase);
    PersistCrashHint(phase);

    if (log_heap) {
        ESP_LOGI(TAG,
                 "PH | t=%lums phase=%s detail=%s heap_int=%lu heap_psram=%lu largest=%lu",
                 (unsigned long)entry.t_ms,
                 entry.phase,
                 entry.detail[0] ? entry.detail : "-",
                 (unsigned long)entry.heap_int,
                 (unsigned long)entry.heap_psram,
                 (unsigned long)entry.heap_largest);
    } else {
        ESP_LOGI(TAG, "PH | t=%lums phase=%s detail=%s",
                 (unsigned long)entry.t_ms,
                 entry.phase,
                 entry.detail[0] ? entry.detail : "-");
    }
}

void RegisterCrashHandlers() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    esp_register_shutdown_handler([]() {
        const esp_reset_reason_t pending = esp_reset_reason();
        (void)pending;
        ESP_LOGW(TAG, "CRASH | shutdown last_phase=%s t=%lums",
                 g_last_phase, (unsigned long)BootMs());
        BootTraceDumpSummary("shutdown");
    });
}

}  // namespace

void BootTraceInit() {
    g_boot_us = esp_timer_get_time();
    RegisterCrashHandlers();

    if (g_rtc_boot_magic != 0xB0070002) {
        g_rtc_boot_count = 0;
        g_rtc_boot_magic = 0xB0070002;
    }
    g_rtc_boot_count++;

    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "FW_MARKER %s", FW_MARKER);
    ESP_LOGI(TAG, "BOOT | reason=%s boot_count=%lu prev_phase=%s prev_t=%lums",
             ResetReasonName(reason),
             (unsigned long)g_rtc_boot_count,
             g_rtc_last_phase[0] ? g_rtc_last_phase : "-",
             (unsigned long)g_rtc_last_phase_ms);

    if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT ||
        reason == ESP_RST_INT_WDT || reason == ESP_RST_PANIC) {
        ESP_LOGW(TAG, "REBOOT_AFTER | reason=%s prev_phase=%s prev_t=%lums",
                 ResetReasonName(reason),
                 g_rtc_last_phase[0] ? g_rtc_last_phase : "-",
                 (unsigned long)g_rtc_last_phase_ms);
    }

    RecordPhase("APP_MAIN", ResetReasonName(reason), true);
}

void BootTraceMark(const char* phase, const char* detail) {
    RecordPhase(phase, detail, false);
}

void BootTraceMarkHeap(const char* phase) {
    RecordPhase(phase, nullptr, true);
}

void BootTraceDumpSummary(const char* reason) {
    std::string line;
    line.reserve(512);
    line += "SUMMARY | reason=";
    line += reason ? reason : "?";
    line += " boots=";
    line += std::to_string(g_rtc_boot_count);
    line += " last=";
    line += g_last_phase;
    line += " t=";
    line += std::to_string(BootMs());
    line += "ms | ring:";
    for (const auto& e : g_phases) {
        line += " ";
        line += e.phase;
        line += "@";
        line += std::to_string(e.t_ms);
    }
    ESP_LOGW(TAG, "%s", line.c_str());
}

const char* BootTraceLastPhase() {
    return g_last_phase;
}

#endif

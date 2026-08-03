#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <string>

enum class ReminderDeliveryMode {
    kMcpWake,      // idle 主动推流：短 detect + 云端 MCP 读队列 + 延后 ack
    kDirectWake,   // 直接把 prompt 当 detect（仅适合短句）
    kAlertOnly,    // 仅屏幕/振动，不连云 TTS
};

enum class ReminderAlarmMode {
    kNone,         // 播报/显示一次，ACK 后回待机
    kRingRepeat,   // 播报后本地响铃，并按配置周期重复
};

class ReminderPoller {
public:
    ReminderPoller();
    ~ReminderPoller();

    void Start();
    void Stop();

    /** Wake the poll loop immediately (e.g. MQTT push notification). */
    void TriggerPoll();

    /** Keep one coalesced poll request pending until the device is idle. */
    void DeferPoll(const char* reason);

    /** Wake a deferred request after the application has fully returned to idle. */
    void NotifyIdleReady();

    static void EnsureNvsConfigured();
    static bool PostAck(const std::string& ack_url, const std::string& id);

private:
    bool running_ = false;
    TaskHandle_t poll_task_handle_ = nullptr;
    std::atomic<bool> deferred_poll_pending_{false};
    std::atomic<bool> deferred_wait_logged_{false};
    void PollTask();
    void DoPollOnce();
    void TracePollBlocked(const char* reason);
    static std::string ExpandDeviceIdInUrl(const std::string& url_template);
    static bool ParsePendingResponse(const std::string& body, std::string& id, std::string& prompt,
                                     std::string& emotion, ReminderDeliveryMode& mode,
                                     ReminderAlarmMode& alarm_mode, std::string& wake_text);
};

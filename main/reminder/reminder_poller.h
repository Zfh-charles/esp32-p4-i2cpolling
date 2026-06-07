#pragma once

#include <string>

enum class ReminderDeliveryMode {
    kMcpWake,      // idle 主动推流：短 detect + 云端 MCP 读队列 + 延后 ack
    kDirectWake,   // 直接把 prompt 当 detect（仅适合短句）
    kAlertOnly,    // 仅屏幕/振动，不连云 TTS
};

class ReminderPoller {
public:
    ReminderPoller();
    ~ReminderPoller();

    void Start();
    void Stop();

    static void EnsureNvsConfigured();
    static bool PostAck(const std::string& ack_url, const std::string& id);

private:
    bool running_ = false;
    void PollTask();
    void DoPollOnce();
    static std::string ExpandDeviceIdInUrl(const std::string& url_template);
    static bool ParsePendingResponse(const std::string& body, std::string& id, std::string& prompt,
                                     std::string& emotion, ReminderDeliveryMode& mode,
                                     std::string& wake_text);
};

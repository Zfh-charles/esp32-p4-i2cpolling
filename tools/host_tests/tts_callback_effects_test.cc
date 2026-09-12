// Production TTS branch is inserted verbatim by test_tts_callback_effects.py.
// Only branch orchestration is tested. Hardware, SetDeviceState, VisualPort,
// reminder completion, and the executor are observable stubs, not ESP runtime.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "cJSON.h"

#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ReminderTraceLog(...) ((void)0)
#define ReminderLcMark(...) ((void)0)
#define ReminderUiTraceTts(...) ((void)0)
#define ReminderLcEmotion(...) ((void)0)
enum class ReminderLcOwner { User, Proactive };
static void ReminderLcDisplay(ReminderLcOwner, const char*, const char*, int) {}
enum class SessionKind { User, ProactiveReminder };
enum DeviceState { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking };
enum ListeningMode { kListeningModeAutoStop, kListeningModeManualStop };
static constexpr int MAIN_EVENT_TTS_UDP_PRIME = 4;
struct Events { int primes = 0; };
static void xEventGroupSetBits(Events* e, int bits) {
    if (bits != MAIN_EVENT_TTS_UDP_PRIME) std::exit(2);
    ++e->primes;
}
struct Display {
    int starts = 0;
    std::vector<std::string> messages;
    std::string emotion;
    void SetChatMessage(const char*, const char* text) { messages.emplace_back(text); }
    void SetEmotion(const char* text) { emotion = text; }
};
struct Board {
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
};
struct VisualPort {
    Display* display;
    explicit VisualPort(Display* d) : display(d) {}
    void NotifyTtsStart() { ++display->starts; }
};
struct Audio {
    std::vector<std::string> effects;
    void PrepareSpeakerPlayback() { effects.emplace_back("prepare"); }
    void ReleasePlaybackPrebuffer() { effects.emplace_back("release"); }
};
struct App {
    std::atomic<bool> visual_tts_audio_started_{true};
    bool aborted_ = true;
    bool proactive_reminder_tts_started_ = false;
    SessionKind session_kind_ = SessionKind::User;
    DeviceState device_state_ = kDeviceStateListening;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    std::string pending_reminder_ack_id_, last_reminder_emotion_;
    int proactive_audio_packets_ = 0, canceled = 0, finished = 0, feedback = 0;
    Audio audio_service_;
    Events events;
    Events* event_group_ = &events;
    Display display;
    std::deque<std::function<void()>> work;
    void Schedule(std::function<void()> task) { work.push_back(std::move(task)); }
    void Drain() {
        while (!work.empty()) { auto task = std::move(work.front()); work.pop_front(); task(); }
    }
    void SetDeviceState(DeviceState state) { device_state_ = state; }
    void StopProactiveReminderTimeout() {}
    void CancelPendingReminderAck() { ++canceled; audio_service_.effects.emplace_back("cancel"); }
    void FinishProactiveReminder() { ++finished; audio_service_.effects.emplace_back("finish"); }
    void StartProactiveFeedbackWindow() { ++feedback; audio_service_.effects.emplace_back("feedback"); }
    void Dispatch(const cJSON* root) {
        auto type = cJSON_GetObjectItem(root, "type");
        auto display = &this->display;
        // Test input is a parsed object with a valid type. Outer dispatch is NOT tested.
        /* PRODUCTION_TTS_BRANCH */
    }
    void Receive(const std::string& json) {
        cJSON* root = cJSON_Parse(json.c_str());
        if (!root) std::exit(2);
        Dispatch(root);
        cJSON_Delete(root); // All deferred effects run after JSON ownership ends.
    }
};
static void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
    App user;
    user.Receive(R"({"type":"tts","state":"start"})");
    Require(!user.visual_tts_audio_started_ && user.display.starts == 1,
            "start resets first-audio latch and notifies visual once");
    Require(user.device_state_ == kDeviceStateListening && user.work.size() == 1,
            "start transition must be scheduled");
    user.Drain();
    Require(!user.aborted_ && user.device_state_ == kDeviceStateSpeaking, "start enters speaking");
    for (int i = 0; i < 32; ++i) {
        user.Receive("{\"type\":\"tts\",\"state\":\"sentence_start\",\"text\":\"sentence-" +
                     std::to_string(i) + "\"}");
    }
    Require(user.display.starts == 1, "sentences must not restart visual generation");
    Require(user.events.primes == 32, "each sentence primes UDP");
    Require(user.display.messages.empty(), "subtitle must stay deferred");
    user.Drain();
    Require(user.display.messages.size() == 32, "all sentence subtitles delivered");
    for (int i = 0; i < 32; ++i)
        Require(user.display.messages[i] == "sentence-" + std::to_string(i),
                "deferred subtitle owns original text and ordering");
    Require(user.device_state_ == kDeviceStateSpeaking, "sentences do not end speaking");
    user.Receive(R"({"type":"tts","state":"stop"})");
    Require(user.device_state_ == kDeviceStateSpeaking, "stop is scheduled");
    user.Drain();
    Require(user.device_state_ == kDeviceStateListening, "auto stop returns to listening");
    user.visual_tts_audio_started_ = true;
    user.Receive(R"({"type":"tts","state":"start"})");
    user.Drain();
    Require(user.display.starts == 2 && !user.visual_tts_audio_started_, "next TTS rearms visual latch");
    user.listening_mode_ = kListeningModeManualStop;
    user.Receive(R"({"type":"tts","state":"stop"})"); user.Drain();
    Require(user.device_state_ == kDeviceStateIdle, "manual stop returns to idle");
    user.listening_mode_ = kListeningModeAutoStop;
    user.Receive(R"({"type":"tts","state":"stop"})"); user.Drain();
    Require(user.device_state_ == kDeviceStateIdle, "late stop must not wake idle");
    const int starts = user.display.starts;
    for (const char* state : {"null", "42", "{}", "[]", "\"future\""}) {
        user.Receive(std::string("{\"type\":\"tts\",\"state\":") + state + "}");
    }
    Require(user.work.empty() && user.display.starts == starts, "invalid/unknown state has no effects");
#if CONFIG_USE_REMINDER_POLL
    for (const int packets : {0, 2}) {
        App reminder;
        reminder.session_kind_ = SessionKind::ProactiveReminder;
        reminder.pending_reminder_ack_id_ = "fixture";
        reminder.last_reminder_emotion_ = "happy";
        reminder.Receive(R"({"type":"tts","state":"start"})"); reminder.Drain();
        Require(reminder.proactive_reminder_tts_started_, "reminder start records lifecycle");
        Require(reminder.audio_service_.effects == std::vector<std::string>{"prepare"},
                "reminder prepares playback without early completion");
        Require(reminder.finished == 0 && reminder.feedback == 0 && reminder.canceled == 0,
                "JSON start must not complete reminder");
        Require(Board::GetInstance().display.emotion == "happy", "reminder retains emotion");
        reminder.proactive_audio_packets_ = packets;
        reminder.Receive(R"({"type":"tts","state":"stop"})"); reminder.Drain();
        const auto& effects = reminder.audio_service_.effects;
        Require(effects.size() == 3 && effects[1] == "release", "release prebuffer precedes completion");
        if (!packets) {
            Require(effects[2] == "cancel" && !reminder.proactive_reminder_tts_started_,
                    "no audio cancels instead of successful completion");
        } else {
#if CONFIG_REMINDER_FEEDBACK_SEC > 0
            Require(effects[2] == "feedback", "audio enters configured feedback path");
#else
            Require(effects[2] == "finish", "audio enters configured finish path");
#endif
        }
    }
#endif
    std::puts("PASS: production TTS branch effects; 32 sentences, ownership, next TTS, stop and reminder paths; NO hardware/wake/ACK internals proof");
}

#pragma once
#include "display.h"
#include <cstdint>
#include <vector>

// Device-side effects only are stubbed; VisualPort itself is production code.
class EezuiDisplayAdapter : public Display {
public:
    std::vector<int> calls;
    uint32_t fps = 0;
    bool preload_result = false;
    void PauseMjpegHeavyWork() { calls.push_back(1); }
    void ResumeMjpegHeavyWork() { calls.push_back(2); }
    void ResumeMjpegHeavyWorkAtFps(uint32_t value) { calls.push_back(3); fps = value; }
    void StartDeferredEmotionPreload() { calls.push_back(4); }
    bool PreloadBaseEmotionsSync() { calls.push_back(5); return preload_result; }
    void EnterConversationPresent() { calls.push_back(6); }
    void LeaveConversationPresent() { calls.push_back(7); }
    void NotifyTtsStart() { calls.push_back(8); }
    void NotifyTtsAudioFirst() { calls.push_back(9); }
};

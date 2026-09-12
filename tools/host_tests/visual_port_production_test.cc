#include "ports/visual_port.h"
#include "boards/ep-chat-p4-ml307/eezui_display_adapter.h"
#include <cassert>
#include <vector>

static bool Exercise(VisualPort port) {
    port.PauseMjpegHeavyWork();
    port.ResumeMjpegHeavyWork();
    port.ResumeMjpegHeavyWorkAtFps(7);
    port.StartDeferredEmotionPreload();
    const bool loaded = port.PreloadBaseEmotionsSync();
    port.EnterConversationPresent();
    port.LeaveConversationPresent();
    port.NotifyTtsStart();
    port.NotifyTtsAudioFirst();
    return loaded;
}

int main() {
    Display unrelated;
    assert(!VisualPort(nullptr).Available());
    assert(!VisualPort(&unrelated).Available());
    assert(!Exercise(VisualPort(nullptr)));
    assert(!Exercise(VisualPort(&unrelated)));

    EezuiDisplayAdapter display;
    VisualPort port(&display);
#if CONFIG_BOARD_TYPE_EP_CHAT_P4_ML307
    assert(port.Available());
    assert(!Exercise(port));
    assert((display.calls == std::vector<int>{1,2,3,4,5,6,7,8,9}));
    assert(display.fps == 7);
    display.preload_result = true;
    assert(port.PreloadBaseEmotionsSync());
    assert(display.calls.back() == 5);
    display.calls.clear();
    // Transparent facade must preserve repeated notifications and edges:
    // deduplication/ordering policy belongs elsewhere, not inside this port.
    port.NotifyTtsStart();
    port.NotifyTtsAudioFirst();
    port.NotifyTtsAudioFirst();
    port.LeaveConversationPresent();
    port.PauseMjpegHeavyWork();
    port.EnterConversationPresent();
    assert((display.calls == std::vector<int>{8,9,9,7,1,6}));
#else
    assert(!port.Available());
    assert(!Exercise(port));
    assert(display.calls.empty());
#endif
}

#include "face_route_v2.h"
#include "afe_fetch_gate.h"
#include <cassert>
#include <cstdio>

namespace {
bool allow_afe = true;
unsigned attempts = 0;
unsigned releases = 0;
uint32_t timeout_seen = 0;
}

extern "C" bool AfeFetchGateTryLock(uint32_t timeout_ms) {
    ++attempts;
    timeout_seen = timeout_ms;
    return allow_afe;
}
extern "C" void AfeFetchGateUnlock(void) { ++releases; }

int main() {
    assert(FaceRouteV2_FsGateEnabled());
    assert(!FaceRouteV2_FullscreenBusy());
    FaceRouteV2_EndFullscreen();
    assert(releases == 0);

    // Existing singleton: a second begin while owned must not touch AFE.
    assert(FaceRouteV2_TryBeginFullscreen("seed", false));
    assert(FaceRouteV2_FullscreenBusy());
    assert(attempts == 1 && timeout_seen == 60);
    assert(!FaceRouteV2_TryBeginFullscreen("mouth_base", false));
    assert(attempts == 1 && releases == 0);
    FaceRouteV2_EndFullscreen();
    assert(!FaceRouteV2_FullscreenBusy() && releases == 1);
    FaceRouteV2_EndFullscreen();
    assert(releases == 1);

    // Failed AFE admission must release busy so the next request can proceed.
    allow_afe = false;
    assert(!FaceRouteV2_TryBeginFullscreen(nullptr, false));
    assert(!FaceRouteV2_FullscreenBusy());
    assert(attempts == 2 && releases == 1);
    allow_afe = true;
    assert(FaceRouteV2_TryBeginFullscreen("retry", false));
    FaceRouteV2_EndFullscreen();
    assert(attempts == 3 && releases == 2);

    // Caller-owned AFE must neither be acquired nor released by this gate.
    assert(FaceRouteV2_TryBeginFullscreen("caller_owned", true));
    assert(!FaceRouteV2_TryBeginFullscreen("overlap", true));
    FaceRouteV2_EndFullscreen();
    assert(attempts == 3 && releases == 2);

    // R0 behavior is characterized at an idle boundary, not mid-transaction.
    FaceRouteV2_SetFsGate(false);
    assert(FaceRouteV2_TryBeginFullscreen("bypass", false));
    assert(!FaceRouteV2_FullscreenBusy());
    FaceRouteV2_EndFullscreen();
    assert(attempts == 3 && releases == 2);
    FaceRouteV2_SetFsGate(true);
    std::puts("PASS: production singleton/AFE ownership characterization (AFE stubbed)");

    // A single test window with no speech edge between calls. The production
    // API has no generation input: it cannot itself enforce a per-TTS budget.
    // This is evidence of a coverage gap, NOT an assertion that all three
    // effects reach the screen or that a full application run repeats them.
    unsigned admitted = 0;
    for (unsigned n = 0; n < 3; ++n) {
        if (FaceRouteV2_TryBeginFullscreen("same_reason", false)) {
            ++admitted;
            FaceRouteV2_EndFullscreen();
        }
    }
    std::printf("OBSERVATION: sequential_admitted=%u/3; no speech-generation input\n", admitted);
    assert(admitted == 3);
    assert(attempts == 6 && releases == 5 && !FaceRouteV2_FullscreenBusy());
    std::puts("KNOWN_GAP: singleton is not a per-TTS/full-frame burst budget; no firmware fix claimed");
}

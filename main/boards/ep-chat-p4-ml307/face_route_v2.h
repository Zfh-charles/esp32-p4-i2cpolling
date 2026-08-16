#pragma once

/**
 * s1cn–s1cp route-v2 defense batch — orthogonal runtime flags (G2).
 *
 * | Flag | Define                 | Role |
 * |------|------------------------|------|
 * | a    | S1CN_A_WORKER          | face_worker: decode/present off main_event_loop |
 * | b    | S1CN_B_EMOTION3        | emotion requested/pending/committed |
 * | c    | S1CN_C_FS_GATE         | fullscreen singleton + AFE safety window |
 * | d    | S1CN_D_QUIET_LOG       | rate-limit per-frame FACE_* logs |
 * | e    | S1CO_E_STATIC_DIALOGUE | dialogue StaticHold — no MID 400-row band |
 * | f    | S1CP_F_FULL_STILL      | breathe/hold/bookend use seed_full (kill seam) |
 * | g    | S1CP_G_ENTER_ARC       | short full-frame enter, then hold on seed |
 *
 * Mouth layer (h) lives in face_mouth_layer.h as S1CR_H_MOUTH — not toggled here.
 *
 * Default ON for this marker; bisect = set one define to 0 and rebuild,
 * or FaceRouteV2_Set*(false) then soft-reboot (atomics survive in RAM until reset).
 * Known-good rollback = reflash s1cm (E8); structural-only extract = s1db.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef S1CN_A_WORKER
#define S1CN_A_WORKER 1
#endif
#ifndef S1CN_B_EMOTION3
#define S1CN_B_EMOTION3 1
#endif
#ifndef S1CN_C_FS_GATE
#define S1CN_C_FS_GATE 1
#endif
#ifndef S1CN_D_QUIET_LOG
#define S1CN_D_QUIET_LOG 1
#endif
#ifndef S1CO_E_STATIC_DIALOGUE
#define S1CO_E_STATIC_DIALOGUE 1
#endif
#ifndef S1CP_F_FULL_STILL
#define S1CP_F_FULL_STILL 1
#endif
#ifndef S1CP_G_ENTER_ARC
#define S1CP_G_ENTER_ARC 1
#endif
#ifndef S1EF_I_LIFE_LAYER
#define S1EF_I_LIFE_LAYER 1
#endif

bool FaceRouteV2_WorkerEnabled(void);
bool FaceRouteV2_Emotion3Enabled(void);
bool FaceRouteV2_FsGateEnabled(void);
bool FaceRouteV2_QuietLogEnabled(void);
bool FaceRouteV2_StaticDialogueEnabled(void);
bool FaceRouteV2_FullStillEnabled(void);
bool FaceRouteV2_EnterArcEnabled(void);
bool FaceRouteV2_LifeLayerEnabled(void);

void FaceRouteV2_SetWorker(bool on);
void FaceRouteV2_SetEmotion3(bool on);
void FaceRouteV2_SetFsGate(bool on);
void FaceRouteV2_SetQuietLog(bool on);
void FaceRouteV2_SetStaticDialogue(bool on);
void FaceRouteV2_SetFullStill(bool on);
void FaceRouteV2_SetEnterArc(bool on);
void FaceRouteV2_SetLifeLayer(bool on);

/** Boot banner: FW sub-markers s1cn-a..d on/off. */
void FaceRouteV2_BootLog(void);

/**
 * s1cn-a: queue-depth-1 face worker. tick_fn(ctx) runs on Core1 stack>=12288.
 * Safe to call repeatedly; first call creates the task.
 */
typedef void (*FaceRouteV2TickFn)(void* ctx);
void FaceRouteV2_EnsureWorker(FaceRouteV2TickFn tick_fn, void* ctx);
/** Overwrite-post a tick; drops stale if worker is busy. */
void FaceRouteV2_PostTick(void);

/**
 * s1cn-c: at most one seed_full / fullscreen present.
 * If afe_already_held=false, takes AfeFetchGate (60ms) for safety window; End releases it.
 * If afe_already_held=true (e.g. idle breathe), only enforces singleton — no nested lock.
 * Returns false → caller must skip the fullscreen present.
 */
bool FaceRouteV2_TryBeginFullscreen(const char* why, bool afe_already_held);
void FaceRouteV2_EndFullscreen(void);
bool FaceRouteV2_FullscreenBusy(void);

/** s1cn-d: true = emit this per-frame log (rate-limited when quiet). */
bool FaceRouteV2_ShouldLogFrame(void);

#ifdef __cplusplus
}
#endif

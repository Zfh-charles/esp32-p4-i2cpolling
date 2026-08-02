#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * s1cl/s1cm: serialize AFE fetch vs idle face present (MSPI-751 skew).
 * Enter/Leave wrap fetch_with_delay; face uses TryLock around present.
 */
void AfeFetchGateEnter(void);
void AfeFetchGateLeave(void);

/** Non-blocking / timed lock for face path. Pair with Unlock. */
bool AfeFetchGateTryLock(uint32_t timeout_ms);
void AfeFetchGateUnlock(void);

/** True if fetch currently holds the gate (timeout 0 probe). */
bool AfeFetchGateBusy(void);

/**
 * Fence an LVGL render/flush while the caller still owns AfeFetchGate.
 * Prepare under the LVGL lock before invalidation, unlock LVGL, then wait.
 * This closes the gap where canvas invalidation returned before MIPI finished
 * reading PSRAM and AFE entered WakeNet on the other core.
 */
typedef struct {
    uint32_t flush_started;
    uint32_t flush_finished;
    uint32_t frame_ready;
} afe_display_fence_t;

void AfeFetchGatePrepareDisplayFence(afe_display_fence_t* fence);
void AfeFetchGateNoteDisplayFlush(bool is_start);
/** Called by the LCD transfer-done callback for LVGL's final strip. */
void AfeFetchGateNoteDisplayFrameReady(void);
bool AfeFetchGateWaitDisplayFence(const afe_display_fence_t* fence,
                                  uint32_t timeout_ms,
                                  uint32_t* waited_ms);

/** s1cv: allocation-free RTC breadcrumbs for bare HP-WDT resets. */
typedef enum {
    AFE_FACE_STAGE_IDLE = 0,
    AFE_FACE_STAGE_BEGIN,
    AFE_FACE_STAGE_CANVAS_WRITE,
    AFE_FACE_STAGE_INVALIDATE,
    AFE_FACE_STAGE_UNLOCK,
    AFE_FACE_STAGE_FENCE_WAIT,
    AFE_FACE_STAGE_DONE,
} afe_face_stage_t;

void AfeFetchGateBeginFaceTxn(void);
void AfeFetchGateNoteFaceStage(afe_face_stage_t stage);
void AfeFetchGateBootReportAndReset(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <sdkconfig.h>
#include <stdint.h>

/**
 * Boot phase tracing — serial filter: BootTrace
 *
 * Lines:
 *   BootTrace: FW_MARKER ...
 *   BootTrace: PH | t=1234ms phase=UI_READY ...
 *   BootTrace: SUMMARY | boots=3 last=WAKE_AFE_START ...
 *   BootTrace: CRASH | reason=... last_phase=...
 */

#if CONFIG_USE_REMINDER_POLL && CONFIG_REMINDER_BOOT_TRACE

void BootTraceInit();
void BootTraceMark(const char* phase, const char* detail);
void BootTraceMarkHeap(const char* phase);
void BootTraceDumpSummary(const char* reason);
const char* BootTraceLastPhase();

#else

inline void BootTraceInit() {}
inline void BootTraceMark(const char* phase, const char* detail) {
    (void)phase;
    (void)detail;
}
inline void BootTraceMarkHeap(const char* phase) { (void)phase; }
inline void BootTraceDumpSummary(const char* reason) { (void)reason; }
inline const char* BootTraceLastPhase() { return "-"; }

#endif

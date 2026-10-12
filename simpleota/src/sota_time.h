/**
 * sota_time.h - duration to FreeRTOS tick conversion.
 *
 * pdMS_TO_TICKS() multiplies in 32 bits, so long durations wrap: past
 * about 71 minutes at a 1000 Hz tick rate, or about 11.9 hours at 100 Hz.
 * This helper multiplies in 64 bits and saturates instead. Every delay in
 * the component goes through it.
 *
 * No ESP-IDF types, so the host tests can include this header.
 */

#ifndef SOTA_TIME_H
#define SOTA_TIME_H

#include <stdint.h>

/** The longest finite wait. UINT32_MAX is portMAX_DELAY, which never ends. */
#define SOTA_MAX_WAIT_TICKS (UINT32_MAX - 1u)

/** Ticks for a duration in milliseconds, saturated to SOTA_MAX_WAIT_TICKS. */
static inline uint32_t sota_ms_to_ticks(uint64_t ms, uint32_t tick_hz)
{
    const uint64_t ticks = ms * (uint64_t)tick_hz / 1000u;
    return ticks > SOTA_MAX_WAIT_TICKS ? SOTA_MAX_WAIT_TICKS : (uint32_t)ticks;
}

#endif /* SOTA_TIME_H */

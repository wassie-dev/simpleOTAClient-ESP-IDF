/**
 * sota_state.h - everything a client persists, and the order it is written.
 *
 * Keys in the store (sota_kv.h). Names and meanings are the ones every
 * earlier release of both clients reads and writes:
 *
 *   sota_build      u32   build number of the live (reported) image
 *   sota_hash       str   sha256 hex of the live image
 *   sota_ver        str   version label of the live image
 *   sota_trial      u8    0 steady, 1 trial, 3 trial that has booted,
 *                         2 rolled back with a report pending
 *   sota_staged     u8    present while the trial record is written but the
 *                         boot slot has not switched yet (sota_trial.h)
 *   sota_new_part   u32   flash address the update went to
 *   sota_prev_part  u32   flash address the update replaced
 *   sota_prev_build u32   \
 *   sota_prev_hash  str    > identity before the update, for a rollback
 *   sota_prev_ver   str   /
 *   sota_fail_dep   str   deployment the confirmed / rolled_back report names
 *   sota_fail_build u32   build that report names
 *   sota_conf_pend  u8    1 until the record's confirmed report is queued
 *   sota_rb_reason  str   why sota_trial is 2; missing means confirm_timeout
 *   sota_outbox     blob  sota_outbox_t: reports waiting to be sent
 *
 * The functions return false when a write did not reach storage. None of
 * them logs: what to do about a failure is the caller's call.
 */

#ifndef SOTA_STATE_H
#define SOTA_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "sota_report.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest version label stored. Labels are short ("1.4.2"); a longer one
 *  is stored as empty rather than spending flash on it. */
#define SOTA_VERSION_MAX 64u
/** sha256 in hex. */
#define SOTA_HASH_HEX_LEN 64u

/** What the device reports itself as running. Zero and empty are valid: the
 *  state before the first update. */
typedef struct {
    uint32_t build;
    char hash[SOTA_HASH_HEX_LEN + 1];
    char version[SOTA_VERSION_MAX + 1];
} sota_identity_t;

/** Everything persisted except the outbox, as loaded at boot. */
typedef struct {
    sota_identity_t live;
    uint8_t trial;
    uint8_t staged;
    uint32_t new_part;
    uint32_t prev_part;
    sota_identity_t prev;
    char deployment[64];   /**< sota_fail_dep; the size of sota_report_t's */
    uint32_t report_build; /**< sota_fail_build */
    uint8_t conf_pend;
    char rb_reason[24];
} sota_state_t;

/** Load the state. A missing key, or a string too long for its field,
 *  loads as zero or empty. */
void sota_state_load(sota_state_t *st);

/**
 * Write the trial record before the boot slot switches, so a bootable new
 * image always has one: sota_staged first, then both partitions and the
 * previous identity, then the new identity and report context, and
 * sota_trial last. On a failure the record is dropped again, and the
 * previous identity put back once it had been saved in full.
 *
 * prev is the identity being replaced; next the update's. Returns false
 * when the record could not be written; the slot must not switch then.
 */
bool sota_state_stage_trial(const sota_identity_t *prev, const sota_identity_t *next,
                            uint32_t new_part, uint32_t prev_part,
                            const char *deployment);

/** The boot slot now points at the new image: remove sota_staged. */
bool sota_state_unstage(void);

/**
 * An update applied without a trial (rollback disabled, or no way to roll
 * back): the new identity goes live, and the next boot queues its
 * confirmed report.
 */
bool sota_state_accept_update(const sota_identity_t *next, const char *deployment);

/** Write the live identity. */
bool sota_state_set_identity(const sota_identity_t *id);

/**
 * The trial is being rolled back: the previous identity goes live, and
 * sota_trial becomes 2 with the reason, for the next boot to report.
 * Restores unconditionally: zero and empty are valid fresh-device values,
 * and skipping them would leave a rolled-back device reporting the new
 * build, which the server would then never offer again.
 */
bool sota_state_mark_rolled_back(const sota_state_t *st, const char *reason);

/** The new image has booted: its next reset ends the trial. */
bool sota_state_mark_booted(void);

/**
 * The trial passed: sota_trial goes, and what is left reads as an update
 * applied without a trial, whose confirmed report the next boot queues if
 * this one cannot.
 */
bool sota_state_end_trial(void);

/** Remove the trial record and its report context. */
bool sota_state_drop_trial(void);

/** Load the outbox; an empty one when none is stored or it is not valid. */
void sota_state_load_outbox(sota_outbox_t *box);

/** Store the outbox, or remove it when empty. */
bool sota_state_save_outbox(const sota_outbox_t *box);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_STATE_H */

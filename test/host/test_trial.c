/**
 * Host checks for boot classification and the report outbox.
 *
 *   cc -std=c99 -Wall -Wextra -Werror -I../../simpleota/src -o test_trial test_trial.c && ./test_trial
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "sota_report.h"
#include "sota_trial.h"

static int g_failures = 0;

static void check(int cond, const char *msg)
{
    printf("%s  %s\n", cond ? "  ok" : "FAIL", msg);
    if (!cond) g_failures++;
}

#define A 0x10000u  /* previous image */
#define B 0x150000u /* new image */

static sota_boot_outcome_t classify(uint8_t trial, uint8_t staged, uint32_t new_part,
                                    uint32_t running, uint32_t boot, bool deep_sleep)
{
    sota_boot_facts_t f = {trial, staged, new_part, running, boot, deep_sleep};
    return sota_classify_boot(&f);
}

int main(void)
{
    /* Records written without sota_staged. */
    check(classify(0, false, 0, A, A, false) == SOTA_BOOT_NONE, "no record");
    check(classify(1, false, B, B, B, false) == SOTA_BOOT_TRIAL,
          "record without sota_staged, running the new image: trial");
    check(classify(1, false, B, A, A, false) == SOTA_BOOT_ROLLED_BACK,
          "record without sota_staged, not on the new image: rolled back");
    check(classify(2, false, B, A, A, false) == SOTA_BOOT_REPORT_PENDING,
          "trial 2 from an earlier boot: report pending");
    check(classify(1, false, 0, B, B, false) == SOTA_BOOT_INCOMPLETE,
          "no new partition: cannot be checked");

    /* Power cut at each point of do_apply(). */
    check(classify(0, true, B, A, A, false) == SOTA_BOOT_ABANDONED,
          "cut after staged, before trial: abandoned");
    check(classify(1, true, B, A, A, false) == SOTA_BOOT_ABANDONED,
          "cut before the slot switch: abandoned, not a rollback");
    check(classify(1, true, B, B, B, false) == SOTA_BOOT_TRIAL,
          "cut after the slot switch, before unstaging: trial");

    /* Deferred reboot and deep sleep. */
    check(classify(1, false, B, A, B, true) == SOTA_BOOT_WAITING,
          "deep-sleep wake on the old image, slot switched: waiting");
    check(classify(1, false, B, A, B, false) == SOTA_BOOT_ROLLED_BACK,
          "any other reset not on the new image: rolled back");
    check(classify(1, true, B, A, B, true) == SOTA_BOOT_WAITING,
          "slot switched, sota_staged left behind, deep-sleep wake: waiting");
    check(classify(1, false, B, A, A, true) == SOTA_BOOT_ROLLED_BACK,
          "deep-sleep wake with the slot back on the old image: rolled back");
    check(classify(1, false, B, B, B, true) == SOTA_BOOT_SLEPT_IN_TRIAL,
          "deep-sleep wake on the new image before it was confirmed: roll back");
    check(classify(1, true, B, B, B, true) == SOTA_BOOT_SLEPT_IN_TRIAL,
          "deep-sleep wake on the new image, sota_staged left behind: roll back");
    check(classify(7, false, B, B, B, false) == SOTA_BOOT_TRIAL,
          "any trial value other than 0 and 2 is a trial");
    check(classify(1, false, B, 0, 0, false) == SOTA_BOOT_INCOMPLETE,
          "running partition unknown: cannot be checked");

    /* The snapshot of the previous identity: a power cut at any write before
     * the slot switch must leave the live identity as it was. */
    enum { W_STARTED, W_NEW_PART, W_PREV_PART, W_PREV_BUILD, W_PREV_HASH,
           W_PREV_VER, W_SNAPSHOT, W_FAIL_DEP, W_FAIL_BUILD, W_CONF_PEND,
           W_BUILD, W_HASH, W_VER, W_TRIAL, W_COUNT };
    int keeps_identity = 1;
    for (int cut = 0; cut <= W_COUNT; ++cut) {
        uint8_t staged = 0, trial = 0;
        uint32_t new_part = 0, prev_build = 0, build = 41;
        for (int w = 0; w < cut; ++w) {
            if (w == W_STARTED) staged = SOTA_STAGED_STARTED;
            if (w == W_NEW_PART) new_part = B;
            if (w == W_PREV_BUILD) prev_build = 41;
            if (w == W_SNAPSHOT) staged = SOTA_STAGED_SNAPSHOT;
            if (w == W_BUILD) build = 42;
            if (w == W_TRIAL) trial = 1;
        }
        sota_boot_outcome_t o = classify(trial, staged, new_part, A, A, false);
        if (o == SOTA_BOOT_ABANDONED && sota_abandon_restores_identity(staged)) build = prev_build;
        keeps_identity = keeps_identity &&
                         (cut == 0 ? o == SOTA_BOOT_NONE
                                   : o == SOTA_BOOT_ABANDONED || o == SOTA_BOOT_INCOMPLETE) &&
                         build == 41;
    }
    check(keeps_identity, "a power cut at any write before the slot switch keeps the identity");
    check(!sota_abandon_restores_identity(0) &&
              !sota_abandon_restores_identity(SOTA_STAGED_STARTED) &&
              sota_abandon_restores_identity(SOTA_STAGED_SNAPSHOT),
          "only a full snapshot is restored");

    /* Rollback reasons. */
    check(strcmp(sota_rollback_reason(SOTA_RST_PANIC), "panic") == 0, "panic");
    check(strcmp(sota_rollback_reason(SOTA_RST_WDT), "watchdog") == 0, "chip watchdog");
    check(strcmp(sota_rollback_reason(SOTA_RST_BROWNOUT), "brownout") == 0, "brownout");
    check(strcmp(sota_rollback_reason(SOTA_RST_SW), "reset") == 0, "software reset");
    check(strcmp(sota_rollback_reason(0), "reset") == 0, "unknown reset");
    check(strcmp(sota_rollback_reason(SOTA_RST_INT_WDT), "int_wdt") == 0, "interrupt watchdog");
    check(strcmp(sota_rollback_reason(SOTA_RST_TASK_WDT), "task_wdt") == 0, "task watchdog");
    check(strcmp(sota_rollback_reason(SOTA_RST_POWERON), "power_on") == 0, "power on");
    check(strcmp(sota_rollback_reason(SOTA_RST_DEEPSLEEP), "deep_sleep") == 0, "deep sleep");
    int all_fit = 1;
    for (int rr = -1; rr <= 32; ++rr) {
        sota_outbox_t b;
        sota_outbox_init(&b);
        all_fit = all_fit && sota_outbox_push(&b, "rolled_back", sota_rollback_reason(rr), "d", 1);
    }
    sota_outbox_t timeout_box;
    sota_outbox_init(&timeout_box);
    check(all_fit && sota_outbox_push(&timeout_box, "rolled_back", "confirm_timeout", "d", 1),
          "every rollback reason fits the reason field");

    /* Outbox. */
    sota_outbox_t box;
    sota_outbox_init(&box);
    check(sota_outbox_valid(&box) && box.count == 0, "a new outbox is empty");
    check(sota_outbox_push(&box, "rolled_back", "boot_failed", "dep-a", 7) &&
              box.count == 1, "a report is queued");
    check(!sota_outbox_push(&box, "confirmed", NULL, "", 8) && box.count == 1,
          "a report without a deployment is not queued");
    check(!sota_outbox_push(&box, "confirmed", NULL, "dep-b", 0) && box.count == 1,
          "a report without a build is not queued");
    check(sota_outbox_push(&box, "confirmed", NULL, "dep-b", 8) &&
              sota_outbox_push(&box, "confirmed", NULL, "dep-c", 9) &&
              box.count == 2 && strcmp(box.reports[0].deployment, "dep-b") == 0,
          "a full outbox drops its oldest report");
    sota_outbox_pop(&box);
    check(box.count == 1 && strcmp(box.reports[0].deployment, "dep-c") == 0,
          "pop removes the oldest report");
    sota_outbox_pop(&box);
    sota_outbox_pop(&box);
    check(box.count == 0, "pop on an empty outbox is harmless");
    box.version = 99;
    check(!sota_outbox_valid(&box), "an outbox with another layout is not loaded");

    sota_outbox_init(&box);
    check(!sota_outbox_push(&box, NULL, NULL, "d", 1) &&
              !sota_outbox_push(&box, "", NULL, "d", 1) && box.count == 0,
          "a report without an event is not queued");
    check(sota_outbox_push(&box, "rolled_back", "panic", "d", UINT32_MAX) &&
              strcmp(box.reports[0].event, "rolled_back") == 0 &&
              strcmp(box.reports[0].reason, "panic") == 0 &&
              box.reports[0].build == UINT32_MAX,
          "a queued report keeps its fields");
    sota_outbox_push(&box, "confirmed", NULL, "d", 2);
    check(box.count == SOTA_OUTBOX_SLOTS && sota_outbox_valid(&box),
          "a full outbox is valid");
    box.count = SOTA_OUTBOX_SLOTS + 1;
    check(!sota_outbox_valid(&box), "a count past the slots is not loaded");
    sota_outbox_init(&box);
    box.count = 1;
    check(!sota_outbox_valid(&box), "an empty loaded report is not loaded");
    sota_outbox_push(&box, "confirmed", NULL, "d", 3);
    memset(box.reports[0].event, 'x', sizeof(box.reports[0].event));
    check(!sota_outbox_valid(&box), "a loaded report with an unterminated field is not loaded");
    check(sizeof(sota_report_t) == 104 && sizeof(sota_outbox_t) == 212 &&
              offsetof(sota_outbox_t, reports) == 4 && SOTA_OUTBOX_VERSION == 1u,
          "outbox blob layout, shared with the Arduino client "
          "(change SOTA_OUTBOX_VERSION with it)");

    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("trial and outbox host checks passed\n");
    return 0;
}

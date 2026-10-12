/**
 * Host checks for the download guard and the waiting-for-reboot decision.
 *
 *   cc -std=c99 -Wall -Wextra -Werror -I../../simpleota/src -o test_guard test_guard.c && ./test_guard
 */

#include <stdio.h>
#include <string.h>

#include "sota_guard.h"

static int g_failures = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("  ok  %s\n", msg);
        return;
    }
    printf("  FAIL %s\n", msg);
    g_failures++;
}

int main(void)
{
    check(sota_apply_decision(false, false, 0) == SOTA_APPLY_DOWNLOAD,
          "an unconfirmed image with no trial does not skip");
    check(sota_apply_decision(true, false, 0) == SOTA_APPLY_WAIT_REBOOT,
          "a flashed image waiting for reboot skips");
    check(sota_apply_decision(false, false, 1) == SOTA_APPLY_WAIT_REBOOT,
          "an armed trial that this boot is not running skips");
    check(sota_apply_decision(false, true, 1) == SOTA_APPLY_IN_TRIAL,
          "a supervised trial skips");
    check(sota_apply_decision(true, true, 1) == SOTA_APPLY_IN_TRIAL,
          "a supervised trial wins over the reboot flag");
    check(sota_apply_decision(false, false, 2) == SOTA_APPLY_ROLLBACK_PENDING,
          "an unaccepted rolled_back report skips");
    check(sota_apply_decision(true, false, 2) == SOTA_APPLY_WAIT_REBOOT,
          "a waiting image wins over a pending rollback report");
    check(sota_apply_skip_reason(SOTA_APPLY_DOWNLOAD) == NULL,
          "a download has no skip reason");
    check(strcmp(sota_apply_skip_reason(SOTA_APPLY_WAIT_REBOOT),
                 "waiting_for_reboot") == 0,
          "waiting for reboot names its reason");
    check(strcmp(sota_apply_skip_reason(SOTA_APPLY_IN_TRIAL),
                 "trial_in_progress") == 0,
          "a trial names its reason");
    check(strcmp(sota_apply_skip_reason(SOTA_APPLY_ROLLBACK_PENDING),
                 "rolled_back_pending") == 0,
          "a pending rollback names its reason");

    check(sota_waiting_for_reboot(true, true, SOTA_IMG_NEW),
          "NEW plus the boot partition selects it means waiting");
    check(!sota_waiting_for_reboot(true, true, SOTA_IMG_ABORTED),
          "ABORTED is a rollback");
    check(!sota_waiting_for_reboot(true, true, SOTA_IMG_INVALID),
          "INVALID is a rollback");
    check(!sota_waiting_for_reboot(true, true, SOTA_IMG_PENDING_VERIFY),
          "PENDING_VERIFY has already left NEW");
    check(!sota_waiting_for_reboot(false, true, SOTA_IMG_NEW),
          "the boot partition moved away from the new slot");
    check(sota_waiting_for_reboot(true, false, 0),
          "no state byte and the boot partition selects the new slot");
    check(!sota_waiting_for_reboot(false, false, 0),
          "no state byte and the boot partition moved");

    /* A retried status report is dropped only when sending it again cannot
     * help. */
    check(sota_status_outcome(200, true, true) == SOTA_STATUS_ACCEPTED,
          "2xx accepted=true is accepted");
    check(sota_status_outcome(200, true, false) == SOTA_STATUS_REJECTED,
          "2xx accepted=false is a final rejection");
    check(sota_status_outcome(200, false, false) == SOTA_STATUS_RETRY,
          "2xx without an accepted field is retried");
    check(sota_status_outcome(404, false, false) == SOTA_STATUS_REJECTED,
          "404 is a final rejection");
    check(sota_status_outcome(400, false, false) == SOTA_STATUS_REJECTED,
          "400 is a final rejection");
    check(sota_status_outcome(401, false, false) == SOTA_STATUS_RETRY,
          "401 is retried: the token may be fixed");
    check(sota_status_outcome(403, false, false) == SOTA_STATUS_RETRY,
          "403 is retried: the token may be fixed");
    check(sota_status_outcome(408, false, false) == SOTA_STATUS_RETRY,
          "408 is retried");
    check(sota_status_outcome(429, false, false) == SOTA_STATUS_RETRY,
          "429 is retried");
    check(sota_status_outcome(503, false, false) == SOTA_STATUS_RETRY,
          "5xx is retried");
    check(sota_status_outcome(-1, false, false) == SOTA_STATUS_RETRY,
          "no response is retried");

    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("guard host checks passed\n");
    return 0;
}

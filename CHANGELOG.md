# Changelog

## Unreleased

- Check intervals and the initial delay are converted to FreeRTOS ticks in
  64 bits. Long durations no longer wrap to a much shorter wait.
- With `disable_auto_reboot`, the component does not download another update
  while a flashed one waits for `simpleota_reboot_for_update()`. A second
  download would have overwritten the waiting update.
- `confirmed` and `rolled_back` reports count as delivered on any HTTP
  response; the device does not act on the response body. A report with no
  response is retried three times at 10 s, then at the normal check interval,
  instead of polling every 10 s indefinitely.
- While `is_connected()` returns false, it is polled every second and the check
  runs as soon as it returns true. A check skipped at boot no longer waits a
  whole check interval.
- Calling `simpleota_init()` again in the boot that flashed a deferred update
  keeps the update waiting. It reported a false `rolled_back` and could
  download another build over the waiting one.
- `simpleota_reboot_for_update()` posts its status from a task with the
  component's stack size, after stopping the background task. It posted from
  the caller's task, and the TLS handshake overflowed the default 3584-byte
  main task stack.
- String values in check-in and status requests are JSON-escaped. A release
  version label containing a quote or backslash made every later check-in
  invalid JSON, so the device never got another offer.
- The trial record, including the new build's identity, is saved before the
  boot slot switches, marked by `sota_staged` until the switch happens. A power
  cut during an update no longer leaves the new image running without a trial.
  The record keeps the key layout of 0.1.0, so updates from 0.1.0 and rollbacks
  to it behave as before.
- `confirmed` and `rolled_back` reports go to an outbox in NVS (two slots,
  oldest dropped first) once the boot has worked them out, and the trial keys
  are cleared. A pending report never holds up an update.
- A deep-sleep wake that resumes the old image while a flashed update waits for
  its reboot keeps the update waiting instead of reporting it as rolled back.

## 0.1.0 (released)

Initial release.

- Managed OTA task: periodic check against the SimpleOTA `/api/v1/ota/check/`
  endpoint, streaming download and flash via `esp_ota_*` with on-the-fly
  SHA-256 checksum and Ed25519 signature verification (Monocypher 3.1.3).
- Full lifecycle status reporting (`download_started` through `confirmed` /
  `failed` / `rolled_back`) matching the SimpleOTA device API contract.
- Bootloader-rollback-aware trial installs when
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is set; graceful degradation when
  it is not. Rollback reports carry the detected cause (`confirm_timeout`
  vs `boot_failed`).
- Signed-firmware enforcement with up to two pinned public keys (rotation),
  fail-closed semantics identical to SimpleOTAClient-Arduino v0.4.0.
- TLS via the ESP-IDF certificate bundle by default; `cert_pem` override.

### Pre-release review fixes (before first publication)

- Deferred-reboot mode (`disable_auto_reboot`) no longer re-downloads and
  re-flashes the same build every check interval: the in-RAM state now
  mirrors the armed NVS state, so the next check reports the new build and
  the manual reboot event carries the right deployment context.
- `cert_pem` is now copied at init, matching the documented "all strings
  are copied" contract (was stored by reference: use-after-free hazard).
- The component moved into the `simpleota/` subdirectory so the examples'
  local override resolves from any clone directory name (the component
  manager matches local dependencies by directory basename).
- An overdue confirm-timeout no longer waits out `initial_delay_ms`, and
  the header documents that `simpleota_start()` must follow promptly on
  trial boots.
- The deferred `rolled_back` report is posted independently of `/check/`
  success (Arduino ordering), so a degraded check endpoint cannot delay
  the rollback signal.
- Non-HTTPS URLs are rejected as `insecure_url` before the signed gate
  runs (Arduino ordering).
- `DOWNLOAD_PROGRESS` events are rate-limited to whole-percent steps
  (or 64 KB steps when the size is unknown).
- `labels_json` is documented as reserved (transmitted but not yet
  consumed by the platform).
- Examples include `<inttypes.h>` explicitly; assorted dead code removed.

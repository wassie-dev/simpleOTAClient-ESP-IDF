# Changelog

## 1.0.0

### Added

- Polling mode: `simpleota_check()` and `simpleota_apply()`, with `simpleota_result_t`, for apps that run the checks from their own task, as the Arduino client can.
- `simpleota_set_server_url()` and `simpleota_last_offered_version()`, as on Arduino.
- A table in the README maps each call to its Arduino counterpart.

### Changed

- Anti-rollback: an offer whose build number is not above the running build is refused (`downgrade_refused`; `SIMPLEOTA_DOWNGRADE` in polling mode). An old image's signature still verifies, so this stops an old, vulnerable build being pushed. To go back, release the old code under a new, higher build number.
- An image flashed some other way than SimpleOTA, such as over serial, reports build 0 instead of the build stored for the image before it, so a device reused for another project is not stuck refusing that project's builds. The running image's ELF SHA-256 is stored as `sota_image` to tell them apart. Release firmware can declare its own build in `image_build`, so a USB-flashed release reports it and the server does not replace it.
- `manual_confirm` applies to managed mode only. Without `simpleota_start()`, a trial is confirmed only by `simpleota_confirm()`, as on Arduino.

- The update logic is now the shared SimpleOTAClient-core, copied into
  `simpleota/src/core`, so the protocol, the trial record in NVS and the
  rollback rules are the Arduino client's. The public API is unchanged.
- Without `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a new image now runs as a
  trial and the component rolls it back itself, as the bootloader would: on a
  confirm timeout, or on any reset before confirming (a crash, a watchdog, a
  brownout, a power loss, `esp_restart()` or deep sleep)
  (`CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK`, on by default). Bootloader rollback
  is still recommended: it also catches an image that crashes before
  `simpleota_init()` on every boot.
- A rollback cut short by a power loss is finished on the next boot, instead
  of leaving the failed image running.
- Configuring a project without `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
  prints a CMake warning, and a software-supervised trial logs one warning
  when it starts.
- The trial watchdog runs without bootloader rollback too.
- A trial that boots after a panic or watchdog reset is rolled back.
- A download redirect is followed only to another `https://` URL
  (`redirect_not_https` otherwise), and a `base_url` that is not `https://`
  fails `simpleota_init()` with a log line saying why.
- Without `simpleota_start()`, the confirm timeout now rolls back from a short-lived task. Before, nothing acted on it until the next reset.
- `simpleota_deinit()` from `event_cb` returns `ESP_ERR_INVALID_STATE`, instead of freeing what the running task uses.
- `simpleota_deinit()` returns `ESP_ERR_TIMEOUT` and releases nothing when
  the task does not stop, instead of freeing what a running download uses.
- Tokens of any length work; one over about 180 characters used to fail every
  request.
- `simpleota_confirm()` returns `ESP_FAIL` when the bootloader cannot record
  the image as good.
- The update slot is erased as the download is written, not all at once
  before it, and an image larger than the slot is refused before the
  download.
- Server responses are read from their top-level fields only, and numbers
  too large for 32 bits are refused.
- Labels that are not one JSON object are left out of the check-in with a
  warning, instead of making every check-in invalid.
- About 1 KB less flash and 100 bytes less static RAM on the ESP32.

## 0.2.1

The component's code is unchanged; this release updates the examples, the
documentation and CI.

### Upgrading

- Existing projects and devices are not affected: a device keeps the
  partition table it was flashed with. To move a project to the new table,
  see "Changing the layout" in the README.

### Changed

- The examples use a partition table with two 1700 KB OTA slots and no
  factory partition (`partitions.csv`), the table that
  `CONFIG_PARTITION_TABLE_TWO_OTA_LARGE` selects on ESP-IDF 5.4 and later.
  With the 1 MB slots of `CONFIG_PARTITION_TABLE_TWO_OTA`, the ESP32-C6
  examples left less than 5% of a slot free; with this table they leave more
  than 40%.
- The README recommends this table, explains how to change a device's layout
  safely, and lists the component's flash and RAM footprint.
- The README quick start and `simpleota.h` say to wait for an IP before
  `simpleota_start()`, or to set `.is_connected` until there is one.
- The hardware-testing guide erases the flash before switching partition
  layouts, and describes what `idf.py flash` writes.
- CI fails when an example leaves less than 20% of its app slot free, and
  builds the signed example on the ESP32-C6.

## 0.2.0

### Upgrading

- Updates from 0.1.0 are protected by the new trial handling: the trial record
  keeps the NVS key layout of 0.1.0, and a rollback to 0.1.0 behaves as it did
  before.
- On a trial boot, `simpleota_init()` starts the chip watchdog. Call it as soon
  as NVS is ready, and confirm within the confirm timeout.
- `simpleota_confirm()` can now return an error; check it when you call it.
- Devices that deep-sleep must confirm before the first sleep after an update;
  waking from deep sleep before that rolls back.

### Added

- A hang during a trial is reset by the chip's RTC or LP watchdog, started by
  `simpleota_init()` for the confirm timeout plus a margin, and the bootloader
  returns to the previous build; the report says `watchdog`. Confirming and
  `simpleota_deinit()` stop it, and so does deep sleep. A confirm timeout past
  the watchdog's limit (about 8 h) logs a warning.
  `CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` leaves it out.
- `rolled_back` reports carry the reset that ended the trial (`panic`,
  `watchdog`, `brownout`, and so on) instead of `boot_failed`, or
  `confirm_timeout` when the component's own timer rolled back.
- An unconfirmed trial image that wakes from deep sleep rolls back and reports
  `deep_sleep`. With `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` the
  bootloader resumes the image on a wake, so without this an image that
  sleeps before confirming would get a new confirm timeout on every wake and
  never roll back.

### Changed

- `confirmed` and `rolled_back` reports go to an outbox in NVS (two slots,
  oldest dropped first) once the boot has worked them out, and the trial keys
  are cleared. A pending report never holds up an update.
- Reports count as delivered on any HTTP response; the device does not act on
  the response body. A report with no response is retried three times at 10 s,
  then at the normal check interval, instead of polling every 10 s
  indefinitely.
- `simpleota_confirm()` marks the image valid first and returns the error, with
  the trial still running, if the bootloader cannot record it.
- No update is downloaded while a trial is running, so the previous image stays
  in the other slot.
- During a trial each request is sent once and queued reports wait until the
  trial ends, and the watchdog margin covers one request at the configured
  HTTP timeout. A confirm timeout during a network outage therefore rolls
  back through the component and reports `confirm_timeout`, not `watchdog`.

### Fixed

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
  boot slot switches. A power cut during an update no longer leaves the new
  image running without a trial.
- Check intervals and the initial delay are converted to FreeRTOS ticks in
  64 bits. Long durations no longer wrap to a much shorter wait.
- With `disable_auto_reboot`, the component does not download another update
  while a flashed one waits for `simpleota_reboot_for_update()`. A second
  download would have overwritten the waiting update.
- A deep-sleep wake that resumes the old image while a flashed update waits for
  its reboot keeps the update waiting instead of reporting it as rolled back.

## 0.1.0 (2026-07-24)

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

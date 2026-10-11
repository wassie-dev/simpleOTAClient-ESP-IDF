# Changelog

## 0.2.0

On a SimpleOTA trial boot, `simpleota_init()` arms the chip's RTC or LP
watchdog for the confirm timeout plus a margin (the larger of 60 seconds
and 10 percent), using the calibrated slow clock. A hang that stops both
cores still resets the chip, the bootloader switches back, and the
`rolled_back` report carries reason `watchdog`. Confirm, the component's
own rollback, and deep sleep stop that watchdog. Other boots leave it
alone, except that a build with `CONFIG_BOOTLOADER_WDT_ENABLE` off stops
one left running by a reset. `CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG`
turns the watchdog off while the confirm timer stays in place.

With `disable_auto_reboot`, the component does not download another
update while a flashed one waits for `simpleota_reboot_for_update()`. A
second download wrote into the slot holding the waiting update, and if
it was cut off, the next boot fell back and reported that update as a
`boot_failed` rollback. The component also does not download while the
running image is unconfirmed, which the bootloader would refuse anyway.

`simpleota_confirm()` keeps the trial going when the image cannot be
marked valid, and a timeout rollback whose mark-invalid call fails
restarts so the bootloader can switch back.

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

# Changelog

## 0.2.0

- On a SimpleOTA trial boot, `simpleota_init()` arms the chip's RTC or LP
  watchdog for the confirm timeout plus a margin (the larger of 60 seconds
  and 10 percent), using the calibrated slow clock. A hang that stops the
  confirm timer still resets the chip, the bootloader switches back, and
  the `rolled_back` report carries reason `watchdog`.
- Confirm, the component's own rollback, and deep sleep stop that
  watchdog. Other boots leave it alone, except that a build with
  `CONFIG_BOOTLOADER_WDT_ENABLE` off stops one left running by a reset.
  `CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` turns the watchdog off and
  keeps the confirm timer.
- Call `simpleota_init()` as soon as NVS is ready, because that is when
  the watchdog starts. Bring up the network before `simpleota_start()`,
  or set `is_connected` so the task waits for it. The examples initialize
  SimpleOTA before Wi-Fi.
- With `disable_auto_reboot`, the component does not download another
  update while a flashed one waits for `simpleota_reboot_for_update()`.
  The next update slot is the one just written, so another download would
  erase it. The check-in already reports that flashed build, so the same
  build is not offered again. A supervised trial waits the same way.
- An unconfirmed image this component did not install is left alone for
  the application to confirm. Its offer is not swallowed: `esp_ota_begin`
  rejects `PENDING_VERIFY` and the failure is reported.
- If the trial record cannot be saved after the boot partition is set,
  that new image is marked invalid and the device reboots into the app it
  was already running.
- No update is downloaded while a `rolled_back` report is waiting for the
  server's answer, because a new trial would overwrite its record. The
  report is dropped once the server accepts it or rejects it for good
  (`accepted: false`, or a 4xx other than 401, 403, 408, and 429), so a
  rejected report cannot block updates.
- `simpleota_deinit()`/`simpleota_init()` in the same boot, and a
  deep-sleep wake that resumes the old app, do not treat a flashed image
  as a `boot_failed` rollback when its otadata state is still
  `ESP_OTA_IMG_NEW` and the boot partition is that slot. That image is
  waiting for reboot.
- `simpleota_confirm()` keeps the trial going when the image cannot be
  marked valid, and a timeout rollback whose mark-invalid call fails
  restarts so the bootloader can switch back.

## 0.1.0

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

# SimpleOTA client for ESP-IDF

First-party ESP-IDF component for the [SimpleOTA](https://simpleota.com)
firmware update platform. Polls for updates, streams and flashes offered
firmware with on-the-fly SHA-256 and Ed25519 signature verification, reports
the full update lifecycle to the dashboard, and integrates with the ESP-IDF
bootloader's app-rollback support so a bad build backs itself out.

The sibling of [SimpleOTAClient-Arduino](https://github.com/xanderwasserman/SimpleOTAClient-Arduino):
both clients implement the same device API contract and the same signed
firmware semantics, so mixed fleets behave identically. A device keeps its
identity (Wi-Fi MAC device id) across a migration between the two.

Local OTA (ArduinoOTA, espota, ElegantOTA) is for a board on your desk. SimpleOTA is managed fleet OTA for ESP32 when those devices are in the field, without pulling you into a full IoT platform.

You upload a .bin from PlatformIO, ESP-IDF, Arduino, or CI, and devices check in on their own. Eligible ones get a short-lived pre-signed URL and pull firmware straight from object storage. Rollouts can start at 5% with deterministic bucketing, so the same unit stays in the same cohort. Builds can be Ed25519-signed and verified on the device during download, before the new partition is marked bootable. After reboot there's a trial window: crash or no successful check-in, and the device rolls back.

Signing here protects the delivery path. It is not Secure Boot. They stack if you already use Secure Boot.

[Quickstart (project, client, first update)](https://wiki.simpleota.com/getting-started/?utm_source=idf_registry&utm_medium=readme&utm_campaign=signup_v1)
[Site](https://simpleota.com/?utm_source=idf_registry&utm_medium=readme&utm_campaign=signup_v1)

## Install

Starting a new project? [simpleOTA-starter-esp-idf](https://github.com/wassie-dev/simpleOTA-starter-esp-idf) is a template repo with a working ESP-IDF app and a GitHub Actions release workflow already set up.

```sh
idf.py add-dependency "xanderwasserman/simpleota"
```

## Requirements

- ESP-IDF **v5.1 or newer** (tested through IDF 6.x). Targets: esp32, esp32s2,
  esp32s3, esp32c3, esp32c6, esp32h2.
- A partition table with **two OTA app slots**, and (strongly recommended)
  bootloader rollback. With the `partitions.csv` from
  [Partition table](#partition-table) in your project directory, put this in
  your project's `sdkconfig.defaults`:

  ```
  CONFIG_PARTITION_TABLE_CUSTOM=y
  CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
  CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
  ```

  ESP-IDF reads `sdkconfig.defaults` only when it creates `sdkconfig`: in a
  project that already has one, delete it or set the options with
  `idf.py menuconfig`.

  The partition table and the bootloader settings ship with the **first
  serial flash**; they cannot be changed later over OTA. Without
  `BOOTLOADER_APP_ROLLBACK_ENABLE` the component rolls back on its own
  (see [Rollback](#rollback)), which catches less.

## Quick start

```c
#include "simpleota.h"

void app_main(void) {
    // nvs_flash_init(), then bring up Wi-Fi (or Ethernet) and wait for an
    // IP before simpleota_start(). A check that runs without one fails, and
    // the next one is check_interval_s (an hour by default) later.

    simpleota_config_t cfg = {
        .token = "YOUR_PROJECT_TOKEN",   // Project -> API tokens (device scope)
    };
    ESP_ERROR_CHECK(simpleota_init(&cfg));
    ESP_ERROR_CHECK(simpleota_start());
}
```

That is a complete integration: the component starts a background task that
checks hourly, flashes offered builds, reboots into them as a supervised
trial, and confirms them once the new image completes a successful check-in.
`esp_wifi_connect()` returns before the IP arrives, so if your bring-up does
not wait for one, set the optional `.is_connected` callback to return false
until it does: each check then waits for the link, as
[examples/basic](../examples/basic/main/main.c) does.
See [examples/basic](../examples/basic) for a full project including Wi-Fi
bring-up, and [examples/signed](../examples/signed) for signed firmware.

### Polling mode

To run the checks from your own task instead, skip `simpleota_start()`:

```c
if (simpleota_check()) {
    simpleota_result_t r = simpleota_apply();   // reboots on success
    if (r != SIMPLEOTA_OK) ESP_LOGW("app", "update failed: %d", r);
}
```

In polling mode you confirm a trial yourself with `simpleota_confirm()` once
the new image is healthy, whatever `manual_confirm` says. The modes don't mix:
while the managed task runs, `simpleota_check()` returns false and
`simpleota_apply()` returns `SIMPLEOTA_NO_OFFER`, with a warning.

`simpleota_set_server_url()` moves check-ins to another `https://` server
while the managed task is stopped, and `simpleota_last_offered_version()`
returns the last offer's version label.

## Configuration

All fields of `simpleota_config_t` (strings are copied at init):

| Field | Default | Purpose |
|---|---|---|
| `token` | required | Project API token (device scope) or per-device token |
| `base_url` | `https://simpleota.com` | Platform base URL; must be `https://` |
| `device_id` | Wi-Fi STA MAC (`aa:bb:cc:dd:ee:ff`) | Stable per-unit identifier |
| `chip_family` | `CONFIG_IDF_TARGET` | e.g. `esp32`, `esp32s3`, `esp32c3` |
| `board_id`, `hardware_revision`, `partition_profile`, `channel` | omitted | Compatibility metadata; report what your artifacts constrain |
| `labels_json` | omitted | Reserved: sent verbatim on check-ins, not yet consumed by the platform |
| `security_mode` | omitted | `SIMPLEOTA_SECURITY_SIGNED` to enforce signed firmware |
| `version_label` | last persisted | Display-only version string |
| `nvs_schema_version` | 1 | On-device data schema the firmware expects |
| `check_interval_s` | 3600 | Steady-state poll interval |
| `initial_delay_ms` | 0 | Delay before the first check after `simpleota_start()` |
| `confirm_timeout_s` | 300 | Trial window before automatic rollback |
| `manual_confirm` | false | Managed mode. true: you call `simpleota_confirm()` after health checks. Polling mode always needs the call |
| `disable_auto_reboot` | false | true: you call `simpleota_reboot_for_update()`. No further update is downloaded until then. |
| `signing_keys`, `num_signing_keys` | none | Up to 2 pinned Ed25519 public keys (PEM) |
| `is_connected` | always try | Return false while the link is down. It is polled every second, and the check runs as soon as it returns true |
| `event_cb`, `user_ctx` | none | Lifecycle events (see `simpleota_event_id_t`) |
| `cert_pem` | none | Pin a specific CA instead of the certificate bundle |
| `crt_bundle_attach` | `esp_crt_bundle_attach` | Override the bundle function |

Build-time knobs (menuconfig, "SimpleOTA"): task stack size (12288),
priority (5), HTTP timeout (15 s), HTTP TX buffer (2048 -- must fit the long
pre-signed download URLs), download chunk size (4096), and
`SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` (off), which leaves the trial watchdog out.

## Update lifecycle

Status events reported to the dashboard, in order:

```
download_started -> downloaded -> flashed -> validated -> reboot
    -> (next successful check-in) confirmed | rolled_back
```

Failures report `failed` with a reason token (`checksum_mismatch`,
`signature_invalid`, `download_stalled`, `update_begin_failed`, ...) that the
dashboard shows inline on the device row. Once a boot has worked out what
happened to a trial, its `confirmed` or `rolled_back` report goes to an outbox
in NVS (two slots, oldest dropped first) and never holds up an update. Reports
are kept until the server answers, across reboots: any HTTP response counts as
delivered, and the device does not act on what it says. Without a response the
report is sent again, first after 10 s (three times), then at the normal check
interval.

## Rollback

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a freshly flashed image boots
as PENDING_VERIFY: the **bootloader itself** guarantees one attempt. If the
image crashes or loses power before confirmation, the next boot returns to
the previous app. On top of that, the component runs a confirm timeout
(default 300 s): if the new image runs but never completes a successful
check-in (or, with `.manual_confirm`, is never confirmed), it is marked
invalid and the device reboots into the previous build, reporting
`rolled_back` / `confirm_timeout`.

A hang that also stops the confirm timer is caught by the chip's RTC or LP
watchdog, which runs on the slow clock. `simpleota_init()` starts it on a trial
boot for the confirm timeout plus a margin, and confirming stops it. The margin
is the larger of 10 percent and the time one request can take at
`CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` (70 s at the default), and never less than
60 s. During a trial each request is sent once and queued reports wait, so
when the confirm timeout runs out the component rolls back first, reporting
`confirm_timeout`. When the watchdog fires, the chip resets, the bootloader
returns to the previous build, and the report says `watchdog`. Call `simpleota_init()`
as soon as NVS is ready, since that is when the watchdog starts. At the usual
internal slow clock it can wait at most about 8 hours (about 13 on the S2); a
longer confirm timeout logs a warning and the watchdog fires at its limit. It
stops before deep sleep. Deep sleep before confirming ends the trial: the
device wakes on the previous build and the report says `deep_sleep`, also with
`CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`, where the component rolls
back on the wake. So confirm before sleeping.
With the bootloader watchdog on (the default), other
boots leave the RTC watchdog alone. `CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG`
leaves it out; crashes still roll back.

After any rollback the component restores the previous build number in NVS,
so the dashboard sees the truth and the server can re-offer fixed builds. The
`rolled_back` report carries the detected cause: `confirm_timeout` when the
component's own supervision marked the image invalid. Otherwise the reset that
ended the trial: `panic`, `int_wdt`, `task_wdt`, `watchdog`, `brownout`,
`power_on`, `deep_sleep`, or `reset` (also used when the bootloader refused the
image, for example on an image-verification or Secure Boot rejection). Even
without `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a corrupt image can be refused
by the bootloader's basic image verification and then surfaces as
`rolled_back`.

Without `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a new image still runs as a
trial (`CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK`, on by default). The component
switches the boot slot back itself when the confirm timeout runs out, when the
trial boots after a panic or watchdog reset, or when the device wakes from
deep sleep before confirming. It cannot catch a crash that comes before
`simpleota_init()` runs, so bootloader rollback is still recommended for new
devices. Turn the option off to accept a new image on its first boot.

`simpleota_confirm()` marks the image valid in the bootloader first. If that
fails, it returns `ESP_FAIL` and the trial keeps running, so call it again.
While a trial is running, no further update is downloaded: the other slot holds
the previous image the trial may still return to.

Confirmation is automatic on the first successful check-in. If your
application has its own health checks, set `.manual_confirm = true` and call
`simpleota_confirm()` once they pass.

## Partition table

The examples ship this table as `partitions.csv`:

```
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x6000,
otadata,  data, ota,     ,        0x2000,
phy_init, data, phy,     ,        0x1000,
ota_0,    app,  ota_0,   ,        1700K,
ota_1,    app,  ota_1,   ,        1700K,
```

It needs 4 MB of flash. Each OTA slot holds an app image of up to 1700 KB,
and 540 KB is free after `ota_1` for your own data partitions. There is no
factory partition: a serial flash boots `ota_0`, and updates alternate between
`ota_0` and `ota_1`. On ESP-IDF 5.4 and later,
`CONFIG_PARTITION_TABLE_TWO_OTA_LARGE=y` selects the same table.

ESP-IDF's `CONFIG_PARTITION_TABLE_TWO_OTA` also works. It adds a 1 MB factory
partition, which a serial flash boots and updates never write: the first
update installs into `ota_0`, and a rollback from it returns to the factory
image. Its two OTA slots are 1 MB each, which leaves little room for
application code on some targets (see [Footprint](#footprint)).

### Changing the layout

A device keeps the partition table it was flashed with; the table cannot
change over OTA. A new layout reaches a device only through a serial flash.

- Erase the flash with `idf.py erase-flash` before flashing a board whose
  current firmware uses a different layout, such as an Arduino sketch or an
  older table. `idf.py flash` writes the bootloader, the partition table, the
  OTA data and the app, and leaves the rest of the flash as it is. Without the
  erase, the new NVS partition starts with whatever the old layout left there,
  including the build number the component stored, so the device goes on
  reporting that build while it runs the firmware you flashed. After the erase
  it reports build 0 until its first update.
- Builds for devices already in the field must fit their app slot. An image
  larger than the slot is refused and the device stays on its build: the
  update fails with `update_begin_failed`, or with `update_write_failed` when
  the image size is not known in advance. Only the inactive slot is ever
  written.
- When a fleet has devices with different layouts, set `partition_profile` on
  each device to name its layout, and give each upload the profile it was
  built for. The server offers an upload only to devices that report the same
  profile. An upload without a profile is offered to every device, and so is
  any upload to a device that reports none.

## Footprint

Measured with ESP-IDF 5.3 and size optimization
(`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`), building the `basic` example against
the same project without SimpleOTA:

| Target | Added to an app that already uses HTTPS, NVS and OTA | Added to a Wi-Fi-only app | `basic` example: free in a 1700 KB slot | `basic` example: free in a 1 MB slot |
|---|---|---|---|---|
| ESP32 | 31 KB flash, 1.5 KB RAM | 206 KB flash, 3.0 KB RAM | 48% | 14% |
| ESP32-S3 | 31 KB flash, 1.5 KB RAM | 211 KB flash, 3.0 KB RAM | 48% | 14% |
| ESP32-C6 | 34 KB flash, 1.5 KB RAM | 251 KB flash, 3.0 KB RAM | 42% | 4% |

RAM is static RAM (`.data` and `.bss`).

The component itself (`libsimpleota.a`, including the vendored Ed25519 and
SHA-256 code) is about 26 KB of flash. Most of the flash added to a Wi-Fi-only
app is the HTTPS stack (mbedTLS, `esp_http_client` and the certificate
bundle), which an app that already makes HTTPS requests has anyway.

At run time the component's task uses a stack of
`CONFIG_SIMPLEOTA_TASK_STACK_SIZE` bytes (12288 by default). A check
allocates a 4 KB response buffer and a download a buffer of
`CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK` bytes (4096 by default), and each request
holds the HTTP client's buffers and a TLS session on the heap while it runs.

## Signed firmware

Uploads with `security_mode: "signed"` carry an Ed25519 signature over the
exact binary bytes. Pin your project's public key and the device verifies
every image during download, **before** the partition is marked bootable:

```c
static const char *PUBKEY_PEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "...from the dashboard's Signing keys section...\n"
    "-----END PUBLIC KEY-----\n";

const simpleota_signing_key_t keys[] = {{.key_id = "prod-2026", .pem = PUBKEY_PEM}};
simpleota_config_t cfg = {
    .token = "...",
    .security_mode = SIMPLEOTA_SECURITY_SIGNED,
    .signing_keys = keys,
    .num_signing_keys = 1,
};
```

Behavior matrix (identical to the Arduino client):

| Device state | Offer | Result |
|---|---|---|
| signed mode + key pinned | valid signature | verify during download; flash only on pass |
| signed mode + key pinned | missing/invalid/stripped signature | **rejected before flashing**; `failed` / `signature_invalid`; device stays on current build |
| key pinned (any mode) | signed offer without usable signature | rejected (fail closed) |
| no key pinned | signed offer | applied with a prominent "unverified" warning (keeps basic -> signed migration flowing) |
| any | unsigned offer, device not enforcing | normal checksum-verified update |

An enforcing device (signed mode + pinned key) cannot be silently downgraded:
stripping the signature fields from a response fails closed.

**Key rotation:** pin the old and new key together (`num_signing_keys = 2`),
start signing with the new key in CI, revoke the old key on the server once
the fleet has moved.

**Migration (basic -> signed) for a deployed fleet:** ship one last `basic`
upload whose firmware pins the key and sets
`.security_mode = SIMPLEOTA_SECURITY_SIGNED`; devices then report signed mode
and all subsequent uploads are signed and verified. The server never offers a
signed artifact to a device still reporting basic.

**Scope note:** this verifies the *delivery path* (compromised build server,
leaked CI token, tampered storage, MITM). It is not Secure Boot: someone with
physical access can still reflash over serial. The two compose; if you enable
Secure Boot v2, sign the final (Secure-Boot-signed) binary for SimpleOTA and
note that a Secure Boot rejection surfaces as `update_end_failed`, distinct
from `signature_invalid`.

## TLS

Server and download connections use the ESP-IDF certificate bundle by
default (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`, enabled by default), so there
is no CA to paste or rotate. Pin a specific CA with `.cert_pem` if you
prefer. Everything is HTTPS: a `base_url` that is not `https://` fails
`simpleota_init()`, a plain HTTP download URL is refused (`insecure_url`),
and a download follows up to three redirects, each only to another
`https://` URL (`redirect_not_https` otherwise).

### Task watchdog warnings during the handshake

Verifying a certificate chain is a synchronous, CPU-bound operation inside
mbedTLS. The OTA task cannot yield while it runs, so the priority-0 IDLE
task may be starved for longer than the default 5 second task-watchdog
window, and you see a warning like:

```
E (17533) task_wdt: Task watchdog got triggered. ... IDLE ...
```

This is a warning, not a crash: the check completes normally. It shows up
most often on the classic ESP32 at the default 160 MHz, on the first
handshake after boot. Any TLS client on this hardware can hit it; it is not
specific to this component, and the component cannot yield from inside the
mbedTLS call. Pick whichever fix suits your project:

| Setting | Effect |
|---|---|
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30` | Widen the window, keep the watchdog. What the examples do, and the recommended default. |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` | Roughly halves handshake time on ESP32/ESP32-S3 (not available on targets capped at 160 MHz, such as ESP32-C3). |
| `.cert_pem = <your CA>` | Pinning one CA verifies less than searching the full bundle. |
| `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` | Stops watching that core's idle task. Blunt; prefer the timeout. |

## Examples

- [examples/basic](../examples/basic): managed updates with rollback.
- [examples/signed](../examples/signed): on-device Ed25519 verification.

Both are complete projects (`idf.py menuconfig` for Wi-Fi + token, then
`idf.py flash monitor`). They build against the in-repo component through a
local override that works from any clone directory name (the component lives
in the `simpleota/` subdirectory precisely so the override resolves).

New to ESP-IDF, or testing on real hardware for the first time? See
[docs/hardware-testing.md](../docs/hardware-testing.md) for a full
install-to-first-OTA walkthrough, including how to test each kind of
rollback.

## Testing

The update logic is the shared core in `src/core`, copied from
[SimpleOTAClient-core](https://github.com/wassie-dev/simpleOTAClient-core) at
the commit in `src/core/CORE_VERSION`. Its host tests cover signing, SHA-256,
response parsing, the trial record, rollback, and a power cut at every storage
write. CI checks that `src/core` matches that commit, and builds both examples
across IDF v5.1 through latest on Xtensa and RISC-V targets.

## Arduino and ESP-IDF

The [Arduino](https://github.com/xanderwasserman/SimpleOTAClient-Arduino) and [ESP-IDF](https://github.com/xanderwasserman/SimpleOTAClient-ESP-IDF) clients run the same core, so they have the same features, defaults and behaviour. Only the spelling follows each platform's style.

| What | Arduino | ESP-IDF |
|---|---|---|
| Set up | `SimpleOTAClient ota(token, ...)` and setters | `simpleota_init(&config)` |
| Managed mode | `begin(interval, isConnected, initialDelayMs)`, `end()` | `simpleota_start()`, `simpleota_stop()` |
| Check now | `checkNow()` | `simpleota_check_now()` |
| Polling mode | `check()`, `apply()` returning `OTAResult` | `simpleota_check()`, `simpleota_apply()` returning `simpleota_result_t` |
| Events | `onEvent(callback)` | `config.event_cb` |
| Confirm a trial | `confirm()` | `simpleota_confirm()` |
| Confirm by hand in managed mode | `setManagedAutoConfirm(false)` | `config.manual_confirm = true` |
| Trial state | `isTrialBoot()` | `simpleota_is_trial_boot()` |
| Confirm timeout | `setConfirmTimeout(s)` | `config.confirm_timeout_s` |
| Reboot when the app is ready | `setAutoReboot(false)`, `rebootForUpdate()` | `config.disable_auto_reboot`, `simpleota_reboot_for_update()` |
| Server URL | `setServerUrl(url)` | `config.base_url`, `simpleota_set_server_url(url)` |
| TLS trust | `setCACert(pem)` | `config.cert_pem` |
| Signing keys | `setSigningPublicKey()`, `addSigningPublicKey()` | `config.signing_keys` |
| Build and offer | `currentBuild()`, `lastOfferedVersion()` | `simpleota_current_build()`, `simpleota_last_offered_version()` |
| Logging | `setDebug(true)` | `esp_log_level_set("simpleota", ...)` |
| HTTP timeout | `-DSIMPLEOTA_HTTP_TIMEOUT_MS` | `CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` |

## License

MIT (see [LICENSE](LICENSE)). Ed25519 verification uses a vendored,
unmodified copy of [Monocypher](https://monocypher.org/) 3.1.3, dual-licensed
CC0-1.0 / BSD-2-Clause (see [src/core/LICENSE-vendor.md](src/core/LICENSE-vendor.md)).

/**
 * @file
 * @brief Writing an update into the next app slot. Implemented by
 * the platform port (esp_ota_* on ESP32) and the host tests' fake.
 *
 * One write at a time. Nothing here makes the image bootable: the engine
 * saves the trial record first and then switches the slot (sota_boot.h).
 */

#ifndef SOTA_IMAGE_H
#define SOTA_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start writing the slot after the running one. expected is the image
 * size, 0 when unknown. *slot gets the flash address written to, and
 * *running the running image's.
 */
bool sota_image_begin(size_t expected, uint32_t *slot, uint32_t *running);

/**
 * Whether a download that starts with these len bytes can be an image for
 * this device, so one built for another chip is dropped after its first
 * bytes, not after all of it. The engine asks once, with the first read.
 * True also when len is too short to tell: sota_image_finish() has the
 * last word.
 */
bool sota_image_accepts(const uint8_t *head, size_t len);

/** Write the next bytes, in order. */
bool sota_image_write(const uint8_t *buf, size_t len);

/** Give up the write. */
void sota_image_abort(void);

/** End the write and check the image is one the bootloader can start. The
 *  write is over either way. */
bool sota_image_finish(void);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_IMAGE_H */

/**
 * @file
 * @brief The signed manifest: proof, from the project's own key, of which
 * build number and which project an image belongs to.
 *
 * The image signature covers the firmware bytes and nothing else, and the
 * server assigns the build number after it was made. So a server that has
 * been taken over could offer an old signed image under a higher number,
 * or another project's image when two projects share a key, and the
 * signature would still verify. The manifest closes that: once the server
 * has said which build an upload became, the SimpleOTA CLI signs these
 * bytes with the same key, on the developer's machine,
 *
 *     SimpleOTA-Manifest-v1\n
 *     <project id: the project's UUID, lowercase with hyphens>\n
 *     <signing key id>\n
 *     <build number, decimal>\n
 *     <sha256 of the image, 64 lowercase hex digits>\n
 *
 * and the server hands the signature out with the offer. The device never
 * parses a signed document. It writes the same lines from what it holds
 * itself (its configured project id, the offer's key id, build number and
 * checksum) and checks the signature over them. The download then has to
 * match that checksum, as it always did.
 *
 * Portable and host-tested (test/host/test_signing.c).
 */

#ifndef SOTA_MANIFEST_H
#define SOTA_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sota_signing.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The first line of a version 1 manifest. No firmware image starts with
 *  it (an ESP32 image starts with 0xE9), so neither signature can pass for
 *  the other. */
#define SOTA_MANIFEST_V1_MAGIC "SimpleOTA-Manifest-v1"

/** What to do about an offer's manifest (see sota_manifest_gate()). */
typedef enum {
    SOTA_MANIFEST_SKIP,        /**< Not checked: the image signature decides. */
    SOTA_MANIFEST_VERIFY,      /**< Check it before the download. */
    SOTA_MANIFEST_FAIL_CLOSED, /**< Required, and absent or uncheckable: refuse. */
} sota_manifest_gate_t;

/**
 * Decide from the device's own settings, never from what the offer claims
 * about itself. A device that requires manifests refuses an offer without
 * one, so a server cannot strip it. Any other device checks a manifest it
 * is able to check (a project id is configured and a key pinned), which
 * lets a fleet try its manifests before it starts to require them. That
 * is no protection: a server can strip the manifest from such a device.
 *
 * @param required            The device requires a manifest.
 * @param has_project         A project id is configured.
 * @param num_keys            Pinned public keys.
 * @param offer_has_manifest  The offer carries a usable manifest signature.
 */
static inline sota_manifest_gate_t sota_manifest_gate(bool required, bool has_project,
                                                      uint8_t num_keys,
                                                      bool offer_has_manifest)
{
    bool can_check = has_project && num_keys > 0 && offer_has_manifest;
    if (can_check) return SOTA_MANIFEST_VERIFY;
    return required ? SOTA_MANIFEST_FAIL_CLOSED : SOTA_MANIFEST_SKIP;
}

/** True for a project id or key id that can stand on a manifest line: no
 *  control characters, so no line break. Empty is fine. */
bool sota_manifest_field_ok(const char *s);

/**
 * Feed a version 1 manifest's bytes to a verifier that sota_verifier_begin()
 * has started with the manifest signature. False, with nothing fed, when a
 * field cannot be on a manifest line, the project or key id is empty, or the
 * checksum is not 64 lowercase hex digits; the caller still ends the
 * verifier.
 */
bool sota_manifest_feed(sota_sig_verifier_t *v, const char *project_id, const char *key_id,
                        uint32_t build, const char *checksum);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_MANIFEST_H */

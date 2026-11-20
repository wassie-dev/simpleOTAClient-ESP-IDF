/**
 * sota_manifest.c - the bytes a manifest signature covers. See the header.
 */

#include "sota_manifest.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "sota_state.h"

bool sota_manifest_field_ok(const char *s)
{
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20u || c == 0x7fu) return false;
    }
    return true;
}

static bool is_sha256_hex(const char *s)
{
    size_t n = 0;
    for (; s[n]; ++n) {
        bool hex = (s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f');
        if (!hex) return false;
    }
    return n == SOTA_HASH_HEX_LEN;
}

static void feed_line(sota_sig_verifier_t *v, const char *line)
{
    sota_verifier_update(v, (const uint8_t *)line, strlen(line));
    sota_verifier_update(v, (const uint8_t *)"\n", 1);
}

bool sota_manifest_feed(sota_sig_verifier_t *v, const char *project_id, const char *key_id,
                        uint32_t build, const char *checksum)
{
    if (!project_id[0] || !sota_manifest_field_ok(project_id) || !key_id[0] ||
        !sota_manifest_field_ok(key_id) || !is_sha256_hex(checksum)) {
        return false;
    }
    char number[11];
    snprintf(number, sizeof(number), "%" PRIu32, build);
    feed_line(v, SOTA_MANIFEST_V1_MAGIC);
    feed_line(v, project_id);
    feed_line(v, key_id);
    feed_line(v, number);
    feed_line(v, checksum);
    return true;
}

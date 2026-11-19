/* sota_json.c - see header. */

#include "sota_json.h"

#include <string.h>

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Find `"key"` among the top-level object's members and set *val_start to
 * the first non-whitespace character of its value. A key of the same name
 * inside a nested object or array is not a match. Strings are skipped
 * honoring backslash escapes, so braces and quotes inside them do not count. */
static bool json_find_value(const char *s, const char *key, size_t *val_start) {
    size_t klen = strlen(key);
    size_t n = strlen(s);
    int depth = 0;
    for (size_t p = 0; p < n; ++p) {
        char c = s[p];
        if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
        } else if (c == '"') {
            size_t q = p + 1;
            bool named = depth == 1 && q + klen < n && memcmp(s + q, key, klen) == 0 &&
                         s[q + klen] == '"';
            /* Skip to the closing quote. */
            for (p = q; p < n && s[p] != '"'; ++p) {
                if (s[p] == '\\') ++p;
            }
            if (!named) continue;
            size_t v = p + 1;
            while (v < n && is_space(s[v])) ++v;
            if (v >= n || s[v] != ':') continue; /* a value, not a key */
            ++v;
            while (v < n && is_space(s[v])) ++v;
            *val_start = v;
            return true;
        }
    }
    return false;
}

bool sota_json_is_object(const char *s) {
    if (!s) return false;
    while (is_space(*s)) ++s;
    if (*s != '{') return false;
    char open[8];
    int depth = 0;
    for (; *s; ++s) {
        char c = *s;
        if (c == '"') {
            for (++s; *s != '"'; ++s) {
                if ((unsigned char)*s < 0x20u) return false; /* also the end */
                if (*s == '\\' && !*++s) return false;
            }
        } else if (c == '{' || c == '[') {
            if (depth == (int)sizeof(open)) return false;
            open[depth++] = c;
        } else if (c == '}' || c == ']') {
            if (depth == 0 || open[--depth] != (c == '}' ? '{' : '[')) return false;
            if (depth == 0) {
                for (++s; is_space(*s); ++s) {}
                return *s == '\0';
            }
        } else if ((unsigned char)c < 0x20u && !is_space(c)) {
            return false;
        }
    }
    return false;
}

bool sota_json_has_key(const char *json, const char *key) {
    size_t p;
    return json_find_value(json, key, &p);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The four hex digits at s as a number, or -1 for fewer or bad ones. */
static long hex4(const char *s, size_t left) {
    if (left < 4) return -1;
    long v = 0;
    for (int i = 0; i < 4; ++i) {
        int d = hex_value(s[i]);
        if (d < 0) return -1;
        v = (v << 4) | d;
    }
    return v;
}

/* The escape at s, just past its "\u", as UTF-8 into out (4 bytes at most).
 * A high surrogate takes the "\uXXXX" low one after it: servers that escape
 * all non-ASCII send an emoji in a version label that way. Returns the byte
 * count and sets *used to the input read, or returns 0 for bad digits,
 * U+0000, which would end the string early, and a surrogate that is not the
 * first half of a pair in order. */
static size_t decode_u(const char *s, size_t left, char out[4], size_t *used) {
    long hi = hex4(s, left);
    if (hi <= 0 || (hi >= 0xdc00 && hi <= 0xdfff)) return 0;
    unsigned long cp = (unsigned long)hi;
    *used = 4;
    if (hi >= 0xd800 && hi <= 0xdbff) {
        if (left < 6 || s[4] != '\\' || s[5] != 'u') return 0;
        long lo = hex4(s + 6, left - 6);
        if (lo < 0xdc00 || lo > 0xdfff) return 0;
        cp = 0x10000ul + (((unsigned long)hi - 0xd800ul) << 10) + ((unsigned long)lo - 0xdc00ul);
        *used = 10;
    }
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xc0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3fu));
        return 2;
    }
    if (cp < 0x10000ul) {
        out[0] = (char)(0xe0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        out[2] = (char)(0x80u | (cp & 0x3fu));
        return 3;
    }
    out[0] = (char)(0xf0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
    out[3] = (char)(0x80u | (cp & 0x3fu));
    return 4;
}

bool sota_json_get_string(const char *json, const char *key, char *out,
                          size_t out_cap) {
    if (!json || !out || out_cap == 0) return false;
    size_t p;
    if (!json_find_value(json, key, &p)) return false;
    size_t n = strlen(json);
    if (p >= n || json[p] != '"') return false;
    ++p;
    size_t w = 0;
    while (p < n && json[p] != '"') {
        char decoded[4] = {json[p], 0, 0, 0};
        size_t len = 1;
        if (json[p] == '\\') {
            if (p + 1 >= n) return false;
            char esc = json[p + 1];
            p += 2;
            if (esc == 'u') {
                size_t used = 0;
                len = decode_u(json + p, n - p, decoded, &used);
                if (len == 0) return false;
                p += used;
            } else if (esc == 'n') decoded[0] = '\n';
            else if (esc == 't') decoded[0] = '\t';
            else if (esc == 'r') decoded[0] = '\r';
            else if (esc == 'b') decoded[0] = '\b';
            else if (esc == 'f') decoded[0] = '\f';
            else decoded[0] = esc;
        } else {
            ++p;
        }
        if (w + len >= out_cap) return false; /* would truncate: fail loudly */
        memcpy(out + w, decoded, len);
        w += len;
    }
    /* A response cut off inside the value: half a URL is not a URL. */
    if (p >= n) return false;
    out[w] = '\0';
    return true;
}

bool sota_json_get_bool(const char *json, const char *key, bool *out) {
    if (!json || !out) return false;
    size_t p;
    if (!json_find_value(json, key, &p)) return false;
    if (strncmp(json + p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(json + p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool sota_json_get_u32(const char *json, const char *key, uint32_t *out) {
    if (!json || !out) return false;
    size_t p;
    if (!json_find_value(json, key, &p)) return false;
    size_t n = strlen(json);
    uint32_t v = 0;
    bool any = false;
    while (p < n && json[p] >= '0' && json[p] <= '9') {
        uint32_t d = (uint32_t)(json[p] - '0');
        if (v > (UINT32_MAX - d) / 10u) return false; /* would wrap */
        v = v * 10u + d;
        ++p;
        any = true;
    }
    if (!any) return false;
    *out = v;
    return true;
}

bool sota_json_escape(const char *src, char *out, size_t out_cap) {
    if (out_cap == 0) return false;
    size_t o = 0;
    for (const char *p = src ? src : ""; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        char esc = 0;
        if (c == '"' || c == '\\') esc = (char)c;
        else if (c == '\n') esc = 'n';
        else if (c == '\r') esc = 'r';
        else if (c == '\t') esc = 't';
        else if (c < 0x20) continue;
        size_t need = esc ? 2 : 1;
        if (o + need >= out_cap) {
            out[0] = '\0';
            return false;
        }
        if (esc) {
            out[o++] = '\\';
            out[o++] = esc;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return true;
}

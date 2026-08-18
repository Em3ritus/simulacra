#include "surveil_ble_name.h"
#include "threat_sig.h"
#include <stddef.h>

typedef struct { const char *needle; uint8_t len; uint8_t class_id; uint8_t category; } surveil_name_t;

// Advertised-local-name substrings observed on known surveillance gear (case-insensitive). See the
// header for sourcing. Confidence for a match is the caller's call (this file only identifies the
// class/category), matching the pattern used elsewhere for a name-only signal.
static const surveil_name_t NAME_WATCH[] = {
    { "flock", 5, SIG_CLASS_FLOCK, SIG_CAT_CAMERA },
};
#define NAME_WATCH_N (sizeof NAME_WATCH / sizeof NAME_WATCH[0])

static bool ci_contains(const uint8_t *hay, uint8_t hay_len, const char *needle, uint8_t needle_len)
{
    if (needle_len == 0 || hay_len < needle_len) return false;
    for (uint16_t i = 0; i + needle_len <= hay_len; i++) {
        uint8_t j = 0;
        for (; j < needle_len; j++) {
            uint8_t a = hay[i + j], b = (uint8_t)needle[j];
            if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + 32);
            if (a != b) break;
        }
        if (j == needle_len) return true;
    }
    return false;
}

bool surveil_name_match(const uint8_t *name, uint8_t len, uint8_t *class_id, uint8_t *category)
{
    if (!name || len == 0) return false;
    for (size_t i = 0; i < NAME_WATCH_N; i++) {
        if (ci_contains(name, len, NAME_WATCH[i].needle, NAME_WATCH[i].len)) {
            if (class_id) *class_id = NAME_WATCH[i].class_id;
            if (category) *category = NAME_WATCH[i].category;
            return true;
        }
    }
    return false;
}

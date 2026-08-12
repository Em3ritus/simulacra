#include "radar_wire.h"
#include <string.h>
#include "mbedtls/gcm.h"

// nonce = salt(8) || counter(4 BE). Wire v3 rebalanced this from salt(4)||counter(8).
//
// GCM nonce reuse under a shared key is catastrophic — it leaks the XOR of two plaintexts AND the
// GHASH authentication key, turning a confidentiality break into forgery. Uniqueness here rests on
// the salt, which is redrawn every boot: with 4 bytes, a fleet sharing one key had even odds of a
// collision after ~2^16 (device, boot) pairs, which a long-lived fleet reaches. 8 bytes moves that
// to ~2^32 boots. The counter only has to be unique *within* one salt, so 32 bits is ample; senders
// still never reset it across reboots (see the Vigil's NVS reservation) because the CONFIG replay
// floor is monotonic over exactly this value.
static void make_nonce(uint8_t nonce[12], const uint8_t salt[RADAR_SALT_LEN], uint64_t counter)
{
    memcpy(nonce, salt, RADAR_SALT_LEN);
    for (int i = 0; i < 4; i++) nonce[RADAR_SALT_LEN + i] = (uint8_t)(counter >> (8 * (3 - i)));
}

int radar_wire_seal(uint8_t *frame, size_t *frame_len, uint8_t type,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[32], const uint8_t salt[RADAR_SALT_LEN], uint64_t counter)
{
    if (RADAR_HDR_LEN + RADAR_NONCE_LEN + payload_len + RADAR_TAG_LEN > RADAR_FRAME_MAX) return -1;
    frame[0] = RADAR_MAGIC0; frame[1] = RADAR_MAGIC1; frame[2] = RADAR_WIRE_VER; frame[3] = type;
    uint8_t nonce[12]; make_nonce(nonce, salt, counter);
    memcpy(frame + RADAR_HDR_LEN, nonce, RADAR_NONCE_LEN);
    uint8_t *ct  = frame + RADAR_HDR_LEN + RADAR_NONCE_LEN;
    uint8_t *tag = ct + payload_len;

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, payload_len,
                          nonce, RADAR_NONCE_LEN, frame, RADAR_HDR_LEN,   // AAD = header
                          payload, ct, RADAR_TAG_LEN, tag);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;
    *frame_len = RADAR_HDR_LEN + RADAR_NONCE_LEN + payload_len + RADAR_TAG_LEN;
    return 0;
}

int radar_wire_open(const uint8_t *frame, size_t frame_len, const uint8_t key[32],
                    uint8_t *out_type, uint8_t *payload, size_t payload_cap, size_t *payload_len,
                    uint8_t out_salt[RADAR_SALT_LEN], uint64_t *out_counter)
{
    if (frame_len < RADAR_HDR_LEN + RADAR_NONCE_LEN + RADAR_TAG_LEN) return -1;
    if (frame[0] != RADAR_MAGIC0 || frame[1] != RADAR_MAGIC1 || frame[2] != RADAR_WIRE_VER) return -1;
    const uint8_t *nonce = frame + RADAR_HDR_LEN;
    size_t pl = frame_len - RADAR_HDR_LEN - RADAR_NONCE_LEN - RADAR_TAG_LEN;
    // Reject before decrypting: mbedtls writes plaintext into `payload` and only THEN compares the
    // tag (it zeroises on mismatch, but the write already happened), so an oversized frame would
    // overflow the caller's buffer pre-auth, from anyone in RF range. ESP-NOW v2 (IDF >= 5.4)
    // carries up to 1470 B, well past the 250 B v1 cap these buffers are sized for.
    if (pl > payload_cap) return -1;
    const uint8_t *ct  = nonce + RADAR_NONCE_LEN;
    const uint8_t *tag = ct + pl;

    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&g, pl, nonce, RADAR_NONCE_LEN,
                          frame, RADAR_HDR_LEN, tag, RADAR_TAG_LEN, ct, payload);
    mbedtls_gcm_free(&g);
    if (rc != 0) return -2;                             // bad tag / bad key
    *out_type = frame[3]; *payload_len = pl;
    memcpy(out_salt, nonce, RADAR_SALT_LEN);
    uint64_t c = 0; for (int i = 0; i < 4; i++) c = (c << 8) | nonce[RADAR_SALT_LEN + i];
    *out_counter = c;
    return 0;
}

bool radar_replay_monotonic_ok(uint64_t *floor, uint64_t counter)
{
    if (counter <= *floor) return false;
    *floor = counter;
    return true;
}

bool radar_replay_ok(radar_replay_t *st, const uint8_t salt[RADAR_SALT_LEN], uint64_t counter)
{
    if (!st->seen || memcmp(st->salt, salt, RADAR_SALT_LEN) != 0) {   // fresh or peer rebooted
        memcpy(st->salt, salt, RADAR_SALT_LEN); st->counter = counter; st->seen = true; return true;
    }
    if (counter > st->counter) { st->counter = counter; return true; }
    return false;
}

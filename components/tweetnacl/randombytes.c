#include <stddef.h>
#include "esp_random.h"

// TweetNaCl's PRNG hook. This IS live on-device key generation, not just a linker stub:
// fleet_key_init() calls crypto_box_keypair() to mint this node's X25519 enrollment identity,
// and the Vigil calls it again per pairing window. Weakening it weakens enrollment.
//
// (The Ed25519 *control* keypair is still provisioned off-device by tools/gen_ctrl_key.py, and
// signing/verifying are deterministic — that part never draws randomness.)
//
// esp_fill_random is only cryptographically strong while an RF subsystem (Wi-Fi or BT) is
// running; before that it degrades to a PRNG. Both callers satisfy this: fleet_key_init runs
// from esp_now_link_start with the STA already up, and the Vigil opens its pairing window after
// net_init. Keep that ordering — a keypair minted pre-radio would be predictable.
void randombytes(unsigned char *p, unsigned long long n)
{
    esp_fill_random(p, (size_t)n);
}

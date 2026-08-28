#include "slip10.h"
#include <string.h>

// HMAC-SHA512 from the bundled trezor crypto (uBitcoin renames it ubtc_*).
extern "C" void ubtc_hmac_sha512(const uint8_t *key, uint32_t keylen,
                                 const uint8_t *msg, uint32_t msglen, uint8_t *out);

namespace slip10 {

bool deriveEd25519(const uint8_t seed[64], const uint32_t *path, size_t pathLen, uint8_t outPriv[32]) {
  uint8_t I[64];
  // master: I = HMAC-SHA512(key="ed25519 seed", data=seed)
  ubtc_hmac_sha512((const uint8_t *)"ed25519 seed", 12, seed, 64, I);

  uint8_t key[32], chain[32];
  memcpy(key, I, 32);
  memcpy(chain, I + 32, 32);

  for (size_t i = 0; i < pathLen; i++) {
    uint32_t index = path[i] | 0x80000000UL;   // ed25519 = hardened only
    uint8_t data[1 + 32 + 4];
    data[0] = 0x00;
    memcpy(data + 1, key, 32);
    data[33] = (uint8_t)(index >> 24);
    data[34] = (uint8_t)(index >> 16);
    data[35] = (uint8_t)(index >> 8);
    data[36] = (uint8_t)(index);
    ubtc_hmac_sha512(chain, 32, data, sizeof(data), I);
    memcpy(key, I, 32);
    memcpy(chain, I + 32, 32);
    memset(data, 0, sizeof(data));
  }

  memcpy(outPriv, key, 32);
  memset(I, 0, sizeof(I));
  memset(key, 0, sizeof(key));
  memset(chain, 0, sizeof(chain));
  return true;
}

}  // namespace slip10

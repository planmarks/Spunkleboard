#include "store.h"
#include "config.h"
#include "wallet.h"

#include <Preferences.h>
#include <string.h>
#include "mbedtls/gcm.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

namespace {
const uint8_t STORE_VER = 1;
const size_t  SALT_LEN  = 16;
const size_t  IV_LEN    = 12;
const size_t  TAG_LEN   = 16;
const size_t  KEY_LEN   = 32;   // AES-256
const size_t  MAX_ENT   = 32;   // 24-word entropy
const size_t  MAX_PLAIN = 32 + 64;  // entropy + passphrase (see MAX_PASSPHRASE)
const char   *AAD       = "coldwallet/v1";

bool deriveKey(const char *pin, const uint8_t *salt, uint8_t *key) {
  int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256,
      (const unsigned char *)pin, strlen(pin),
      salt, SALT_LEN, PBKDF2_ITERS, KEY_LEN, key);
  return rc == 0;
}
}  // namespace

namespace store {

bool hasSeed() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, true)) return false;   // read-only
  bool has = p.isKey("ct");
  p.end();
  return has;
}

uint8_t triesRemaining() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, true)) return PIN_MAX_TRIES;
  uint8_t t = p.getUChar("tries", 0);
  p.end();
  if (t > PIN_MAX_TRIES) t = PIN_MAX_TRIES;
  return PIN_MAX_TRIES - t;
}

void wipe() {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.clear();
    p.end();
  }
}

bool isTestnet() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, true)) return true;
  uint8_t n = p.getUChar("net", 1);   // default: testnet
  p.end();
  return n != 0;
}

void setTestnet(bool testnet) {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.putUChar("net", testnet ? 1 : 0);
    p.end();
  }
}

bool bleEnabled() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, true)) return false;
  uint8_t b = p.getUChar("ble", 0);   // default: OFF
  p.end();
  return b != 0;
}

void setBleEnabled(bool on) {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.putUChar("ble", on ? 1 : 0);
    p.end();
  }
}

uint8_t coin() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, true)) return COIN_BTC;
  uint8_t c = p.getUChar("coin", COIN_BTC);
  p.end();
  return c;
}

void setCoin(uint8_t c) {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.putUChar("coin", c);
    p.end();
  }
}

bool saveSeed(const uint8_t *entropy, size_t entLen, const char *passphrase, const char *pin) {
  if (!entropy || !pin || entLen == 0 || entLen > MAX_ENT) return false;
  size_t plen = passphrase ? strlen(passphrase) : 0;
  if (plen > MAX_PASSPHRASE) return false;
  const size_t total = entLen + plen;

  uint8_t rnd[SALT_LEN + IV_LEN];
  if (!wallet::generateEntropy(rnd, sizeof(rnd))) return false;

  uint8_t salt[SALT_LEN], iv[IV_LEN], key[KEY_LEN], tag[TAG_LEN];
  uint8_t plain[MAX_PLAIN], ct[MAX_PLAIN];
  memcpy(salt, rnd, SALT_LEN);
  memcpy(iv, rnd + SALT_LEN, IV_LEN);
  mbedtls_platform_zeroize(rnd, sizeof(rnd));

  // Plaintext = entropy || passphrase (both lengths stored separately).
  memcpy(plain, entropy, entLen);
  if (plen) memcpy(plain + entLen, passphrase, plen);

  bool ok = false;
  if (deriveKey(pin, salt, key)) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8) == 0 &&
        mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, total,
                                  iv, IV_LEN,
                                  (const unsigned char *)AAD, strlen(AAD),
                                  plain, ct, TAG_LEN, tag) == 0) {
      Preferences p;
      if (p.begin(NVS_NAMESPACE, false)) {
        p.putUChar("ver", STORE_VER);
        p.putBytes("salt", salt, SALT_LEN);
        p.putBytes("iv", iv, IV_LEN);
        p.putBytes("ct", ct, total);
        p.putBytes("tag", tag, TAG_LEN);
        p.putUChar("elen", (uint8_t)entLen);
        p.putUChar("plen", (uint8_t)plen);
        p.putUChar("tries", 0);
        p.end();
        ok = true;
      }
    }
    mbedtls_gcm_free(&gcm);
  }

  mbedtls_platform_zeroize(key, sizeof(key));
  mbedtls_platform_zeroize(plain, sizeof(plain));
  mbedtls_platform_zeroize(ct, sizeof(ct));
  mbedtls_platform_zeroize(tag, sizeof(tag));
  return ok;
}

LoadResult loadSeed(const char *pin, uint8_t *entropy, size_t *outLen,
                    char *passphrase, size_t passMax, uint8_t *triesLeft) {
  if (!pin || !entropy || !outLen) return LOAD_ERROR;
  if (passphrase && passMax) passphrase[0] = '\0';

  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return LOAD_ERROR;
  if (!p.isKey("ct")) { p.end(); return LOAD_EMPTY; }

  uint8_t tries = p.getUChar("tries", 0);
  if (tries >= PIN_MAX_TRIES) {
    p.end();
    wipe();
    if (triesLeft) *triesLeft = 0;
    return LOAD_WIPED;
  }

  // Persist the incremented counter BEFORE checking, so yanking power during an
  // attempt still burns the try (anti power-cycle brute force).
  const uint8_t attempt = tries + 1;
  p.putUChar("tries", attempt);

  uint8_t salt[SALT_LEN], iv[IV_LEN], tag[TAG_LEN], key[KEY_LEN];
  uint8_t ct[MAX_PLAIN], plain[MAX_PLAIN];
  uint8_t elen    = p.getUChar("elen", 0);
  uint8_t plen    = p.getUChar("plen", 0);   // 0 for wallets made before passphrases
  size_t  total   = (size_t)elen + plen;
  size_t  gotCt   = p.getBytes("ct", ct, sizeof(ct));
  size_t  gotSalt = p.getBytes("salt", salt, SALT_LEN);
  size_t  gotIv   = p.getBytes("iv", iv, IV_LEN);
  size_t  gotTag  = p.getBytes("tag", tag, TAG_LEN);

  LoadResult result = LOAD_ERROR;
  if (elen > 0 && elen <= MAX_ENT && plen <= MAX_PASSPHRASE && total <= MAX_PLAIN &&
      gotCt == total && gotSalt == SALT_LEN && gotIv == IV_LEN && gotTag == TAG_LEN &&
      deriveKey(pin, salt, key)) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8) == 0) {
      int rc = mbedtls_gcm_auth_decrypt(&gcm, total, iv, IV_LEN,
                                        (const unsigned char *)AAD, strlen(AAD),
                                        tag, TAG_LEN, ct, plain);
      if (rc == 0) {
        memcpy(entropy, plain, elen);
        *outLen = elen;
        if (passphrase && passMax > plen) {
          memcpy(passphrase, plain + elen, plen);
          passphrase[plen] = '\0';
        }
        p.putUChar("tries", 0);   // reset on success
        result = LOAD_OK;
      } else {
        if (attempt >= PIN_MAX_TRIES) {
          result = LOAD_WIPED;
        } else {
          result = LOAD_WRONG_PIN;
          if (triesLeft) *triesLeft = PIN_MAX_TRIES - attempt;
        }
      }
    }
    mbedtls_gcm_free(&gcm);
  }

  mbedtls_platform_zeroize(key, sizeof(key));
  mbedtls_platform_zeroize(ct, sizeof(ct));
  mbedtls_platform_zeroize(plain, sizeof(plain));
  p.end();

  if (result == LOAD_WIPED) wipe();
  return result;
}

}  // namespace store

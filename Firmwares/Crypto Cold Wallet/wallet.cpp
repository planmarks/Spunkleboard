#include "wallet.h"

#include <Bitcoin.h>                    // uBitcoin: mnemonicFromEntropy, checkMnemonic
#include <string.h>
#include "esp_random.h"
// bootloader_random.h lacks C++ linkage guards in some core versions, which
// causes an undefined-reference at link time from a C++ sketch — wrap it.
extern "C" {
#include "bootloader_random.h"
}
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/platform_util.h"

namespace wallet {

bool generateEntropy(uint8_t *out, size_t out_len) {
  if (!out || out_len == 0) return false;

  // Turn the hardware RNG into a true TRNG while the radios are off.
  bootloader_random_enable();

  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);

  // ESP-IDF's mbedtls port registers the ESP hardware RNG as the entropy
  // source used by mbedtls_entropy_func, so the DRBG is seeded from the TRNG.
  const char *pers = "coldwallet/seed/v1";
  bool ok = false;
  int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                 (const unsigned char *)pers, strlen(pers));
  if (rc == 0) {
    rc = mbedtls_ctr_drbg_random(&drbg, out, out_len);
    ok = (rc == 0);
  }

  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  bootloader_random_disable();

  if (!ok) {
    mbedtls_platform_zeroize(out, out_len);
  }
  return ok;
}

String generateSeedPhrase(uint8_t numWords) {
  const size_t entLen = (numWords == 24) ? ENTROPY_BYTES_24 : ENTROPY_BYTES_12;
  uint8_t ent[ENTROPY_BYTES_24];   // largest case
  String result;

  if (generateEntropy(ent, entLen)) {
    const char *m = mnemonicFromEntropy(ent, entLen);  // BIP39, deterministic
    if (m && m[0]) {
      result = String(m);          // copy out of uBitcoin's internal buffer
    }
  }

  mbedtls_platform_zeroize(ent, sizeof(ent));  // don't leave entropy in RAM
  return result;
}

bool validateMnemonic(const String &mnemonic) {
  return checkMnemonic(mnemonic);
}

size_t seedPhraseToEntropy(const String &mnemonic, uint8_t *out, size_t outLen) {
  if (!out || outLen == 0) return 0;
  return mnemonicToEntropy(mnemonic, out, outLen);
}

String entropyToSeedPhrase(const uint8_t *entropy, size_t len) {
  String result;
  const char *m = mnemonicFromEntropy(entropy, len);
  if (m && m[0]) result = String(m);
  return result;
}

void zeroize(void *buf, size_t len) {
  mbedtls_platform_zeroize(buf, len);
}

// ---- Receive-address derivation --------------------------------------------
namespace {
HDPrivateKey s_root;
bool         s_rootValid = false;
bool         s_testnet   = false;
}  // namespace

void endReceive() {
  s_root      = HDPrivateKey();
  s_rootValid = false;
  s_testnet   = false;
}

bool beginReceive(const uint8_t *entropy, size_t len, const char *passphrase, bool testnet) {
  endReceive();
  String m = entropyToSeedPhrase(entropy, len);
  if (m.length() == 0) return false;

  s_root    = HDPrivateKey(m, String(passphrase ? passphrase : ""), testnet ? &Testnet : &Mainnet);
  m         = "";
  s_testnet = testnet;

  // Validate end-to-end by deriving index 0.
  s_rootValid = true;
  s_rootValid = (receiveAddress(0, nullptr).length() > 0);
  return s_rootValid;
}

String receiveAddress(uint32_t index, String *outPath) {
  if (!s_rootValid) return String("");
  uint32_t coin = s_testnet ? 1 : 0;
  char path[40];
  snprintf(path, sizeof(path), "m/84h/%luh/0h/0/%lu",
           (unsigned long)coin, (unsigned long)index);
  HDPrivateKey child = s_root.derive(path);
  child.type = P2WPKH;                 // native segwit (bech32) address
  if (outPath) *outPath = String(path);
  return child.address();
}

// uBitcoin/trezor exports the sorted 2048-word BIP39 English list (NULL-terminated).
extern "C" const char *const *mnemonic_wordlist(void);

int wordlistMatch(const char *prefix, int *count) {
  const char *const *wl = mnemonic_wordlist();
  size_t plen = strlen(prefix);
  int first = -1, c = 0;
  for (int i = 0; wl[i] != nullptr; i++) {
    if (strncmp(wl[i], prefix, plen) == 0) {
      if (first < 0) first = i;
      c++;
    }
  }
  if (count) *count = c;
  return first;
}

const char *wordlistAt(int idx) {
  if (idx < 0) return "";
  const char *const *wl = mnemonic_wordlist();
  for (int i = 0; wl[i] != nullptr; i++) {
    if (i == idx) return wl[i];
  }
  return "";
}

bool selfTestBip84(String *got) {
  // Official BIP84 test vectors (public — safe to hardcode/print).
  //   m/84'/0'/0'/0/0 -> bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu
  //   m/84'/0'/0'/0/1 -> bc1qnjg0jd8228aq7egyzacy8cys3knf9xvrerkf9g
  // We derive BOTH from one reused root, mirroring how the Receive screen
  // reuses a cached master key across indices.
  const char *MN =
      "abandon abandon abandon abandon abandon abandon "
      "abandon abandon abandon abandon abandon about";
  HDPrivateKey root(String(MN), String(""), &Mainnet);

  HDPrivateKey c0 = root.derive("m/84h/0h/0h/0/0"); c0.type = P2WPKH;
  String a0 = c0.address();
  HDPrivateKey c1 = root.derive("m/84h/0h/0h/0/1"); c1.type = P2WPKH;
  String a1 = c1.address();

  if (got) *got = a0 + " | " + a1;
  bool ok0 = (a0 == String("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"));
  bool ok1 = (a1 == String("bc1qnjg0jd8228aq7egyzacy8cys3knf9xvrerkf9g"));
  return ok0 && ok1;
}

String accountXpub() {
  if (!s_rootValid) return String("");
  uint32_t coin = s_testnet ? 1 : 0;
  char path[24];
  snprintf(path, sizeof(path), "m/84h/%luh/0h", (unsigned long)coin);
  HDPrivateKey acct = s_root.derive(path);
  acct.type = P2WPKH;
  char buf[160];
  memset(buf, 0, sizeof(buf));
  acct.xpub(buf, sizeof(buf));
  return String(buf);
}

String masterFingerprintHex() {
  if (!s_rootValid) return String("");
  uint8_t fp[4];
  s_root.fingerprint(fp);
  char b[9];
  snprintf(b, sizeof(b), "%02x%02x%02x%02x", fp[0], fp[1], fp[2], fp[3]);
  return String(b);
}

}  // namespace wallet

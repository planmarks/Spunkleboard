#pragma once
//
// Wallet crypto: entropy generation and BIP39 mnemonic handling.
// Kept separate from UI so it can be unit-reasoned about and reused.
//
// SECURITY: nothing in here should ever print secrets to Serial. Entropy and
// mnemonics live only in RAM (and, from Phase 3, encrypted in NVS).

#include <Arduino.h>

namespace wallet {

// 128-bit entropy -> 12-word mnemonic; 256-bit -> 24 words.
static const size_t ENTROPY_BYTES_12 = 16;
static const size_t ENTROPY_BYTES_24 = 32;

// Fill `out` with cryptographically-strong random bytes.
//
// Enables the bootloader RNG entropy source (SAR-ADC noise) so the ESP32-C5
// hardware RNG is a TRUE TRNG even with the radios off — this is REQUIRED for a
// cold wallet, whose radios are disabled. Output is drawn from an mbedTLS
// CTR-DRBG seeded from that TRNG rather than consuming the raw RNG directly.
// Returns true on success.
bool generateEntropy(uint8_t *out, size_t out_len);

// Generate a fresh BIP39 mnemonic (default 12 words). Returns the phrase as a
// String copied out of uBitcoin's internal buffer; empty String on failure.
// The internal entropy buffer is zeroized before returning.
String generateSeedPhrase(uint8_t numWords = 12);

// BIP39 checksum validation.
bool validateMnemonic(const String &mnemonic);

// Convert a mnemonic to its raw BIP39 entropy. Returns bytes written (16 for a
// 12-word phrase, 32 for 24), or 0 on failure.
size_t seedPhraseToEntropy(const String &mnemonic, uint8_t *out, size_t outLen);

// Reconstruct the mnemonic from raw entropy (inverse of the above).
String entropyToSeedPhrase(const uint8_t *entropy, size_t len);

// Best-effort secure wipe of a RAM buffer (won't be optimized away).
void zeroize(void *buf, size_t len);

// ---- Receive-address derivation (BIP84 native segwit) -----------------------
// Deriving from a mnemonic re-runs the expensive BIP39 PBKDF2, so we cache the
// master key: call beginReceive() once, then receiveAddress(i) repeatedly, then
// endReceive() to drop the cached key. Paths are m/84'/coin'/0'/0/i where
// coin=0 (mainnet) or 1 (testnet).
bool   beginReceive(const uint8_t *entropy, size_t len, const char *passphrase, bool testnet);
String receiveAddress(uint32_t index, String *outPath);  // "" on failure
String accountXpub();                                     // account extended pubkey
String masterFingerprintHex();                            // 8 hex chars (BIP32 fingerprint)
void   endReceive();

// Diagnostic: derive the official BIP84 test vector (public mnemonic) and check
// it against the known address. Proves the derivation path + uBitcoin are correct
// independent of the RNG/storage. `got` receives the derived address if non-null.
bool   selfTestBip84(String *got);

// ---- BIP39 wordlist access (for seed restore autocomplete) ------------------
// Returns the index of the first wordlist entry starting with `prefix` (or -1),
// and sets *count to how many entries match.
int         wordlistMatch(const char *prefix, int *count);
const char *wordlistAt(int idx);   // "" if out of range

}  // namespace wallet

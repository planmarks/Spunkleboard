#pragma once
//
// Encrypted seed storage in NVS.
//
// The BIP39 entropy is encrypted with AES-256-GCM. The key is derived from the
// user PIN with PBKDF2-HMAC-SHA256 over a random per-wallet salt, so the PIN is
// never stored and a wrong PIN simply fails the GCM authentication. A persisted
// wrong-attempt counter triggers a self-wipe after PIN_MAX_TRIES.
//
// NOTE: at this phase the ciphertext lives in ordinary (unencrypted) flash.
// Flash encryption + Secure Boot v2 are enabled in Phase 7; until then, at-rest
// protection rests on the PIN-derived key and the wipe counter only.

#include <Arduino.h>

namespace store {

enum LoadResult {
  LOAD_OK,          // decrypted successfully
  LOAD_WRONG_PIN,   // authentication failed; attempts remain
  LOAD_WIPED,       // too many wrong attempts — wallet erased
  LOAD_EMPTY,       // no seed stored
  LOAD_ERROR        // storage/crypto error
};

// Is there an encrypted seed present?
bool hasSeed();

// Max BIP39 passphrase length we store.
static const size_t MAX_PASSPHRASE = 64;

// Encrypt `entropy` (entLen bytes, <=32) plus an optional BIP39 `passphrase`
// (may be "" for none) under `pin` and persist it. Resets the wrong-attempt
// counter. Returns true on success.
bool saveSeed(const uint8_t *entropy, size_t entLen, const char *passphrase, const char *pin);

// Attempt to decrypt with `pin`. On LOAD_OK, writes the entropy into `entropy`
// (buffer >=32) and sets *outLen, and writes the passphrase into `passphrase`
// (buffer >= MAX_PASSPHRASE+1, NUL-terminated). On a wrong PIN, *triesLeft is
// set to the remaining attempts. The attempt counter is incremented in flash
// BEFORE the check, so a power-cycle mid-attempt cannot bypass the limit.
LoadResult loadSeed(const char *pin, uint8_t *entropy, size_t *outLen,
                    char *passphrase, size_t passMax, uint8_t *triesLeft);

// Erase all wallet data.
void wipe();

// Remaining wrong-PIN attempts before wipe.
uint8_t triesRemaining();

// Network selection (non-secret setting). Defaults to testnet. Cleared on wipe.
bool isTestnet();
void setTestnet(bool testnet);

// BLE transport enable (non-secret setting). Defaults to OFF. Cleared on wipe.
bool bleEnabled();
void setBleEnabled(bool on);

// Selected coin (non-secret setting): COIN_BTC (default) or COIN_ETH. Cleared on wipe.
uint8_t coin();
void    setCoin(uint8_t c);

}  // namespace store

#pragma once
//
// PSBT (BIP174) parsing and signing. Encapsulates the uBitcoin PSBT object so
// the rest of the firmware never includes the Bitcoin headers.
//
// Flow: load() a base64 PSBT and precompute a display summary (each output's
// address + amount, change detection, fee); the UI shows it for on-device
// confirmation; sign() signs with the seed and returns the signed base64 PSBT.

#include <Arduino.h>

namespace signer {

// Max outputs we keep for display. Signing still covers all inputs regardless.
static const uint8_t MAX_OUTPUTS = 8;

// Parse `b64` and precompute the summary using the seed (for change detection
// and the address network). Returns true if the PSBT is valid.
bool load(const String &b64, const uint8_t *entropy, size_t entLen,
          const char *passphrase, bool testnet);

uint8_t     storedOutputs();          // number of outputs we captured (<= MAX_OUTPUTS)
uint8_t     totalOutputs();           // actual output count in the tx
bool        truncated();              // totalOutputs() > storedOutputs()
uint64_t    outputAmountSat(uint8_t i);
const char *outputAddress(uint8_t i);
bool        outputIsMine(uint8_t i);  // true = change back to us
uint64_t    feeSat();

// Sign with the seed. Returns the number of inputs signed (<=0 on failure) and
// writes the signed PSBT base64 into `outB64`.
int  sign(const uint8_t *entropy, size_t entLen, const char *passphrase, String &outB64);

void clear();

}  // namespace signer

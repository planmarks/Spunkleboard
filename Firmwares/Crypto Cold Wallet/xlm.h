#pragma once
//
// Stellar (XLM) support: address derivation + transaction signing.
//   Address: ed25519 SLIP-0010, SEP-0005 path m/44'/148'/index', strkey "G...".
//   Signing: ed25519 over SHA-256(signatureBase), where
//            signatureBase = networkId(32) || ENVELOPE_TYPE_TX(4) || txXDR.
//   The device parses a single native Payment op for display; otherwise it
//   blind-signs (showing the tx hash for host comparison).
//
// Interchange line:  XLM|acct|<signatureBaseHex>

#include <Arduino.h>

namespace xlm {

// Stellar public address ("G...") for account `index` (path m/44'/148'/index').
String address(const uint8_t *entropy, size_t entLen, const char *passphrase, uint32_t index);

// ---- Transaction signing ----------------------------------------------------
bool     loadTx(const String &line);   // "XLM|acct|signatureBaseHex"
bool     txHasPayment();               // a native Payment op was recognized
String   txRecipient();                // "G..." recipient (if payment)
String   txAmountStr();                // XLM amount (if payment)
String   txHashHex();                  // SHA-256 tx hash (first bytes) for blind-sign
// Sign the loaded tx; returns the 64-byte signature as hex ("" on failure).
String   signTx(const uint8_t *entropy, size_t entLen, const char *passphrase);
void     clearTx();

}  // namespace xlm

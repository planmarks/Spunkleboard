#pragma once
//
// Solana support: address derivation + transaction signing.
//   Address: ed25519 SLIP-0010, Phantom path m/44'/501'/index'/0', base58.
//   Signing: ed25519 over the serialized (legacy) transaction message. The
//   device parses a SystemProgram transfer to show recipient + amount; anything
//   else falls back to a "blind sign" warning.
//
// Interchange line:  SOL|acct|<messageHex>
//   acct = decimal account index; messageHex = serialized legacy message.

#include <Arduino.h>

namespace sol {

// Base58 Solana address for account `index` (path m/44'/501'/index'/0').
String address(const uint8_t *entropy, size_t entLen, const char *passphrase, uint32_t index);

// ---- Transaction signing ----------------------------------------------------
bool     loadTx(const String &line);   // parse "SOL|acct|messageHex"
bool     txHasTransfer();              // a SystemProgram transfer was recognized
String   txRecipient();                // base58 recipient (if transfer)
String   txAmountStr();                // SOL amount (if transfer)
uint32_t txMsgLen();                   // message length in bytes
// Sign the loaded message; returns the base58 signature ("" on failure).
String   signTx(const uint8_t *entropy, size_t entLen, const char *passphrase);
void     clearTx();

}  // namespace sol

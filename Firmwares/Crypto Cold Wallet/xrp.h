#pragma once
//
// XRP Ledger support.
//   Address: BIP44 m/44'/144'/0'/0/i, secp256k1.
//            AccountID = RIPEMD160(SHA256(compressedPubKey)).
//            Address   = XRP-base58check(0x00 + AccountID).
//            XRP alphabet: rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz
//   Signing: SHA-512-half of (0x53545800 + canonical_tx_bytes), secp256k1 ECDSA.
//            App sends:    XRP|acct|txBodyHex
//            Device sends: XRP-SIG> <DER-signature-hex>
//
#include <Arduino.h>

namespace xrp {

// XRP address for BIP44 account `index`.
String address(const uint8_t *entropy, size_t entLen,
               const char *passphrase, uint32_t index);

// Parse a signing request. Returns true if the line is well-formed.
// line format: "XRP|acct|txBodyHex"
bool loadTx(const String &line);

// Display helpers (valid after loadTx).
String txHashHex();   // first 8 hex chars of the signing digest (for on-screen verification)

// Sign the loaded tx. Returns DER-signature hex, "" on failure.
String signTx(const uint8_t *entropy, size_t entLen, const char *passphrase);

void clearTx();

}  // namespace xrp

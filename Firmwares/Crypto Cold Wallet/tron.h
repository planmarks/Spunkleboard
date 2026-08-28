#pragma once
//
// TRON (TRX) support.
//   Address: BIP44 m/44'/195'/0'/0/i, secp256k1.
//            ethAddr = Keccak256(uncompressedPubKey[1:])[12:] (20 bytes);
//            address = base58check(0x41 || ethAddr)  ->  "T..." (double-SHA256 checksum).
//   Signing: txID = SHA256(raw_data); secp256k1 ECDSA (recoverable). Returns the
//            65-byte r||s||v signature (v = recovery id). Blind-sign: the device
//            shows the txID for on-host verification.
//            App sends:    TRX|acct|rawDataHex
//            Device sends: TRX-SIG> <65-byte r||s||v hex>
//
#include <Arduino.h>

namespace tron {

// TRON (T...) address for BIP44 account `index`.
String address(const uint8_t *entropy, size_t entLen,
               const char *passphrase, uint32_t index);

// Parse a signing request "TRX|acct|rawDataHex". Returns true if well-formed.
bool loadTx(const String &line);

// First 8 hex chars of the txID (SHA256 of raw_data) for on-screen verification.
String txHashHex();

// Sign the loaded tx. Returns 65-byte r||s||v hex, "" on failure.
String signTx(const uint8_t *entropy, size_t entLen, const char *passphrase);

void clearTx();

}  // namespace tron

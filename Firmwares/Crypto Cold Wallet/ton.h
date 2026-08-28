#pragma once
//
// TON (Toncoin) support — wallet contract v4R2 (structured so W5 can be added).
//
//   Derivation: SLIP-0010 ed25519 on m/44'/607'/0'  (Ledger-style path). NOTE:
//               this does NOT match Tonkeeper's native-mnemonic accounts — a
//               BIP39 seed fundamentally can't reproduce TON's native scheme.
//               Verify the address against a wallet/tool using this same path.
//   Address:    hash of the v4R2 StateInit cell {code, data{seqno=0, walletId,
//               pubkey, plugins=0}} -> user-friendly bounceable mainnet address
//               (base64url, "EQ...").
//   Signing:    BLIND — the host provides the 32-byte message hash to sign; the
//               device ed25519-signs it and shows the hash for verification.
//               App sends:    TON|acct|msgHashHex   (msgHash = 32 bytes)
//               Device sends: TON-SIG> <64-byte ed25519 signature hex>
//
// ⚠️ The address (TON cell-representation hashing) is UNVERIFIED on hardware.
//    Confirm it matches a reference before sending funds.
//
#include <Arduino.h>

namespace ton {

// TON (EQ...) address for account `index`.
String address(const uint8_t *entropy, size_t entLen,
               const char *passphrase, uint32_t index);

// Parse "TON|acct|msgHashHex" (32-byte hash). Returns true if well-formed.
bool loadTx(const String &line);

// First 8 hex chars of the message hash to be signed (on-screen verification).
String txHashHex();

// ed25519-sign the loaded hash. Returns 64-byte signature hex, "" on failure.
String signTx(const uint8_t *entropy, size_t entLen, const char *passphrase);

void clearTx();

}  // namespace ton

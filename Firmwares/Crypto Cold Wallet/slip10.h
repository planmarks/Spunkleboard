#pragma once
//
// SLIP-0010 hierarchical key derivation for the ed25519 curve (used by Solana,
// Stellar, and other ed25519 chains). ed25519 supports HARDENED derivation only,
// so every path element is hardened regardless of the value passed in.

#include <Arduino.h>

namespace slip10 {

// Derive an ed25519 private key (32 bytes) from a BIP39 seed (64 bytes) along
// `path` (each element hardened automatically). Returns true on success.
bool deriveEd25519(const uint8_t seed[64], const uint32_t *path, size_t pathLen, uint8_t outPriv[32]);

}  // namespace slip10

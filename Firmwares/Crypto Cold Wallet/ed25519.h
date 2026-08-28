#pragma once
//
// ed25519 over the vendored TweetNaCl (pure C, public domain). Replaces the
// rweather Crypto library, which crashed on the ESP32-C5 (RISC-V) inside its
// ed25519 path. Deterministic: keys derive purely from the supplied 32-byte
// SLIP-0010 seed.

#include <Arduino.h>

namespace ed25519 {

// Derive the 32-byte public key from a 32-byte ed25519 seed/private key.
void publicKey(const uint8_t seed[32], uint8_t pub[32]);

// Detached signature (64 bytes) of `msg` under the seed.
void sign(const uint8_t seed[32], const uint8_t *msg, size_t len, uint8_t sig[64]);

}  // namespace ed25519

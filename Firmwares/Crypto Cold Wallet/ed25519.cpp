#include "ed25519.h"
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "tweetnacl.h"
}

// TweetNaCl requires the application to supply randombytes(). We feed it the
// supplied seed so key generation is fully DETERMINISTIC (no real RNG involved).
namespace {
const uint8_t *s_rndSrc = nullptr;
size_t         s_rndLen = 0, s_rndPos = 0;

void keypairFromSeed(const uint8_t seed[32], uint8_t pk[32], uint8_t sk[64]) {
  s_rndSrc = seed; s_rndLen = 32; s_rndPos = 0;
  crypto_sign_keypair(pk, sk);   // randombytes(sk,32) -> seed; derives pk; sk = seed||pk
  s_rndSrc = nullptr; s_rndLen = 0; s_rndPos = 0;
}
}  // namespace

extern "C" void randombytes(unsigned char *x, unsigned long long n) {
  for (unsigned long long i = 0; i < n; i++)
    x[i] = (s_rndSrc && s_rndPos < s_rndLen) ? s_rndSrc[s_rndPos++] : 0;
}

namespace ed25519 {

void publicKey(const uint8_t seed[32], uint8_t pub[32]) {
  uint8_t sk[64];
  keypairFromSeed(seed, pub, sk);
  memset(sk, 0, sizeof(sk));
}

void sign(const uint8_t seed[32], const uint8_t *msg, size_t len, uint8_t sig[64]) {
  uint8_t pk[32], sk[64];
  keypairFromSeed(seed, pk, sk);
  uint8_t *sm = (uint8_t *)malloc(len + 64);   // TweetNaCl outputs sig||msg
  if (sm) {
    unsigned long long smlen = 0;
    crypto_sign(sm, &smlen, msg, (unsigned long long)len, sk);
    memcpy(sig, sm, 64);
    memset(sm, 0, len + 64);
    free(sm);
  }
  memset(sk, 0, sizeof(sk));
}

}  // namespace ed25519

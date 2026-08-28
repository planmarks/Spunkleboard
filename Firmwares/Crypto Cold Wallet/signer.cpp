#include "signer.h"
#include "wallet.h"

#include <Bitcoin.h>
#include <PSBT.h>

namespace {
PSBT     s_psbt;
bool     s_loaded  = false;
bool     s_testnet = false;
uint8_t  s_stored  = 0;
uint8_t  s_total   = 0;
uint64_t s_fee     = 0;

struct OutInfo {
  char     address[80];
  uint64_t amountSat;
  bool     isMine;
};
OutInfo s_outs[signer::MAX_OUTPUTS];

// Build a root HD key from the seed (+ optional passphrase) for the given
// network. Caller-scoped; the stack copy is dropped on return.
HDPrivateKey rootFromSeed(const uint8_t *entropy, size_t entLen, const char *passphrase, bool testnet) {
  String m = wallet::entropyToSeedPhrase(entropy, entLen);
  HDPrivateKey root(m, String(passphrase ? passphrase : ""), testnet ? &Testnet : &Mainnet);
  m = "";
  return root;
}
}  // namespace

namespace signer {

void clear() {
  s_psbt   = PSBT();
  s_loaded = false;
  s_stored = 0;
  s_total  = 0;
  s_fee    = 0;
}

bool load(const String &b64, const uint8_t *entropy, size_t entLen,
          const char *passphrase, bool testnet) {
  clear();
  size_t r = s_psbt.parseBase64(b64);
  if (r == 0 || !s_psbt.isValid()) { clear(); return false; }

  s_testnet = testnet;
  const Network *net = testnet ? &Testnet : &Mainnet;
  HDPrivateKey root = rootFromSeed(entropy, entLen, passphrase, testnet);

  s_total  = (uint8_t)s_psbt.tx.outputsNumber;
  s_stored = (s_total < MAX_OUTPUTS) ? s_total : MAX_OUTPUTS;
  for (uint8_t i = 0; i < s_stored; i++) {
    s_outs[i].address[0] = '\0';
    s_psbt.tx.txOuts[i].address(s_outs[i].address, sizeof(s_outs[i].address), net);
    s_outs[i].amountSat = s_psbt.tx.txOuts[i].amount;
    s_outs[i].isMine    = s_psbt.isMine(i, root);
  }
  s_fee    = s_psbt.fee();
  s_loaded = true;
  return true;
}

uint8_t     storedOutputs()            { return s_stored; }
uint8_t     totalOutputs()             { return s_total; }
bool        truncated()                { return s_total > s_stored; }
uint64_t    outputAmountSat(uint8_t i) { return (i < s_stored) ? s_outs[i].amountSat : 0; }
const char *outputAddress(uint8_t i)   { return (i < s_stored) ? s_outs[i].address : ""; }
bool        outputIsMine(uint8_t i)    { return (i < s_stored) ? s_outs[i].isMine : false; }
uint64_t    feeSat()                   { return s_fee; }

int sign(const uint8_t *entropy, size_t entLen, const char *passphrase, String &outB64) {
  if (!s_loaded) return -1;
  HDPrivateKey root = rootFromSeed(entropy, entLen, passphrase, s_testnet);
  uint8_t sigs = s_psbt.sign(root);
  outB64 = s_psbt.toBase64();
  return (int)sigs;
}

}  // namespace signer

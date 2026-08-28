#include "sol.h"
#include "wallet.h"
#include "slip10.h"
#include "ed25519.h"

#include <vector>
#include <string.h>

extern "C" void mnemonic_to_seed(const char *mnemonic, const char *passphrase,
                                 uint8_t seed[64], void (*cb)(uint32_t, uint32_t));
extern "C" bool b58enc(char *b58, size_t *b58sz, const void *data, size_t binsz);

namespace {

// --- pending tx state ---
std::vector<uint8_t> s_msg;
uint32_t s_acct        = 0;
bool     s_loaded      = false;
bool     s_hasTransfer = false;
uint8_t  s_recipient[32];
uint64_t s_lamports    = 0;

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Solana compact-u16 (shortvec). Returns bytes consumed (1-3), 0 on error.
size_t shortvec(const uint8_t *p, size_t remaining, uint32_t *out) {
  uint32_t val = 0; int shift = 0; size_t used = 0;
  while (used < remaining && used < 3) {
    uint8_t b = p[used++];
    val |= (uint32_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) { *out = val; return used; }
    shift += 7;
  }
  return 0;
}

// Find a SystemProgram transfer in a legacy message; extract recipient+lamports.
bool parseTransfer(const std::vector<uint8_t> &m, uint8_t recipient[32], uint64_t *lamports) {
  size_t n = m.size();
  if (n < 3 || (m[0] & 0x80)) return false;   // need legacy (not versioned) message
  size_t pos = 3;                             // skip 3-byte header

  uint32_t numAccounts = 0;
  size_t used = shortvec(&m[pos], n - pos, &numAccounts);
  if (!used) return false;
  pos += used;
  size_t accountsStart = pos;
  if ((uint64_t)numAccounts * 32 > n - pos) return false;
  pos += (size_t)numAccounts * 32;

  if (pos + 32 > n) return false;             // recent blockhash
  pos += 32;

  uint32_t numInstr = 0;
  used = shortvec(&m[pos], n - pos, &numInstr);
  if (!used) return false;
  pos += used;

  for (uint32_t ii = 0; ii < numInstr; ii++) {
    if (pos >= n) return false;
    uint8_t progIdx = m[pos++];
    uint32_t numAcc = 0;
    used = shortvec(&m[pos], n - pos, &numAcc);
    if (!used) return false;
    pos += used;
    if (pos + numAcc > n) return false;
    const uint8_t *accIdx = &m[pos];
    pos += numAcc;
    uint32_t dataLen = 0;
    used = shortvec(&m[pos], n - pos, &dataLen);
    if (!used) return false;
    pos += used;
    if (pos + dataLen > n) return false;
    const uint8_t *data = &m[pos];
    pos += dataLen;

    if (progIdx < numAccounts) {
      const uint8_t *prog = &m[accountsStart + (size_t)progIdx * 32];
      bool isSystem = true;
      for (int k = 0; k < 32; k++) if (prog[k] != 0) { isSystem = false; break; }
      // SystemProgram Transfer = instruction index 2 (u32 LE) + u64 lamports LE.
      if (isSystem && dataLen == 12 && data[0] == 2 && data[1] == 0 &&
          data[2] == 0 && data[3] == 0 && numAcc >= 2) {
        uint8_t toIdx = accIdx[1];
        if (toIdx < numAccounts) {
          memcpy(recipient, &m[accountsStart + (size_t)toIdx * 32], 32);
          uint64_t lp = 0;
          for (int k = 0; k < 8; k++) lp |= (uint64_t)data[4 + k] << (8 * k);
          *lamports = lp;
          return true;
        }
      }
    }
  }
  return false;
}

String lamportsToSol(uint64_t lp) {
  char b[32];
  snprintf(b, sizeof(b), "%llu.%09llu",
           (unsigned long long)(lp / 1000000000ULL),
           (unsigned long long)(lp % 1000000000ULL));
  String s(b);
  int e = (int)s.length();
  while (e > 0 && s[e - 1] == '0') e--;
  if (e > 0 && s[e - 1] == '.') e--;
  return s.substring(0, e);
}

String base58(const uint8_t *data, size_t len) {
  char b58[128];
  size_t sz = sizeof(b58);
  if (!b58enc(b58, &sz, data, len)) return String("");
  return String(b58);
}

}  // namespace

namespace sol {

String address(const uint8_t *entropy, size_t entLen, const char *passphrase, uint32_t index) {
  String mn = wallet::entropyToSeedPhrase(entropy, entLen);
  if (mn.length() == 0) return String("");
  uint8_t seed[64];
  mnemonic_to_seed(mn.c_str(), passphrase ? passphrase : "", seed, nullptr);
  mn = "";
  uint32_t path[4] = { 44, 501, index, 0 };
  uint8_t priv[32];
  slip10::deriveEd25519(seed, path, 4, priv);
  memset(seed, 0, sizeof(seed));
  uint8_t pub[32];
  ed25519::publicKey(priv, pub);
  memset(priv, 0, sizeof(priv));
  return base58(pub, 32);
}

void clearTx() {
  s_msg.clear();
  s_loaded = false;
  s_hasTransfer = false;
  s_acct = 0;
  s_lamports = 0;
}

bool loadTx(const String &line) {
  clearTx();
  int p1 = line.indexOf('|');
  if (p1 < 0) return false;
  int p2 = line.indexOf('|', p1 + 1);
  if (p2 < 0) return false;
  if (line.substring(0, p1) != "SOL") return false;
  s_acct = (uint32_t)line.substring(p1 + 1, p2).toInt();

  String hex = line.substring(p2 + 1);
  int L = (int)hex.length();
  if (L < 2 || (L & 1)) return false;
  s_msg.reserve(L / 2);
  for (int i = 0; i < L; i += 2) {
    int hi = hexVal(hex[i]), lo = hexVal(hex[i + 1]);
    if (hi < 0 || lo < 0) { clearTx(); return false; }
    s_msg.push_back((uint8_t)((hi << 4) | lo));
  }
  s_loaded = true;
  s_hasTransfer = parseTransfer(s_msg, s_recipient, &s_lamports);
  return true;
}

bool     txHasTransfer() { return s_hasTransfer; }
String   txRecipient()   { return s_hasTransfer ? base58(s_recipient, 32) : String(""); }
String   txAmountStr()   { return s_hasTransfer ? lamportsToSol(s_lamports) : String(""); }
uint32_t txMsgLen()      { return (uint32_t)s_msg.size(); }

String signTx(const uint8_t *entropy, size_t entLen, const char *passphrase) {
  if (!s_loaded) return String("");
  String mn = wallet::entropyToSeedPhrase(entropy, entLen);
  if (mn.length() == 0) return String("");
  uint8_t seed[64];
  mnemonic_to_seed(mn.c_str(), passphrase ? passphrase : "", seed, nullptr);
  mn = "";
  uint32_t path[4] = { 44, 501, s_acct, 0 };
  uint8_t priv[32];
  slip10::deriveEd25519(seed, path, 4, priv);
  memset(seed, 0, sizeof(seed));

  uint8_t sig[64];
  ed25519::sign(priv, s_msg.data(), s_msg.size(), sig);
  memset(priv, 0, sizeof(priv));
  return base58(sig, 64);
}

}  // namespace sol

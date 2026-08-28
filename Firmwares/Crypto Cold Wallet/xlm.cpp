#include "xlm.h"
#include "wallet.h"
#include "slip10.h"
#include "ed25519.h"

#include <vector>
#include <string.h>

extern "C" void mnemonic_to_seed(const char *mnemonic, const char *passphrase,
                                 uint8_t seed[64], void (*cb)(uint32_t, uint32_t));
extern "C" void sha256_Raw(const uint8_t *data, size_t len, uint8_t digest[32]);

namespace {

// CRC16-XModem — Stellar strkey checksum.
uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int k = 0; k < 8; k++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

String base32(const uint8_t *data, size_t len) {
  static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  String out;
  uint32_t buffer = 0; int bits = 0;
  for (size_t i = 0; i < len; i++) {
    buffer = (buffer << 8) | data[i]; bits += 8;
    while (bits >= 5) { out += A[(buffer >> (bits - 5)) & 0x1f]; bits -= 5; }
  }
  if (bits > 0) out += A[(buffer << (5 - bits)) & 0x1f];
  return out;
}

// strkey "G..." for a 32-byte ed25519 public key.
String strkey(const uint8_t pub[32]) {
  uint8_t payload[35];
  payload[0] = 0x30;                       // ed25519 public key version ('G')
  memcpy(payload + 1, pub, 32);
  uint16_t c = crc16(payload, 33);
  payload[33] = (uint8_t)(c & 0xff);
  payload[34] = (uint8_t)((c >> 8) & 0xff);
  return base32(payload, 35);
}

// --- pending tx state ---
std::vector<uint8_t> s_sigBase;
uint8_t  s_hash[32];
uint32_t s_acct       = 0;
bool     s_loaded     = false;
bool     s_hasPayment = false;
uint8_t  s_dest[32];
int64_t  s_amount     = 0;

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// XDR is big-endian. Bounds-checked readers over the tx body.
struct Reader { const uint8_t *p; size_t n, pos; };
bool need(Reader &r, size_t k) { return r.pos + k <= r.n; }
bool rU32(Reader &r, uint32_t *v) {
  if (!need(r, 4)) return false;
  *v = ((uint32_t)r.p[r.pos] << 24) | ((uint32_t)r.p[r.pos + 1] << 16) |
       ((uint32_t)r.p[r.pos + 2] << 8) | r.p[r.pos + 3];
  r.pos += 4; return true;
}
bool rI64(Reader &r, int64_t *v) {
  if (!need(r, 8)) return false;
  uint64_t u = 0;
  for (int i = 0; i < 8; i++) u = (u << 8) | r.p[r.pos + i];
  r.pos += 8; *v = (int64_t)u; return true;
}
bool skip(Reader &r, size_t k) { if (!need(r, k)) return false; r.pos += k; return true; }

// Parse a single native Payment from the tx XDR (which starts at sigBase+36).
// Handles ed25519 source/dest, PRECOND_NONE/TIME, common memo types.
bool parsePayment(const uint8_t *tx, size_t txlen, uint8_t dest[32], int64_t *amount) {
  Reader r = { tx, txlen, 0 };
  uint32_t t;
  if (!rU32(r, &t) || t != 0) return false;     // sourceAccount: KEY_TYPE_ED25519
  if (!skip(r, 32)) return false;
  if (!skip(r, 4)) return false;                 // fee (u32)
  if (!skip(r, 8)) return false;                 // seqNum (i64)

  if (!rU32(r, &t)) return false;                // preconditions union
  if (t == 0) {}                                 // NONE
  else if (t == 1) { if (!skip(r, 16)) return false; }   // TIME: min+max u64
  else return false;                             // V2 etc. -> blind sign

  if (!rU32(r, &t)) return false;                // memo union
  if (t == 0) {}                                 // NONE
  else if (t == 1) { uint32_t l; if (!rU32(r, &l) || !skip(r, (l + 3) & ~3u)) return false; } // TEXT
  else if (t == 2) { if (!skip(r, 8)) return false; }    // ID (u64)
  else if (t == 3 || t == 4) { if (!skip(r, 32)) return false; } // HASH/RETURN
  else return false;

  uint32_t numOps;
  if (!rU32(r, &numOps) || numOps < 1) return false;

  uint32_t hasSrc;                               // op.sourceAccount (optional)
  if (!rU32(r, &hasSrc)) return false;
  if (hasSrc) { if (!rU32(r, &t) || t != 0 || !skip(r, 32)) return false; }

  if (!rU32(r, &t) || t != 1) return false;      // op body: PAYMENT = 1
  if (!rU32(r, &t) || t != 0) return false;      // destination: KEY_TYPE_ED25519
  if (!need(r, 32)) return false;
  memcpy(dest, r.p + r.pos, 32); r.pos += 32;
  if (!rU32(r, &t) || t != 0) return false;      // asset: ASSET_TYPE_NATIVE = 0
  if (!rI64(r, amount)) return false;
  return true;
}

String stroopsToXlm(int64_t s) {
  char b[32];
  snprintf(b, sizeof(b), "%lld.%07lld",
           (long long)(s / 10000000LL), (long long)(s % 10000000LL));
  String str(b);
  int e = (int)str.length();
  while (e > 0 && str[e - 1] == '0') e--;
  if (e > 0 && str[e - 1] == '.') e--;
  return str.substring(0, e);
}

uint8_t *pubFromPath(const uint8_t *entropy, size_t entLen, const char *passphrase,
                     uint32_t acct, uint8_t priv[32]) {
  String mn = wallet::entropyToSeedPhrase(entropy, entLen);
  if (mn.length() == 0) return nullptr;
  uint8_t seed[64];
  mnemonic_to_seed(mn.c_str(), passphrase ? passphrase : "", seed, nullptr);
  mn = "";
  uint32_t path[3] = { 44, 148, acct };
  slip10::deriveEd25519(seed, path, 3, priv);
  memset(seed, 0, sizeof(seed));
  return priv;
}

}  // namespace

namespace xlm {

String address(const uint8_t *entropy, size_t entLen, const char *passphrase, uint32_t index) {
  uint8_t priv[32];
  if (!pubFromPath(entropy, entLen, passphrase, index, priv)) return String("");
  uint8_t pub[32];
  ed25519::publicKey(priv, pub);
  memset(priv, 0, sizeof(priv));
  return strkey(pub);
}

void clearTx() {
  s_sigBase.clear();
  s_loaded = false;
  s_hasPayment = false;
  s_acct = 0;
  s_amount = 0;
}

bool loadTx(const String &line) {
  clearTx();
  int p1 = line.indexOf('|');
  if (p1 < 0) return false;
  int p2 = line.indexOf('|', p1 + 1);
  if (p2 < 0) return false;
  if (line.substring(0, p1) != "XLM") return false;
  s_acct = (uint32_t)line.substring(p1 + 1, p2).toInt();

  String hex = line.substring(p2 + 1);
  int L = (int)hex.length();
  if (L < 72 || (L & 1)) return false;           // >= 36 bytes (networkId+type)
  s_sigBase.reserve(L / 2);
  for (int i = 0; i < L; i += 2) {
    int hi = hexVal(hex[i]), lo = hexVal(hex[i + 1]);
    if (hi < 0 || lo < 0) { clearTx(); return false; }
    s_sigBase.push_back((uint8_t)((hi << 4) | lo));
  }

  sha256_Raw(s_sigBase.data(), s_sigBase.size(), s_hash);
  // tx XDR begins after networkId(32) + envelopeType(4).
  s_hasPayment = parsePayment(s_sigBase.data() + 36, s_sigBase.size() - 36, s_dest, &s_amount);
  s_loaded = true;
  return true;
}

bool   txHasPayment() { return s_hasPayment; }
String txRecipient()  { return s_hasPayment ? strkey(s_dest) : String(""); }
String txAmountStr()  { return s_hasPayment ? stroopsToXlm(s_amount) : String(""); }

String txHashHex() {
  static const char *H = "0123456789abcdef";
  String out;
  for (int i = 0; i < 8; i++) { out += H[s_hash[i] >> 4]; out += H[s_hash[i] & 0xf]; }
  return out;   // first 8 bytes, for host comparison
}

String signTx(const uint8_t *entropy, size_t entLen, const char *passphrase) {
  if (!s_loaded) return String("");
  uint8_t priv[32];
  if (!pubFromPath(entropy, entLen, passphrase, s_acct, priv)) return String("");
  uint8_t sig[64];
  ed25519::sign(priv, s_hash, 32, sig);          // sign the 32-byte tx hash
  memset(priv, 0, sizeof(priv));
  static const char *H = "0123456789abcdef";
  String out;
  for (int i = 0; i < 64; i++) { out += H[sig[i] >> 4]; out += H[sig[i] & 0xf]; }
  return out;
}

}  // namespace xlm

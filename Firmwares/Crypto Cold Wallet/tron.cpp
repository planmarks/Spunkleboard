#include "tron.h"
#include "wallet.h"

#include <Bitcoin.h>
#include <string.h>

// Trezor primitives compiled into the uBitcoin library (keccak_256 needs
// USE_KECCAK=1 in the trezor options.h — the same patch Ethereum relies on).
extern "C" {
  void sha256_Raw(const uint8_t *data, uint32_t len, uint8_t digest[32]);
  void keccak_256(const uint8_t *data, size_t len, uint8_t *digest);
}

namespace {

static const char HEXCH[]  = "0123456789abcdef";
static const char B58_ALPHA[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";  // standard base58

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int hexToBytes(const String &h, uint8_t *out, size_t maxLen) {
  int start = 0, L = (int)h.length();
  if (L >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) start = 2;
  int nhex = L - start;
  if (nhex <= 0 || (nhex & 1)) return -1;
  int oi = 0, idx = start;
  while (nhex > 0) {
    int hi = hexVal(h[idx++]), lo = hexVal(h[idx++]);
    if (hi < 0 || lo < 0) return -1;
    if ((size_t)oi >= maxLen) return -1;
    out[oi++] = (uint8_t)((hi << 4) | lo);
    nhex -= 2;
  }
  return oi;
}

// Pure base58 encode of `len` bytes (standard alphabet).
String base58(const uint8_t *data, int len) {
  int leadZeros = 0;
  while (leadZeros < len && data[leadZeros] == 0) leadZeros++;

  uint8_t tmp[64];
  if (len > (int)sizeof(tmp)) return String("");
  memcpy(tmp, data, len);

  char digits[96];
  int dlen = 0;
  bool nonzero;
  do {
    uint32_t rem = 0;
    nonzero = false;
    for (int i = 0; i < len; i++) {
      uint32_t cur = rem * 256u + tmp[i];
      tmp[i] = (uint8_t)(cur / 58u);
      rem    = cur % 58u;
      if (tmp[i]) nonzero = true;
    }
    if (dlen < (int)sizeof(digits) - 1) digits[dlen++] = B58_ALPHA[rem];
  } while (nonzero);

  for (int i = 0; i < leadZeros && dlen < (int)sizeof(digits) - 1; i++)
    digits[dlen++] = B58_ALPHA[0];

  for (int i = 0, j = dlen - 1; i < j; i++, j--) { char t = digits[i]; digits[i] = digits[j]; digits[j] = t; }
  digits[dlen] = '\0';
  return String(digits);
}

// base58check: append the double-SHA256 checksum (first 4 bytes) then base58.
String base58Check(const uint8_t *data, int len) {
  uint8_t h1[32], h2[32];
  sha256_Raw(data, (uint32_t)len, h1);
  sha256_Raw(h1, 32, h2);
  uint8_t enc[64];
  if (len + 4 > (int)sizeof(enc)) return String("");
  memcpy(enc, data, len);
  memcpy(enc + len, h2, 4);
  return base58(enc, len + 4);
}

// ---- pending tx state -------------------------------------------------------
static bool     s_loaded = false;
static uint32_t s_acct   = 0;
static uint8_t  s_txid[32];   // SHA256(raw_data)

}  // namespace

namespace tron {

String address(const uint8_t *entropy, size_t entLen,
               const char *passphrase, uint32_t index) {
  String m = wallet::entropyToSeedPhrase(entropy, entLen);
  if (m.length() == 0) return String("");

  HDPrivateKey root(m, String(passphrase ? passphrase : ""), &Mainnet);
  m = "";

  char path[40];
  snprintf(path, sizeof(path), "m/44h/195h/0h/0/%lu", (unsigned long)index);
  HDPrivateKey child = root.derive(path);
  PublicKey pub = child.publicKey();
  pub.compressed = false;

  uint8_t sec[65];
  if (pub.sec(sec, sizeof(sec)) != 65) return String("");

  uint8_t h[32];
  keccak_256(sec + 1, 64, h);          // hash the 64-byte X||Y (drop the 0x04 prefix)

  uint8_t addr[21];
  addr[0] = 0x41;                       // TRON mainnet prefix
  memcpy(addr + 1, h + 12, 20);         // low 20 bytes of the keccak hash
  return base58Check(addr, 21);
}

bool loadTx(const String &line) {
  clearTx();
  // TRX|acct|rawDataHex
  String parts[3]; int np = 0, start = 0;
  for (int i = 0; i <= (int)line.length() && np < 3; i++) {
    if (i == (int)line.length() || line[i] == '|') { parts[np++] = line.substring(start, i); start = i + 1; }
  }
  if (np < 3 || parts[0] != "TRX") return false;
  s_acct = (uint32_t)parts[1].toInt();

  static uint8_t raw[4096];            // raw_data bytes (transient — only hashed)
  int n = hexToBytes(parts[2], raw, sizeof(raw));
  if (n <= 0) return false;
  sha256_Raw(raw, (uint32_t)n, s_txid);  // txID = SHA256(raw_data)
  s_loaded = true;
  return true;
}

String txHashHex() {
  if (!s_loaded) return String("?");
  char buf[9];
  for (int i = 0; i < 4; i++) {
    buf[2 * i]     = HEXCH[s_txid[i] >> 4];
    buf[2 * i + 1] = HEXCH[s_txid[i] & 0x0F];
  }
  buf[8] = '\0';
  return String(buf);
}

String signTx(const uint8_t *entropy, size_t entLen, const char *passphrase) {
  if (!s_loaded) return String("");

  String m = wallet::entropyToSeedPhrase(entropy, entLen);
  if (m.length() == 0) return String("");

  HDPrivateKey root(m, String(passphrase ? passphrase : ""), &Mainnet);
  m = "";

  char path[40];
  snprintf(path, sizeof(path), "m/44h/195h/0h/0/%lu", (unsigned long)s_acct);
  HDPrivateKey child = root.derive(path);

  Signature sig = child.sign(s_txid);       // low-s RFC6979
  uint8_t rs[65]; sig.bin(rs, sizeof(rs));    // r[32] s[32] index(=recovery id v)

  // TRON signatures are 65 bytes: r || s || v.
  String out; out.reserve(160);
  for (int i = 0; i < 65; i++) { out += HEXCH[rs[i] >> 4]; out += HEXCH[rs[i] & 0x0F]; }
  return out;
}

void clearTx() {
  s_loaded = false;
  s_acct   = 0;
  memset(s_txid, 0, sizeof(s_txid));
}

}  // namespace tron

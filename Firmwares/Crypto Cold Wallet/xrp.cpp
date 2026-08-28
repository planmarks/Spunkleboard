#include "xrp.h"
#include "wallet.h"

#include <Bitcoin.h>
#include <string.h>

// Trezor hash primitives compiled into the uBitcoin library.
extern "C" {
  void sha256_Raw(const uint8_t *data, uint32_t len, uint8_t digest[32]);
  void sha512_Raw(const uint8_t *data, uint32_t len, uint8_t digest[64]);
  void ripemd160(const uint8_t *msg, uint32_t msg_len, uint8_t *hash);
}

// ============================================================================
// helpers (file-local)
// ============================================================================
namespace {

static const char HEXCH[]    = "0123456789abcdef";
static const char XRP_ALPHA[] =
    "rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz";

// Parse two hex nibbles.
static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int hexToBytes(const String &h, uint8_t *out, size_t maxLen) {
  int start = 0, L = (int)h.length();
  if (L >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) start = 2;
  int nhex = L - start;
  if (nhex < 0 || (nhex & 1)) return -1;   // XRP tx bytes must be even-length hex
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

// XRP base58check: encode `len` bytes using XRP's custom alphabet.
static String xrpBase58Check(const uint8_t *data, int len) {
  // Count leading zero bytes (each encodes as the first alphabet character 'r').
  int leadZeros = 0;
  while (leadZeros < len && data[leadZeros] == 0) leadZeros++;

  uint8_t tmp[32];  // largest input we'll see is 25 bytes
  if (len > (int)sizeof(tmp)) return String("");
  memcpy(tmp, data, len);

  char digits[40];  // 25 bytes → at most 34 base58 chars
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
    if (dlen < (int)sizeof(digits) - 1) digits[dlen++] = XRP_ALPHA[rem];
  } while (nonzero);

  for (int i = 0; i < leadZeros && dlen < (int)sizeof(digits) - 1; i++)
    digits[dlen++] = XRP_ALPHA[0];

  // Reverse in-place.
  for (int i = 0, j = dlen - 1; i < j; i++, j--) {
    char t = digits[i]; digits[i] = digits[j]; digits[j] = t;
  }
  digits[dlen] = '\0';
  return String(digits);
}

// ---- transaction state ----
static const size_t MAX_XRP_TX = 1024;
static bool    s_loaded = false;
static uint32_t s_acct  = 0;
static uint8_t s_txBody[MAX_XRP_TX];
static size_t  s_txLen  = 0;
static uint8_t s_digest[32];   // SHA-512-half of (SIGN_PREFIX + txBody)

static void computeDigest() {
  // XRP canonical signing prefix: STX\x00 = 0x53 54 58 00
  static const uint8_t PREFIX[4] = {0x53, 0x54, 0x58, 0x00};
  const size_t msgLen = 4 + s_txLen;
  uint8_t *msg = (uint8_t *)malloc(msgLen);
  if (!msg) { memset(s_digest, 0, 32); return; }
  memcpy(msg, PREFIX, 4);
  memcpy(msg + 4, s_txBody, s_txLen);
  uint8_t sha512[64];
  sha512_Raw(msg, (uint32_t)msgLen, sha512);
  memcpy(s_digest, sha512, 32);   // SHA-512 half = first 32 bytes
  free(msg);
}

}  // namespace

// ============================================================================
// public API
// ============================================================================
namespace xrp {

String address(const uint8_t *entropy, size_t entLen,
               const char *passphrase, uint32_t index) {
  String m = wallet::entropyToSeedPhrase(entropy, entLen);
  if (m.length() == 0) return String("");

  HDPrivateKey root(m, String(passphrase ? passphrase : ""), &Mainnet);
  m = "";

  char path[40];
  snprintf(path, sizeof(path), "m/44h/144h/0h/0/%lu", (unsigned long)index);
  HDPrivateKey child = root.derive(path);
  PublicKey pub = child.publicKey();
  pub.compressed = true;

  uint8_t sec[33];
  if (pub.sec(sec, sizeof(sec)) != 33) return String("");

  // AccountID = RIPEMD160(SHA256(compressed_pubkey))
  uint8_t sha[32];
  sha256_Raw(sec, 33, sha);
  uint8_t accountId[20];
  ripemd160(sha, 32, accountId);

  // Payload = version_byte(0x00) + accountId
  uint8_t payload[21];
  payload[0] = 0x00;
  memcpy(payload + 1, accountId, 20);

  // Checksum = SHA256(SHA256(payload))[0:4]
  uint8_t h1[32], h2[32];
  sha256_Raw(payload, 21, h1);
  sha256_Raw(h1,      32, h2);

  // Final encoding input = payload + checksum (25 bytes total)
  uint8_t enc[25];
  memcpy(enc,      payload, 21);
  memcpy(enc + 21, h2,       4);

  return xrpBase58Check(enc, 25);
}

bool loadTx(const String &line) {
  clearTx();
  // Format: XRP|acct|txBodyHex
  String parts[3]; int np = 0, start = 0;
  for (int i = 0; i <= (int)line.length() && np < 3; i++) {
    if (i == (int)line.length() || line[i] == '|') {
      parts[np++] = line.substring(start, i);
      start = i + 1;
    }
  }
  if (np < 3 || parts[0] != "XRP") return false;
  s_acct = (uint32_t)parts[1].toInt();
  int n = hexToBytes(parts[2], s_txBody, MAX_XRP_TX);
  if (n <= 0) return false;
  s_txLen = (size_t)n;
  computeDigest();
  s_loaded = true;
  return true;
}

String txHashHex() {
  if (!s_loaded) return String("?");
  char buf[9];
  for (int i = 0; i < 4; i++) {
    buf[2 * i]     = HEXCH[s_digest[i] >> 4];
    buf[2 * i + 1] = HEXCH[s_digest[i] & 0x0F];
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
  snprintf(path, sizeof(path), "m/44h/144h/0h/0/%lu", (unsigned long)s_acct);
  HDPrivateKey child = root.derive(path);

  Signature sig = child.sign(s_digest);
  uint8_t der[73];
  size_t derLen = sig.der(der, sizeof(der));
  if (derLen == 0) return String("");

  // Return DER signature as lowercase hex.
  String out = "";
  out.reserve(derLen * 2);
  for (size_t i = 0; i < derLen; i++) {
    out += HEXCH[der[i] >> 4];
    out += HEXCH[der[i] & 0x0F];
  }
  return out;
}

void clearTx() {
  s_loaded = false;
  s_acct   = 0;
  s_txLen  = 0;
  memset(s_txBody, 0, sizeof(s_txBody));
  memset(s_digest, 0, sizeof(s_digest));
}

}  // namespace xrp

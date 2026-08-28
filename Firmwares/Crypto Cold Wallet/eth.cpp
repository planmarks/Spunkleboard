#include "eth.h"
#include "wallet.h"

#include <Bitcoin.h>
#include <vector>
#include <string.h>

// Ethereum uses original Keccak-256 (0x01 padding), exposed by trezor as
// keccak_256() (requires USE_KECCAK=1 in the uBitcoin trezor options.h).
extern "C" void keccak_256(const uint8_t *data, size_t len, uint8_t *digest);

// ============================================================================
// low-level helpers
// ============================================================================
namespace {

const char *HEXCH = "0123456789abcdef";

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parse a hex string (optional 0x, odd length tolerated) into `out`. Returns the
// number of bytes, or -1 on error / overflow.
int hexToBytes(const String &h, uint8_t *out, size_t maxLen) {
  int start = 0, L = (int)h.length();
  if (L >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) start = 2;
  int nhex = L - start;
  if (nhex < 0) return -1;
  int oi = 0, idx = start;
  if (nhex & 1) {                              // leading lone nibble
    int v = hexVal(h[idx++]); if (v < 0) return -1;
    if ((size_t)oi >= maxLen) return -1;
    out[oi++] = (uint8_t)v; nhex--;
  }
  while (nhex > 0) {
    int hi = hexVal(h[idx++]), lo = hexVal(h[idx++]);
    if (hi < 0 || lo < 0) return -1;
    if ((size_t)oi >= maxLen) return -1;
    out[oi++] = (uint8_t)((hi << 4) | lo);
    nhex -= 2;
  }
  return oi;
}

// EIP-55 checksummed "0x..." for a 20-byte address. `out` must hold >=43 bytes.
void toEip55(const uint8_t addr[20], char *out) {
  char lower[41];
  for (int i = 0; i < 20; i++) {
    lower[2 * i]     = HEXCH[addr[i] >> 4];
    lower[2 * i + 1] = HEXCH[addr[i] & 0x0F];
  }
  lower[40] = '\0';
  uint8_t h[32];
  keccak_256((const uint8_t *)lower, 40, h);
  out[0] = '0'; out[1] = 'x';
  for (int i = 0; i < 40; i++) {
    char c = lower[i];
    uint8_t nib = (h[i / 2] >> (4 * (1 - (i & 1)))) & 0x0F;
    if (c >= 'a' && c <= 'f' && nib >= 8) c = (char)(c - 'a' + 'A');
    out[2 + i] = c;
  }
  out[42] = '\0';
}

String addr0x(const uint8_t addr[20]) { char b[43]; toEip55(addr, b); return String(b); }

// Big-endian bytes -> decimal string.
String bytesToDecimal(const uint8_t *buf, size_t len) {
  uint8_t t[48];
  if (len > sizeof(t)) len = sizeof(t);
  memcpy(t, buf, len);
  bool nonzero = false;
  for (size_t i = 0; i < len; i++) if (t[i]) { nonzero = true; break; }
  if (!nonzero) return String("0");
  String out = "";
  while (nonzero) {
    int rem = 0;
    for (size_t i = 0; i < len; i++) {
      int cur = (rem << 8) | t[i];
      t[i] = (uint8_t)(cur / 10);
      rem = cur % 10;
    }
    out = String((char)('0' + rem)) + out;
    nonzero = false;
    for (size_t i = 0; i < len; i++) if (t[i]) { nonzero = true; break; }
  }
  return out;
}

// Insert a decimal point `decimals` places from the right, trimming trailing 0s.
String formatUnits(const String &dec, int decimals) {
  if (decimals <= 0) return dec;
  String d = dec;
  while ((int)d.length() <= decimals) d = "0" + d;
  int pt = (int)d.length() - decimals;
  String ip = d.substring(0, pt), fp = d.substring(pt);
  int e = (int)fp.length();
  while (e > 0 && fp[e - 1] == '0') e--;
  fp = fp.substring(0, e);
  return fp.length() ? ip + "." + fp : ip;
}

// big-endian buf * m (m fits in 32 bits) -> out (big-endian). Returns out len.
int bnMulU32(const uint8_t *buf, int len, uint32_t m, uint8_t *out, int outMax) {
  uint8_t tmp[48]; int tn = 0;
  uint64_t carry = 0;
  for (int i = len - 1; i >= 0; i--) {
    uint64_t cur = (uint64_t)buf[i] * m + carry;
    if (tn >= (int)sizeof(tmp)) return -1;
    tmp[tn++] = (uint8_t)(cur & 0xff);
    carry = cur >> 8;
  }
  while (carry) { if (tn >= (int)sizeof(tmp)) return -1; tmp[tn++] = (uint8_t)(carry & 0xff); carry >>= 8; }
  if (tn > outMax) return -1;
  for (int i = 0; i < tn; i++) out[i] = tmp[tn - 1 - i];   // reverse to big-endian
  return tn;
}

// ---- RLP ----
void rlpStr(std::vector<uint8_t> &out, const uint8_t *data, size_t len) {
  if (len == 1 && data[0] < 0x80) { out.push_back(data[0]); return; }
  if (len <= 55) { out.push_back((uint8_t)(0x80 + len)); out.insert(out.end(), data, data + len); return; }
  uint8_t lb[8]; int nl = 0; size_t l = len;
  while (l) { lb[nl++] = (uint8_t)(l & 0xff); l >>= 8; }
  out.push_back((uint8_t)(0xb7 + nl));
  for (int i = nl - 1; i >= 0; i--) out.push_back(lb[i]);
  out.insert(out.end(), data, data + len);
}

// RLP for an integer: strip leading zero bytes first (0 -> empty -> 0x80).
void rlpInt(std::vector<uint8_t> &out, const uint8_t *data, size_t len) {
  size_t i = 0;
  while (i < len && data[i] == 0) i++;
  rlpStr(out, data + i, len - i);
}

void rlpList(std::vector<uint8_t> &out, const std::vector<uint8_t> &payload) {
  size_t len = payload.size();
  if (len <= 55) { out.push_back((uint8_t)(0xc0 + len)); }
  else {
    uint8_t lb[8]; int nl = 0; size_t l = len;
    while (l) { lb[nl++] = (uint8_t)(l & 0xff); l >>= 8; }
    out.push_back((uint8_t)(0xf7 + nl));
    for (int i = nl - 1; i >= 0; i--) out.push_back(lb[i]);
  }
  out.insert(out.end(), payload.begin(), payload.end());
}

// Extract raw 32-byte r,s from a DER-encoded uBitcoin Signature.
bool derToRS(const Signature &sig, uint8_t r[32], uint8_t s[32]) {
  uint8_t d[80];
  size_t n = sig.der(d, sizeof(d));
  if (n < 8 || d[0] != 0x30) return false;
  size_t i = 2;
  if (d[i++] != 0x02) return false;
  size_t rlen = d[i++]; const uint8_t *rp = d + i; i += rlen;
  if (i >= n || d[i++] != 0x02) return false;
  size_t slen = d[i++]; const uint8_t *sp = d + i;
  memset(r, 0, 32); memset(s, 0, 32);
  while (rlen > 32 && *rp == 0) { rp++; rlen--; }
  while (slen > 32 && *sp == 0) { sp++; slen--; }
  if (rlen > 32 || slen > 32) return false;
  memcpy(r + 32 - rlen, rp, rlen);
  memcpy(s + 32 - slen, sp, slen);
  return true;
}

// ---- token registry (contract hex lowercase, no 0x) ----
struct TokenInfo { const char *contract; const char *symbol; uint8_t decimals; };
const TokenInfo TOKENS[] = {
  {"dac17f958d2ee523a2206206994597c13d831ec7", "USDT", 6},   // Ethereum mainnet USDT
  {"a0b86991c6218b36c1d19d4a2e9eb0ce3606eb48", "USDC", 6},   // Ethereum mainnet USDC
  {"1c7d4b196cb0c7b01d743fbc6116a902379c7238", "USDC", 6},   // Sepolia testnet USDC (Circle) — verify
  // Add your own token here: {"<contract hex, lowercase, no 0x>", "SYMBOL", decimals},
  // Unknown tokens still work — they display as raw base units + the contract.
};

const TokenInfo *lookupToken(const uint8_t to[20]) {
  char hex[41];
  for (int i = 0; i < 20; i++) { hex[2 * i] = HEXCH[to[i] >> 4]; hex[2 * i + 1] = HEXCH[to[i] & 0x0F]; }
  hex[40] = '\0';
  for (const auto &t : TOKENS) if (strcmp(hex, t.contract) == 0) return &t;
  return nullptr;
}

// ---- pending transaction state ----
struct Field { uint8_t buf[32]; size_t len; };
int hexToField(const String &h, Field &f) { int n = hexToBytes(h, f.buf, 32); if (n < 0) return -1; f.len = (size_t)n; return n; }
uint32_t fieldToU32(const Field &f) { uint32_t v = 0; for (size_t i = 0; i < f.len; i++) v = (v << 8) | f.buf[i]; return v; }
uint64_t fieldToU64(const Field &f) { uint64_t v = 0; for (size_t i = 0; i < f.len; i++) v = (v << 8) | f.buf[i]; return v; }

bool  s_loaded = false;
uint32_t s_acct = 0;
Field s_chainId, s_nonce, s_maxPrio, s_maxFee, s_gas, s_value;
uint8_t s_to[20];
std::vector<uint8_t> s_data;

bool isErc20Transfer() {
  return s_data.size() >= 4 + 32 + 32 &&
         s_data[0] == 0xa9 && s_data[1] == 0x05 && s_data[2] == 0x9c && s_data[3] == 0xbb;
}

}  // namespace

// ============================================================================
// public API
// ============================================================================
namespace eth {

String address(const uint8_t *entropy, size_t entLen, const char *passphrase, uint32_t index) {
  String m = wallet::entropyToSeedPhrase(entropy, entLen);
  if (m.length() == 0) return String("");
  HDPrivateKey root(m, String(passphrase ? passphrase : ""), &Mainnet);
  m = "";
  char path[40];
  snprintf(path, sizeof(path), "m/44h/60h/0h/0/%lu", (unsigned long)index);
  HDPrivateKey child = root.derive(path);
  PublicKey pub = child.publicKey();
  pub.compressed = false;
  uint8_t sec[65];
  if (pub.sec(sec, sizeof(sec)) != 65) return String("");
  uint8_t h[32];
  keccak_256(sec + 1, 64, h);
  char out[43];
  toEip55(h + 12, out);
  return String(out);
}

void clearTx() {
  s_loaded = false;
  s_acct = 0;
  s_data.clear();
}

bool loadTx(const String &line) {
  clearTx();
  String parts[10]; int np = 0, start = 0;
  for (int i = 0; i <= (int)line.length() && np < 10; i++) {
    if (i == (int)line.length() || line[i] == '|') { parts[np++] = line.substring(start, i); start = i + 1; }
  }
  if (np < 10 || (parts[0] != "ETH"  && parts[0] != "BNB"  &&
                  parts[0] != "USDT" && parts[0] != "USDC" &&
                  parts[0] != "AVAX" && parts[0] != "BASE" &&
                  parts[0] != "POL"  && parts[0] != "CRO")) return false;
  s_acct = (uint32_t)parts[1].toInt();
  if (hexToField(parts[2], s_chainId) < 0) return false;
  if (hexToField(parts[3], s_nonce)   < 0) return false;
  if (hexToField(parts[4], s_maxPrio) < 0) return false;
  if (hexToField(parts[5], s_maxFee)  < 0) return false;
  if (hexToField(parts[6], s_gas)     < 0) return false;
  if (hexToBytes(parts[7], s_to, 20) != 20) return false;
  if (hexToField(parts[8], s_value)   < 0) return false;
  {
    uint8_t tmp[1024];
    int n = parts[9].length() ? hexToBytes(parts[9], tmp, sizeof(tmp)) : 0;
    if (n < 0) return false;
    s_data.assign(tmp, tmp + n);
  }
  s_loaded = true;
  return true;
}

bool txIsErc20() { return s_loaded && isErc20Transfer(); }

// Native gas token for a chain id (used when the tx is not an ERC-20 transfer).
// EVM chains share ETH's key/address; only the chainId (and thus the ticker) differ.
static String nativeSym(uint64_t chainId) {
  switch (chainId) {
    case 56: case 97:       return String("BNB");    // BNB Smart Chain
    case 43114: case 43113: return String("AVAX");   // Avalanche C-Chain
    case 137: case 80002:   return String("POL");    // Polygon
    case 25: case 338:      return String("CRO");    // Cronos
    default:                return String("ETH");    // Ethereum, Base, testnets
  }
}

String txSymbol() {
  if (!txIsErc20()) return nativeSym(txChainId());
  const TokenInfo *t = lookupToken(s_to);
  if (t) return String(t->symbol);
  char b[12]; snprintf(b, sizeof(b), "0x%02x%02x..", s_to[0], s_to[1]);
  return String(b);
}

String txAmountStr() {
  if (txIsErc20()) {
    const TokenInfo *t = lookupToken(s_to);
    int dec = t ? t->decimals : 0;               // unknown token -> raw base units
    return formatUnits(bytesToDecimal(&s_data[4 + 32], 32), dec);
  }
  return formatUnits(bytesToDecimal(s_value.buf, s_value.len), 18);
}

String txRecipient() {
  if (txIsErc20()) return addr0x(&s_data[4 + 12]);  // address is right-aligned in arg 1
  return addr0x(s_to);
}

String txFeeStr() {
  uint8_t fee[48];
  int n = bnMulU32(s_maxFee.buf, (int)s_maxFee.len, fieldToU32(s_gas), fee, sizeof(fee));
  if (n < 0) return String("?");
  return formatUnits(bytesToDecimal(fee, n), 18);
}

uint64_t txChainId() { return fieldToU64(s_chainId); }
String   txNativeSymbol() { return nativeSym(txChainId()); }

String signTx(const uint8_t *entropy, size_t entLen, const char *passphrase) {
  if (!s_loaded) return String("");
  String m = wallet::entropyToSeedPhrase(entropy, entLen);
  if (m.length() == 0) return String("");
  HDPrivateKey root(m, String(passphrase ? passphrase : ""), &Mainnet);
  m = "";
  char path[40];
  snprintf(path, sizeof(path), "m/44h/60h/0h/0/%lu", (unsigned long)s_acct);
  HDPrivateKey child = root.derive(path);

  // 9-item EIP-1559 body (shared by the unsigned + signed encodings).
  std::vector<uint8_t> items;
  rlpInt(items, s_chainId.buf, s_chainId.len);
  rlpInt(items, s_nonce.buf,   s_nonce.len);
  rlpInt(items, s_maxPrio.buf, s_maxPrio.len);
  rlpInt(items, s_maxFee.buf,  s_maxFee.len);
  rlpInt(items, s_gas.buf,     s_gas.len);
  rlpStr(items, s_to, 20);
  rlpInt(items, s_value.buf,   s_value.len);
  rlpStr(items, s_data.data(), s_data.size());
  items.push_back(0xc0);                          // empty accessList

  std::vector<uint8_t> unsignedTx; unsignedTx.push_back(0x02);
  rlpList(unsignedTx, items);
  uint8_t digest[32];
  keccak_256(unsignedTx.data(), unsignedTx.size(), digest);

  Signature sig = child.sign(digest);
  uint8_t r[32], s[32];
  if (!derToRS(sig, r, s)) return String("");
  uint8_t yParity = (uint8_t)(sig.index & 0x01);

  std::vector<uint8_t> sitems = items;
  rlpInt(sitems, &yParity, 1);
  rlpInt(sitems, r, 32);
  rlpInt(sitems, s, 32);
  std::vector<uint8_t> signedTx; signedTx.push_back(0x02);
  rlpList(signedTx, sitems);

  String out = "0x";
  for (size_t i = 0; i < signedTx.size(); i++) {
    out += HEXCH[signedTx[i] >> 4];
    out += HEXCH[signedTx[i] & 0x0F];
  }
  return out;
}

}  // namespace eth

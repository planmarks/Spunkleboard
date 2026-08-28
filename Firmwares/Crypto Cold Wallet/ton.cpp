#include "ton.h"
#include "wallet.h"
#include "slip10.h"
#include "ed25519.h"

#include <Bitcoin.h>       // fromBase64 / Conversion helpers
#include <string.h>

extern "C" {
  void sha256_Raw(const uint8_t *data, uint32_t len, uint8_t digest[32]);
  void mnemonic_to_seed(const char *mnemonic, const char *passphrase,
                        uint8_t seed[64], void (*cb)(uint32_t, uint32_t));
}

namespace {

static const char HEXCH[] = "0123456789abcdef";

// v4R2 wallet contract code (BOC, base64) — from ton-org/ton WalletContractV4.
// Structured so W5 is a drop-in: swap this constant + the data-cell layout.
static const char *V4R2_CODE_B64 =
  "te6ccgECFAEAAtQAART/APSkE/S88sgLAQIBIAIDAgFIBAUE+PKDCNcYINMf0x/THwL4I7vyZO1E0NMf"
  "0x/T//QE0VFDuvKhUVG68qIF+QFUEGT5EPKj+AAkpMjLH1JAyx9SMMv/UhD0AMntVPgPAdMHIcAAn2xR"
  "kyDXSpbTB9QC+wDoMOAhwAHjACHAAuMAAcADkTDjDQOkyMsfEssfy/8QERITAubQAdDTAyFxsJJfBOAi"
  "10nBIJJfBOAC0x8hghBwbHVnvSKCEGRzdHK9sJJfBeAD+kAwIPpEAcjKB8v/ydDtRNCBAUDXIfQEMFyB"
  "AQj0Cm+hMbOSXwfgBdM/yCWCEHBsdWe6kjgw4w0DghBkc3RyupJfBuMNBgcCASAICQB4AfoA9AQw+Cdv"
  "IjBQCqEhvvLgUIIQcGx1Z4MesXCAGFAEywUmzxZY+gIZ9ADLaRfLH1Jgyz8gyYBA+wAGAIpQBIEBCPRZ"
  "MO1E0IEBQNcgyAHPFvQAye1UAXKwjiOCEGRzdHKDHrFwgBhQBcsFUAPPFiP6AhPLassfyz/JgED7AJJf"
  "A+ICASAKCwBZvSQrb2omhAgKBrkPoCGEcNQICEekk30pkQzmkD6f+YN4EoAbeBAUiYcVnzGEAgFYDA0A"
  "EbjJftRNDXCx+AA9sp37UTQgQFA1yH0BDACyMoHy//J0AGBAQj0Cm+hMYAIBIA4PABmtznaiaEAga5Dr"
  "hf/AABmvHfaiaEAQa5DrhY/AAG7SB/oA1NQi+QAFyMoHFcv/ydB3dIAYyMsFywIizxZQBfoCFMtrEszM"
  "yXP7AMhAFIEBCPRR8qcCAHCBAQjXGPoA0z/IVCBHgQEI9FHyp4IQbm90ZXB0gBjIywXLAlAGzxZQBPoC"
  "FMtqEssfyz/Jc/sAAgBsgQEI1xj6ANM/MFIkgQEI9Fnyp4IQZHN0cnB0gBjIywXLAlAFzxZQA/oCE8tq"
  "yx8Syz/Jc/sAAAr0AMntVA==";

const int    WALLET_ID = 698983191;   // v4R2 default (workchain 0)
const size_t MAXCELLS  = 64;

struct Cell {
  uint8_t  data[128];   // up to 1023 bits
  uint16_t bitLen;
  uint8_t  refCount;
  uint8_t  refs[4];
};

// Representation hash + depth of a cell described by (data,bitLen) + resolved
// child hashes/depths. Ordinary cell, level 0.
void hashCell(const uint8_t *data, int bitLen,
              const uint8_t childHashes[4][32], const uint16_t *childDepths, int refCount,
              uint8_t outHash[32], uint16_t *outDepth) {
  int dataBytes = (bitLen + 7) >> 3;
  uint8_t d1 = (uint8_t)refCount;                                  // ordinary, level 0
  uint8_t d2 = (uint8_t)((bitLen >> 3) + ((bitLen + 7) >> 3));     // floor + ceil

  uint8_t buf[2 + 128 + 4 * 2 + 4 * 32];
  int p = 0;
  buf[p++] = d1; buf[p++] = d2;
  memcpy(buf + p, data, dataBytes);
  if (bitLen & 7) {                              // augment non-byte-aligned cell
    int used = bitLen & 7;                       // used bits in the last byte
    buf[p + dataBytes - 1] &= (uint8_t)(0xFF << (8 - used));
    buf[p + dataBytes - 1] |= (uint8_t)(0x80 >> used);
  }
  p += dataBytes;

  uint16_t maxd = 0;
  for (int i = 0; i < refCount; i++) { buf[p++] = childDepths[i] >> 8; buf[p++] = childDepths[i] & 0xff; if (childDepths[i] > maxd) maxd = childDepths[i]; }
  for (int i = 0; i < refCount; i++) { memcpy(buf + p, childHashes[i], 32); p += 32; }

  sha256_Raw(buf, (uint32_t)p, outHash);
  *outDepth = refCount ? (uint16_t)(maxd + 1) : 0;
}

// Parse a standard BOC into `cells`. Returns root cell index, or -1 on error.
int parseBoc(const uint8_t *b, size_t len, Cell *cells, size_t maxCells) {
  if (len < 6 || b[0] != 0xb5 || b[1] != 0xee || b[2] != 0x9c || b[3] != 0x72) return -1;
  size_t o = 4;
  uint8_t flags = b[o++];
  int refSize = flags & 0x07;
  bool hasIdx  = flags & 0x80;
  bool hasCrc  = flags & 0x40;
  int offSize = b[o++];
  if (refSize < 1 || refSize > 4 || offSize < 1) return -1;

  auto rd = [&](int nbytes) -> uint32_t {
    uint32_t v = 0; for (int i = 0; i < nbytes; i++) v = (v << 8) | b[o++]; return v;
  };
  uint32_t nCells = rd(refSize);
  uint32_t nRoots = rd(refSize);
  rd(refSize);                       // absent
  rd(offSize);                       // tot_cells_size
  if (nCells == 0 || nCells > maxCells || nRoots < 1) return -1;
  uint32_t rootIdx = rd(refSize);    // first root
  for (uint32_t i = 1; i < nRoots; i++) rd(refSize);
  if (hasIdx) for (uint32_t i = 0; i < nCells; i++) rd(offSize);

  for (uint32_t i = 0; i < nCells; i++) {
    if (o + 2 > len) return -1;
    uint8_t d1 = b[o++], d2 = b[o++];
    int rc = d1 & 0x07;
    if (d1 & 0x08) return -1;                    // exotic cells not supported here
    int dataBytes = (d2 >> 1) + (d2 & 1);
    int bitLen = (d2 & 1) ? -1 : (dataBytes * 8);
    if (o + dataBytes > len || dataBytes > 128 || rc > 4) return -1;
    Cell &c = cells[i];
    memcpy(c.data, b + o, dataBytes); o += dataBytes;
    if (d2 & 1) {                                // partial last byte -> find completion tag
      uint8_t last = c.data[dataBytes - 1];
      int usedBits = 7;
      while (usedBits >= 0 && !((last >> (7 - usedBits)) & 1)) usedBits--;   // locate the '1'
      bitLen = (dataBytes - 1) * 8 + usedBits;   // bits before the completion '1'
    }
    c.bitLen = (uint16_t)(bitLen < 0 ? 0 : bitLen);
    c.refCount = (uint8_t)rc;
    for (int r = 0; r < rc; r++) c.refs[r] = (uint8_t)rd(refSize);
  }
  (void)hasCrc;
  return (int)rootIdx;
}

// ---- pending signing state --------------------------------------------------
static bool    s_loaded = false;
static uint32_t s_acct  = 0;
static uint8_t s_hash[32];   // 32-byte message hash to sign

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ---- key derivation (SLIP-0010 ed25519, m/44'/607'/0') ----------------------
bool derivePub(const uint8_t *entropy, size_t entLen, const char *passphrase,
               uint8_t priv[32], uint8_t pub[32]) {
  String m = wallet::entropyToSeedPhrase(entropy, entLen);
  if (m.length() == 0) return false;
  uint8_t seed[64];
  mnemonic_to_seed(m.c_str(), passphrase ? passphrase : "", seed, nullptr);
  m = "";
  uint32_t path[3] = { 44, 607, 0 };             // slip10 hardens each element
  bool ok = slip10::deriveEd25519(seed, path, 3, priv);
  memset(seed, 0, sizeof(seed));
  if (!ok) return false;
  ed25519::publicKey(priv, pub);
  return true;
}

// CRC16/XMODEM (poly 0x1021, init 0) — used by the user-friendly address.
uint16_t crc16(const uint8_t *d, int len) {
  uint16_t crc = 0;
  for (int i = 0; i < len; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

String base64url(const uint8_t *d, int len) {
  static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  String out;
  for (int i = 0; i < len; i += 3) {
    int n = (d[i] << 16) | ((i + 1 < len ? d[i + 1] : 0) << 8) | (i + 2 < len ? d[i + 2] : 0);
    out += A[(n >> 18) & 63];
    out += A[(n >> 12) & 63];
    out += (i + 1 < len) ? A[(n >> 6) & 63] : '=';
    out += (i + 2 < len) ? A[n & 63] : '=';
  }
  return out;
}

}  // namespace

namespace ton {

String address(const uint8_t *entropy, size_t entLen,
               const char *passphrase, uint32_t index) {
  (void)index;   // v4R2 is single-account per key; index reserved for future use
  uint8_t priv[32], pub[32];
  if (!derivePub(entropy, entLen, passphrase, priv, pub)) return String("");
  memset(priv, 0, sizeof(priv));

  // Decode + parse the v4R2 code cell, compute its representation hash + depth.
  static uint8_t bocBytes[1024];
  size_t bl = fromBase64(V4R2_CODE_B64, bocBytes, sizeof(bocBytes));
  if (bl == 0) return String("");
  static Cell cells[MAXCELLS];
  int root = parseBoc(bocBytes, bl, cells, MAXCELLS);
  if (root < 0) return String("");

  // Hash all code cells bottom-up (BOC refs point to higher indices).
  static uint8_t hashes[MAXCELLS][32];
  static uint16_t depths[MAXCELLS];
  // count is not returned separately; re-derive from parse by scanning refs is
  // avoided — parseBoc filled cells[0..nCells); we hash indices we can reach by
  // processing the full array in reverse (unused cells are harmless).
  for (int i = (int)MAXCELLS - 1; i >= 0; i--) {
    Cell &c = cells[i];
    if (c.refCount > 4) { c.refCount = 0; c.bitLen = 0; }   // guard uninitialised slots
    uint8_t ch[4][32]; uint16_t cd[4];
    for (int r = 0; r < c.refCount; r++) { memcpy(ch[r], hashes[c.refs[r]], 32); cd[r] = depths[c.refs[r]]; }
    hashCell(c.data, c.bitLen, ch, cd, c.refCount, hashes[i], &depths[i]);
  }
  uint8_t codeHash[32]; memcpy(codeHash, hashes[root], 32);
  uint16_t codeDepth = depths[root];

  // Data cell: seqno(32=0) | walletId(32) | pubkey(256) | plugins(1=0) = 321 bits.
  uint8_t dataBits[41];
  memset(dataBits, 0, sizeof(dataBits));
  dataBits[4] = (WALLET_ID >> 24) & 0xff; dataBits[5] = (WALLET_ID >> 16) & 0xff;
  dataBits[6] = (WALLET_ID >> 8) & 0xff;  dataBits[7] = WALLET_ID & 0xff;
  memcpy(dataBits + 8, pub, 32);           // byte 40 (plugins bit) stays 0
  uint8_t dataHash[32]; uint16_t dataDepth;
  hashCell(dataBits, 321, nullptr, nullptr, 0, dataHash, &dataDepth);

  // StateInit: 00110 (no split, no special, +code, +data, empty library) + 2 refs.
  uint8_t siBits[1] = { 0x30 };            // '00110' in the high bits
  uint8_t siChildH[4][32]; uint16_t siChildD[4];
  memcpy(siChildH[0], codeHash, 32); siChildD[0] = codeDepth;
  memcpy(siChildH[1], dataHash, 32); siChildD[1] = dataDepth;
  uint8_t addrHash[32]; uint16_t addrDepth;
  hashCell(siBits, 5, siChildH, siChildD, 2, addrHash, &addrDepth);

  // User-friendly bounceable mainnet address: 0x11 | wc(0) | hash | crc16.
  uint8_t a[36];
  a[0] = 0x11; a[1] = 0x00;
  memcpy(a + 2, addrHash, 32);
  uint16_t crc = crc16(a, 34);
  a[34] = crc >> 8; a[35] = crc & 0xff;
  return base64url(a, 36);
}

bool loadTx(const String &line) {
  clearTx();
  // TON|acct|msgHashHex (32-byte hash to sign)
  String parts[3]; int np = 0, start = 0;
  for (int i = 0; i <= (int)line.length() && np < 3; i++) {
    if (i == (int)line.length() || line[i] == '|') { parts[np++] = line.substring(start, i); start = i + 1; }
  }
  if (np < 3 || parts[0] != "TON") return false;
  s_acct = (uint32_t)parts[1].toInt();

  const String &h = parts[2];
  if (h.length() != 64) return false;      // exactly 32 bytes
  for (int i = 0; i < 32; i++) {
    int hi = hexVal(h[2 * i]), lo = hexVal(h[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    s_hash[i] = (uint8_t)((hi << 4) | lo);
  }
  s_loaded = true;
  return true;
}

String txHashHex() {
  if (!s_loaded) return String("?");
  char buf[9];
  for (int i = 0; i < 4; i++) { buf[2 * i] = HEXCH[s_hash[i] >> 4]; buf[2 * i + 1] = HEXCH[s_hash[i] & 0x0F]; }
  buf[8] = '\0';
  return String(buf);
}

String signTx(const uint8_t *entropy, size_t entLen, const char *passphrase) {
  if (!s_loaded) return String("");
  uint8_t priv[32], pub[32];
  if (!derivePub(entropy, entLen, passphrase, priv, pub)) return String("");

  uint8_t sig[64];
  ed25519::sign(priv, s_hash, 32, sig);
  memset(priv, 0, sizeof(priv));

  String out; out.reserve(128);
  for (int i = 0; i < 64; i++) { out += HEXCH[sig[i] >> 4]; out += HEXCH[sig[i] & 0x0F]; }
  return out;
}

void clearTx() {
  s_loaded = false;
  s_acct   = 0;
  memset(s_hash, 0, sizeof(s_hash));
}

}  // namespace ton

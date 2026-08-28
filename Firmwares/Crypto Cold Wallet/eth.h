#pragma once
//
// Ethereum support.
//   E1: address derivation (BIP44 m/44'/60'/0'/0/i, EIP-55).
//   E2: EIP-1559 transaction signing + ERC-20 transfer decoding (incl. USDT).
//
// The device is given the transaction FIELDS (not an opaque blob), displays the
// recipient/amount/fee it will sign, builds the canonical EIP-1559 RLP itself,
// and returns the signed raw transaction — so it signs exactly what it shows.
//
// Interchange line (fields '|'-separated; numeric fields are hex big-endian,
// acct is decimal, `to`/`data` are hex without 0x, `data` may be empty):
//   ETH|acct|chainId|nonce|maxPrioFee|maxFee|gasLimit|to|value|data

#include <Arduino.h>

namespace eth {

// EIP-55 checksummed "0x..." address for account 0, receive index `index`.
String address(const uint8_t *entropy, size_t entLen, const char *passphrase, uint32_t index);

// Parse an interchange line into the pending transaction. Returns true if valid.
bool loadTx(const String &line);

// ---- Display accessors (valid after loadTx) --------------------------------
bool     txIsErc20();       // true if data is an ERC-20 transfer(address,uint256)
String   txSymbol();        // "ETH", "USDT", or "0x1234…" for an unknown token
String   txAmountStr();     // decimal amount (ETH, or token units) being sent
String   txRecipient();     // "0x…" (token recipient for ERC-20, else `to`)
String   txFeeStr();        // max fee in ETH (maxFee * gasLimit)
uint64_t txChainId();
String   txNativeSymbol();  // native gas token (ETH/BNB/AVAX/POL/CRO) — used for the fee

// Sign the loaded tx with the seed. Returns "0x…" signed raw tx, "" on failure.
String   signTx(const uint8_t *entropy, size_t entLen, const char *passphrase);

void     clearTx();

}  // namespace eth

/**
 * Copyright (c) 2013-2016 Tomas Dzetkulic
 * Copyright (c) 2013-2016 Pavol Rusnak
 * Copyright (c) 2015-2016 Jochen Hoenicke
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 * OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "keepkey/crypto/bignum.h"
#include "keepkey/crypto/hmac.h"
#include "keepkey/crypto/ecdsa.h"
#include "keepkey/crypto/bip32.h"
#include "keepkey/crypto/sha2.h"
#include "keepkey/crypto/ripemd160.h"
#include "keepkey/crypto/base58.h"
#include "keepkey/crypto/macros.h"
#include "keepkey/crypto/curves.h"
#include "keepkey/crypto/secp256k1.h"
#include "keepkey/crypto/nist256p1.h"
#include "keepkey/crypto/ed25519-donna/ed25519.h"
#if USE_ETHEREUM
#include "keepkey/crypto/sha3.h"
#endif

#include <string.h>
#include <stdbool.h>

const curve_info ed25519_info = {
	/* bip32_name */
	"ed25519 seed",
	0
};

int hdnode_from_xpub(uint32_t depth, uint32_t child_num, const uint8_t *chain_code, const uint8_t *public_key, const char* curve, HDNode *out)
{
	const curve_info *info = get_curve_by_name(curve);
	if (info == 0) {
		return 0;
	}
	if (public_key[0] != 0x02 && public_key[0] != 0x03) { // invalid pubkey
		return 0;
	}
	out->curve = info;
	out->depth = depth;
	out->child_num = child_num;
	memcpy(out->chain_code, chain_code, 32);
	MEMSET_BZERO(out->private_key, 32);
	memcpy(out->public_key, public_key, 33);
	return 1;
}

int hdnode_from_xprv(uint32_t depth, uint32_t child_num, const uint8_t *chain_code, const uint8_t *private_key, const char* curve, HDNode *out)
{
	bool failed = false;
	const curve_info *info = get_curve_by_name(curve);
	if (info == 0) {
		failed = true;
	} else {
		bignum256 a;
		bn_read_be(private_key, &a);
		if (bn_is_zero(&a)) { // == 0
			failed = true;
		} else {
			if (!bn_is_less(&a, &info->params->order)) { // >= order
				failed = true;
			}
		}
		MEMSET_BZERO(&a, sizeof(a));
	}

	if (failed) {
		return 0;
	}

	out->curve = info;
	out->depth = depth;
	out->child_num = child_num;
	memcpy(out->chain_code, chain_code, 32);
	memcpy(out->private_key, private_key, 32);
	MEMSET_BZERO(out->public_key, sizeof(out->public_key));
	return 1;
}

int hdnode_from_seed(const uint8_t *seed, int seed_len, const char* curve, HDNode *out)
{
	uint8_t I[32 + 32];
	memset(out, 0, sizeof(HDNode));
	out->depth = 0;
	out->child_num = 0;
	out->curve = get_curve_by_name(curve);
	if (out->curve == 0) {
		return 0;
	}
	hmac_sha512((const uint8_t*) out->curve->bip32_name,
				strlen(out->curve->bip32_name), seed, seed_len, I);

	if (out->curve->params) {
		bignum256 a;
		while (true) {
			bn_read_be(I, &a);
			if (!bn_is_zero(&a) // != 0
				&& bn_is_less(&a, &out->curve->params->order)) { // < order
				break;
			}
			hmac_sha512((const uint8_t*) out->curve->bip32_name,
						strlen(out->curve->bip32_name), I, sizeof(I), I);
		}
		MEMSET_BZERO(&a, sizeof(a));
	}
	memcpy(out->private_key, I, 32);
	memcpy(out->chain_code, I + 32, 32);

	MEMSET_BZERO(I, sizeof(I));
	return 1;
}

uint32_t hdnode_fingerprint(HDNode *node)
{
	uint8_t digest[32];
	uint32_t fingerprint;

	hdnode_fill_public_key(node);
	sha256_Raw(node->public_key, 33, digest);
	ripemd160(digest, 32, digest);
	fingerprint = ((uint32_t)digest[0] << 24) + (digest[1] << 16) + (digest[2] << 8) + digest[3];
	MEMSET_BZERO(digest, sizeof(digest));
	return fingerprint;
}

int hdnode_private_ckd(HDNode *inout, uint32_t i)
{
	uint8_t data[1 + 32 + 4];
	uint8_t I[32 + 32];
	bignum256 a, b;

	if (i & 0x80000000) { // private derivation
		data[0] = 0;
		memcpy(data + 1, inout->private_key, 32);
	} else { // public derivation
		if (!inout->curve->params) {
			return 0;
		}
		hdnode_fill_public_key(inout);
		memcpy(data, inout->public_key, 33);
	}
	write_be(data + 33, i);

	bn_read_be(inout->private_key, &a);

	hmac_sha512(inout->chain_code, 32, data, sizeof(data), I);
	if (inout->curve->params) {
		while (true) {
			bool failed = false;
			bn_read_be(I, &b);
			if (!bn_is_less(&b, &inout->curve->params->order)) { // >= order
				failed = true;
			} else {
				bn_addmod(&b, &a, &inout->curve->params->order);
				bn_mod(&b, &inout->curve->params->order);
				if (bn_is_zero(&b)) {
					failed = true;
				}
			}
			
			if (!failed) {
				bn_write_be(&b, inout->private_key);
				break;
			}

			data[0] = 1;
			memcpy(data + 1, I + 32, 32);
			hmac_sha512(inout->chain_code, 32, data, sizeof(data), I);
		}
	} else {
		memcpy(inout->private_key, I, 32);
	}
		
	memcpy(inout->chain_code, I + 32, 32);
	inout->depth++;
	inout->child_num = i;
	MEMSET_BZERO(inout->public_key, sizeof(inout->public_key));

	// making sure to wipe our memory
	MEMSET_BZERO(&a, sizeof(a));
	MEMSET_BZERO(&b, sizeof(b));
	MEMSET_BZERO(I, sizeof(I));
	MEMSET_BZERO(data, sizeof(data));
	return 1;
}

int hdnode_public_ckd(HDNode *inout, uint32_t i)
{
	uint8_t data[1 + 32 + 4];
	uint8_t I[32 + 32];
	uint8_t fingerprint[32];
	curve_point a, b;
	bignum256 c;

	if (i & 0x80000000) { // private derivation
		return 0;
	} else { // public derivation
		if (!inout->curve->params) {
			return 0;
		}
		memcpy(data, inout->public_key, 33);
	}
	write_be(data + 33, i);
	memset(inout->private_key, 0, 32);

	if (!ecdsa_read_pubkey(inout->curve->params, inout->public_key, &a)) {
		return 0;
	}

	while (true) {
		bool failed = false;
		hmac_sha512(inout->chain_code, 32, data, sizeof(data), I);
		bn_read_be(I, &c);
		if (!bn_is_less(&c, &inout->curve->params->order)) { // >= order
			failed = true;
		} else {
			scalar_multiply(inout->curve->params, &c, &b); // b = c * G
			point_add(inout->curve->params, &a, &b);       // b = a + b
			if (point_is_infinity(&b)) {
				failed = true;
			}
		}
		
		if (!failed) {
			inout->public_key[0] = 0x02 | (b.y.val[0] & 0x01);
			bn_write_be(&b.x, inout->public_key + 1);
			break;
		}

		data[0] = 1;
		memcpy(data + 1, I + 32, 32);
	}

	inout->depth++;
	inout->child_num = i;
	memcpy(inout->chain_code, I + 32, 32);

	// Wipe all stack data.
	MEMSET_BZERO(data, sizeof(data));
	MEMSET_BZERO(I, sizeof(I));
	MEMSET_BZERO(fingerprint, sizeof(fingerprint));
	MEMSET_BZERO(&a, sizeof(a));
	MEMSET_BZERO(&b, sizeof(b));
	MEMSET_BZERO(&c, sizeof(c));

	return 1;
}

int hdnode_public_ckd_address_optimized(const curve_point *pub, const uint8_t *public_key, const uint8_t *chain_code, uint32_t i, uint8_t version, char *addr, int addrsize)
{
	uint8_t data[1 + 32 + 4];
	uint8_t I[32 + 32];
	uint8_t child_pubkey[33];
	curve_point b;
	bignum256 c;

	if (i & 0x80000000) { // private derivation
		return 0;
	}
	memcpy(data, public_key, 33);
	write_be(data + 33, i);

	while (true) {
		bool failed = false;
		hmac_sha512(chain_code, 32, data, sizeof(data), I);
		bn_read_be(I, &c);
		if (!bn_is_less(&c, &secp256k1.order)) { // >= order
			failed = true;
		} else {
			scalar_multiply(&secp256k1, &c, &b); // b = c * G
			point_add(&secp256k1, pub, &b);       // b = a + b
			if (point_is_infinity(&b)) {
				failed = true;
			}
		}
		if (!failed) {
			child_pubkey[0] = 0x02 | (b.y.val[0] & 0x01);
			bn_write_be(&b.x, child_pubkey + 1);
			break;
		}
		data[0] = 1;
		memcpy(data + 1, I + 32, 32);
	}
	ecdsa_get_address(child_pubkey, version, addr, addrsize);
	return 1;
}

#if USE_BIP32_CACHE

static bool private_ckd_cache_root_set = false;
static HDNode private_ckd_cache_root;
static int private_ckd_cache_index = 0;

static struct {
	bool set;
	size_t depth;
	uint32_t i[BIP32_CACHE_MAXDEPTH];
	HDNode node;
} private_ckd_cache[BIP32_CACHE_SIZE];

int hdnode_private_ckd_cached(HDNode *inout, const uint32_t *i, size_t i_count)
{
	if (i_count == 0) {
		return 1;
	}
	if (i_count == 1) {
		if (hdnode_private_ckd(inout, i[0]) == 0) return 0;
		return 1;
	}

	bool found = false;
	// if root is not set or not the same
	if (!private_ckd_cache_root_set || memcmp(&private_ckd_cache_root, inout, sizeof(HDNode)) != 0) {
		// clear the cache
		private_ckd_cache_index = 0;
		memset(private_ckd_cache, 0, sizeof(private_ckd_cache));
		// setup new root
		memcpy(&private_ckd_cache_root, inout, sizeof(HDNode));
		private_ckd_cache_root_set = true;
	} else {
		// try to find parent
		int j;
		for (j = 0; j < BIP32_CACHE_SIZE; j++) {
			if (private_ckd_cache[j].set &&
			    private_ckd_cache[j].depth == i_count - 1 &&
			    memcmp(private_ckd_cache[j].i, i, (i_count - 1) * sizeof(uint32_t)) == 0 &&
				private_ckd_cache[j].node.curve == inout->curve) {
				memcpy(inout, &(private_ckd_cache[j].node), sizeof(HDNode));
				found = true;
				break;
			}
		}
	}

	// else derive parent
	if (!found) {
		size_t k;
		for (k = 0; k < i_count - 1; k++) {
			if (hdnode_private_ckd(inout, i[k]) == 0) return 0;
		}
		// and save it
		memset(&(private_ckd_cache[private_ckd_cache_index]), 0, sizeof(private_ckd_cache[private_ckd_cache_index]));
		private_ckd_cache[private_ckd_cache_index].set = true;
		private_ckd_cache[private_ckd_cache_index].depth = i_count - 1;
		memcpy(private_ckd_cache[private_ckd_cache_index].i, i, (i_count - 1) * sizeof(uint32_t));
		memcpy(&(private_ckd_cache[private_ckd_cache_index].node), inout, sizeof(HDNode));
		private_ckd_cache_index = (private_ckd_cache_index + 1) % BIP32_CACHE_SIZE;
	}

	if (hdnode_private_ckd(inout, i[i_count - 1]) == 0) return 0;

	return 1;
}

#endif

void hdnode_get_address_raw(HDNode *node, uint32_t version, uint8_t *addr_raw)
{
	hdnode_fill_public_key(node);
	ecdsa_get_address_raw(node->public_key, version, addr_raw);
}

void hdnode_fill_public_key(HDNode *node)
{
	if (node->public_key[0] != 0)
		return;
	if (node->curve == &ed25519_info) {
		node->public_key[0] = 1;
		ed25519_publickey(node->private_key, node->public_key + 1);
	} else {
		ecdsa_get_public_key33(node->curve->params, node->private_key, node->public_key);
	}
}

#if USE_ETHEREUM
int hdnode_get_ethereum_pubkeyhash(const HDNode *node, uint8_t *pubkeyhash)
{
	uint8_t buf[65];
	SHA3_CTX ctx;

	/* get uncompressed public key */
	ecdsa_get_public_key65(node->curve->params, node->private_key, buf);

	/* compute sha3 of x and y coordinate without 04 prefix */
	sha3_256_Init(&ctx);
	sha3_Update(&ctx, buf + 1, 64);
	keccak_Final(&ctx, buf);

	/* result are the least significant 160 bits */
	memcpy(pubkeyhash, buf + 12, 20);

	return 1;
}
#endif

// msg is a data to be signed
// msg_len is the message length
int hdnode_sign(HDNode *node, const uint8_t *msg, uint32_t msg_len, uint8_t *sig, uint8_t *pby)
{
	if (node->curve == &ed25519_info) {
		hdnode_fill_public_key(node);
		ed25519_sign(msg, msg_len, node->private_key, node->public_key + 1, sig);
		return 0;
	} else {
		return ecdsa_sign(node->curve->params, node->private_key, msg, msg_len, sig, pby);
	}
}

int hdnode_sign_digest(HDNode *node, const uint8_t *digest, uint8_t *sig, uint8_t *pby)
{
	if (node->curve == &ed25519_info) {
		hdnode_fill_public_key(node);
		ed25519_sign(digest, 32, node->private_key, node->public_key + 1, sig);
		return 0;
	} else {
		return ecdsa_sign_digest(node->curve->params, node->private_key, digest, sig, pby);
	}
}


void hdnode_serialize(const HDNode *node, uint32_t fingerprint, uint32_t version, char use_public, char *str, int strsize)
{
	uint8_t node_data[78];
	write_be(node_data, version);
	node_data[4] = node->depth;
	write_be(node_data + 5, fingerprint);
	write_be(node_data + 9, node->child_num);
	memcpy(node_data + 13, node->chain_code, 32);
	if (use_public) {
		memcpy(node_data + 45, node->public_key, 33);
	} else {
		node_data[45] = 0;
		memcpy(node_data + 46, node->private_key, 32);
	}
	base58_encode_check(node_data, sizeof(node_data), str, strsize);

	MEMSET_BZERO(node_data, sizeof(node_data));
}

void hdnode_serialize_public(const HDNode *node, uint32_t fingerprint, char *str, int strsize)
{
	hdnode_serialize(node, fingerprint, 0x0488B21E, 1, str, strsize);
}

void hdnode_serialize_private(const HDNode *node, uint32_t fingerprint, char *str, int strsize)
{
	hdnode_serialize(node, fingerprint, 0x0488ADE4, 0, str, strsize);
}

// check for validity of curve point in case of public data not performed
int hdnode_deserialize(const char *str, HDNode *node)
{
	uint8_t node_data[78];
	memset(node, 0, sizeof(HDNode));
	if (base58_decode_check(str, node_data, sizeof(node_data)) != sizeof(node_data)) {
		return -1;
	}
	node->curve = get_curve_by_name(SECP256K1_NAME);
	uint32_t version = read_be(node_data);
	if (version == 0x0488B21E) { // public node
		MEMSET_BZERO(node->private_key, sizeof(node->private_key));
		memcpy(node->public_key, node_data + 45, 33);
	} else if (version == 0x0488ADE4) { // private node
		if (node_data[45]) { // invalid data
			return -2;
		}
		memcpy(node->private_key, node_data + 46, 32);
		MEMSET_BZERO(node->public_key, sizeof(node->public_key));
	} else {
		return -3; // invalid version
	}
	node->depth = node_data[4];
	node->child_num = read_be(node_data + 9);
	memcpy(node->chain_code, node_data + 13, 32);
	return 0;
}

const curve_info *get_curve_by_name(const char *curve_name) {
	if (curve_name == 0) {
		return 0;
	}
	if (strcmp(curve_name, SECP256K1_NAME) == 0) {
		return &secp256k1_info;
	}
	if (strcmp(curve_name, NIST256P1_NAME) == 0) {
		return &nist256p1_info;
	}
	if (strcmp(curve_name, ED25519_NAME) == 0) {
		return &ed25519_info;
	}
	return 0;
}

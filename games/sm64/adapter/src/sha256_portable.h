/* SHA-256 (FIPS 180-4) where CommonCrypto is missing (Linux, Android), under CommonCrypto's names so
 * sm64_pack_stamp.c reads the same on every platform. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define CC_SHA256_DIGEST_LENGTH 32
typedef uint32_t CC_LONG;
typedef struct { uint32_t h[8]; uint64_t bits; unsigned char buf[64]; size_t n; } CC_SHA256_CTX;
void sm64_sha256_init(CC_SHA256_CTX *c);
void sm64_sha256_update(CC_SHA256_CTX *c, const void *data, CC_LONG len);
void sm64_sha256_final(unsigned char out[CC_SHA256_DIGEST_LENGTH], CC_SHA256_CTX *c);
#define CC_SHA256_Init sm64_sha256_init
#define CC_SHA256_Update sm64_sha256_update
#define CC_SHA256_Final sm64_sha256_final

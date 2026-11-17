// SHA-256 (runtime/sha256.c)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } Sha256;
void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);
bool sha256_file_hex(const char *path, char hex[65]);  // lowercase hex of a whole file; false: couldn't read it

#ifdef __cplusplus
}
#endif

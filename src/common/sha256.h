#ifndef IPC_COMMON_SHA256_H
#define IPC_COMMON_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
/* Write 64-char lowercase hex + NUL into hex_out (needs >=65). */
void sha256_hex(const uint8_t *data, size_t len, char *hex_out);

#ifdef __cplusplus
}
#endif

#endif

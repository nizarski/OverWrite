#ifndef OVERWRITE_CHACHA20_H
#define OVERWRITE_CHACHA20_H

#include <stddef.h>
#include <stdint.h>

void chacha20_init(uint32_t state[16], const uint8_t key[32],
                   const uint8_t nonce[12], uint32_t counter);
void chacha20_xor(uint32_t state[16], uint8_t *out, const uint8_t *in, size_t len);

#endif

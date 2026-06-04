/* Public-domain ChaCha20 ( Bernstein ). */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void chacha_quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a += *b; *d ^= *a; *d = ROTL32(*d, 16);
    *c += *d; *b ^= *c; *b = ROTL32(*b, 12);
    *a += *b; *d ^= *a; *d = ROTL32(*d, 8);
    *c += *d; *b ^= *c; *b = ROTL32(*b, 7);
}

static void chacha20_block(const uint32_t in[16], uint32_t out[16])
{
    uint32_t x[16];
    size_t i;

    memcpy(x, in, sizeof(x));
    for (i = 0; i < 10; i++) {
        chacha_quarter_round(&x[0], &x[4], &x[8],  &x[12]);
        chacha_quarter_round(&x[1], &x[5], &x[9],  &x[13]);
        chacha_quarter_round(&x[2], &x[6], &x[10], &x[14]);
        chacha_quarter_round(&x[3], &x[7], &x[11], &x[15]);
        chacha_quarter_round(&x[0], &x[5], &x[10], &x[15]);
        chacha_quarter_round(&x[1], &x[6], &x[11], &x[12]);
        chacha_quarter_round(&x[2], &x[7], &x[8],  &x[13]);
        chacha_quarter_round(&x[3], &x[4], &x[9],  &x[14]);
    }
    for (i = 0; i < 16; i++) {
        out[i] = x[i] + in[i];
    }
}

void chacha20_init(uint32_t state[16], const uint8_t key[32],
                   const uint8_t nonce[12], uint32_t counter)
{
    size_t i;

    state[0] = 0x61707865U;
    state[1] = 0x3320646eU;
    state[2] = 0x79622d32U;
    state[3] = 0x6b206574U;
    for (i = 0; i < 8; i++) {
        state[4 + i] = ((const uint32_t *)key)[i];
    }
    state[12] = counter;
    for (i = 0; i < 3; i++) {
        state[13 + i] = ((const uint32_t *)nonce)[i];
    }
}

void chacha20_xor(uint32_t state[16], uint8_t *out, const uint8_t *in, size_t len)
{
    uint32_t block[16];
    uint8_t keystream[64];
    size_t off = 0;

    while (off < len) {
        size_t n;
        chacha20_block(state, block);
        memcpy(keystream, block, 64);
        state[12]++;
        n = len - off;
        if (n > 64) {
            n = 64;
        }
        if (in != NULL) {
            size_t i;
            for (i = 0; i < n; i++) {
                out[off + i] = in[off + i] ^ keystream[i];
            }
        } else {
            memcpy(out + off, keystream, n);
        }
        off += n;
    }
}

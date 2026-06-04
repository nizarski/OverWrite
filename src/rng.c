#include "rng.h"
#include "chacha20.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct rng_ctx {
    rng_mode_t mode;
    uint32_t chacha_state[16];
    uint64_t turbo_s[4];
    uint32_t pass_index;
    uint32_t thread_salt;
};

static uint64_t rotl64(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static uint64_t xoshiro256ss(uint64_t s[4])
{
    uint64_t result = rotl64(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;

    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return result;
}

static void turbo_fill(uint64_t s[4], uint8_t *buf, size_t len)
{
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t v = xoshiro256ss(s);
        memcpy(buf + i, &v, 8);
        i += 8;
    }
    if (i < len) {
        uint64_t v = xoshiro256ss(s);
        memcpy(buf + i, &v, len - i);
    }
}

static int parse_nonce_hex(const char *hex, uint8_t out[32])
{
    size_t len;
    size_t i;

    if (hex == NULL || hex[0] == '\0') {
        return 0;
    }
    len = strlen(hex);
    if (len > 64) {
        return -1;
    }
    memset(out, 0, 32);
    for (i = 0; i < len; i += 2) {
        unsigned int byte;
        char pair[3] = { hex[i], hex[i + 1] ? hex[i + 1] : '0', '\0' };
        if (sscanf(pair, "%02x", &byte) != 1) {
            return -1;
        }
        out[i / 2] = (uint8_t)byte;
    }
    return 0;
}

static void mix_key(uint8_t key[32], const uint8_t nonce[32], uint32_t pass,
                    uint32_t salt)
{
    size_t i;
    for (i = 0; i < 32; i++) {
        key[i] ^= nonce[i];
    }
    key[(pass * 7) % 32] ^= (uint8_t)(pass ^ salt);
    key[(pass * 13 + 5) % 32] ^= (uint8_t)(salt >> ((pass % 4) * 8));
}

int rng_create(rng_mode_t mode, const char *nonce_hex, rng_ctx_t **out)
{
    rng_ctx_t *ctx;
    uint8_t key[32];
    uint8_t nonce_mix[32];
    uint8_t chacha_nonce[12];

    if (out == NULL) {
        return -1;
    }

    ctx = (rng_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -1;
    }

    ctx->mode = mode;
    if (platform_os_random(key, sizeof(key)) != 0) {
        free(ctx);
        return -1;
    }
    memset(nonce_mix, 0, sizeof(nonce_mix));
    if (parse_nonce_hex(nonce_hex, nonce_mix) != 0) {
        free(ctx);
        return -1;
    }
    mix_key(key, nonce_mix, 0, 0);

    if (platform_os_random(chacha_nonce, sizeof(chacha_nonce)) != 0) {
        free(ctx);
        return -1;
    }
    chacha20_init(ctx->chacha_state, key, chacha_nonce, 1);

    if (platform_os_random(ctx->turbo_s, sizeof(ctx->turbo_s)) != 0) {
        free(ctx);
        return -1;
    }
    for (size_t i = 0; i < 4; i++) {
        ctx->turbo_s[i] ^= ((uint64_t *)nonce_mix)[i % 4];
    }

    *out = ctx;
    return 0;
}

void rng_destroy(rng_ctx_t *ctx)
{
    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
        free(ctx);
    }
}

void rng_set_thread_salt(rng_ctx_t *ctx, uint32_t salt)
{
    if (ctx != NULL) {
        ctx->thread_salt = salt;
    }
}

int rng_reseed_pass(rng_ctx_t *ctx, uint32_t pass_index)
{
    uint8_t key[32];
    uint8_t chacha_nonce[12];

    if (ctx == NULL) {
        return -1;
    }
    ctx->pass_index = pass_index;
    if (platform_os_random(key, sizeof(key)) != 0) {
        return -1;
    }
    mix_key(key, (const uint8_t *)ctx->turbo_s, pass_index, ctx->thread_salt);
    if (platform_os_random(chacha_nonce, sizeof(chacha_nonce)) != 0) {
        return -1;
    }
    chacha20_init(ctx->chacha_state, key, chacha_nonce, pass_index + 1);
    if (platform_os_random(ctx->turbo_s, sizeof(ctx->turbo_s)) != 0) {
        return -1;
    }
    return 0;
}

void rng_fill(rng_ctx_t *ctx, void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;

    if (ctx == NULL || buf == NULL || len == 0) {
        return;
    }

    switch (ctx->mode) {
    case RNG_OS_CHUNK:
        if (platform_os_random(out, len) != 0) {
            memset(out, 0, len);
        }
        break;

    case RNG_TURBO:
        turbo_fill(ctx->turbo_s, out, len);
        break;

    case RNG_VAULT:
        chacha20_xor(ctx->chacha_state, out, NULL, len);
        break;

    case RNG_HYBRID:
        if (len <= 8192) {
            chacha20_xor(ctx->chacha_state, out, NULL, len);
        } else {
            chacha20_xor(ctx->chacha_state, out, NULL, 4096);
            turbo_fill(ctx->turbo_s, out + 4096, len - 8192);
            chacha20_xor(ctx->chacha_state, out + len - 4096, NULL, 4096);
        }
        break;
    }
}

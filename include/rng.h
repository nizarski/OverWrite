#ifndef OVERWRITE_RNG_H
#define OVERWRITE_RNG_H

#include "common.h"
#include <stddef.h>
#include <stdint.h>

typedef struct rng_ctx rng_ctx_t;

int  rng_create(rng_mode_t mode, const char *nonce_hex, rng_ctx_t **out);
void rng_destroy(rng_ctx_t *ctx);
void rng_fill(rng_ctx_t *ctx, void *buf, size_t len);
void rng_set_thread_salt(rng_ctx_t *ctx, uint32_t salt);
int  rng_reseed_pass(rng_ctx_t *ctx, uint32_t pass_index);

#endif

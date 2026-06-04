#ifndef OVERWRITE_SAFETY_H
#define OVERWRITE_SAFETY_H

#include "target.h"

#define SAFETY_PROMPT_MAX 4160

typedef struct {
    char prompt[SAFETY_PROMPT_MAX];
    bool blocked;
    char reason[256];
} safety_result_t;

int  safety_check(const wipe_target_t *target, const overwrite_config_t *cfg,
                  safety_result_t *result);
int  safety_confirm(const wipe_target_t *target, const overwrite_config_t *cfg,
                    const safety_result_t *check);
void safety_print_plan(const wipe_target_t *target,
                       const overwrite_config_t *cfg);

#endif

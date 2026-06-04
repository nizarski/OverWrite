#ifndef OVERWRITE_PROGRESS_H
#define OVERWRITE_PROGRESS_H

#include "wipe.h"

typedef struct progress_bar progress_bar_t;

progress_bar_t *progress_create(bool enabled);
void progress_update(progress_bar_t *pb, const wipe_status_t *st);
void progress_destroy(progress_bar_t *pb);

#endif

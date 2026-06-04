#ifndef OVERWRITE_COMMON_H
#define OVERWRITE_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define OVERWRITE_VERSION "1.0.0"
#define OVERWRITE_DEFAULT_CHUNK (1024u * 1024u)
#define OVERWRITE_MAX_THREADS 16
#define OVERWRITE_PROGRESS_HZ 4

typedef enum {
    TARGET_WHOLE_DISK,
    TARGET_PARTITION,
    TARGET_FILE,
    TARGET_FREE_SPACE,
    TARGET_UNALLOCATED,
    TARGET_RANGE_LIST
} target_kind_t;

typedef enum {
    PROFILE_GHOST,
    PROFILE_CHAMELEON,
    PROFILE_SPECTRUM,
    PROFILE_FLASH_REALIST,
    PROFILE_FILESYSTEM_SHADOW,
    PROFILE_BLOCK_CARTOGRAPHER,
    PROFILE_SLACK_HUNTER
} wipe_profile_t;

typedef enum {
    RNG_TURBO,
    RNG_VAULT,
    RNG_HYBRID,
    RNG_OS_CHUNK
} rng_mode_t;

typedef struct {
    uint64_t offset;
    uint64_t length;
} byte_range_t;

typedef struct {
    byte_range_t *ranges;
    size_t count;
    size_t capacity;
} range_list_t;

typedef struct {
    target_kind_t kind;
    char path[4096];
    wipe_profile_t profile;
    rng_mode_t rng;
    char nonce_hex[65];
    bool dry_run;
    bool force;
    bool quiet;
    bool normalize_meta;
    bool allow_trim;
    bool ssd_secure_erase;
    bool format_after;
    char format_fs[16];
    bool yes;
    int thread_override;
    uint32_t chunk_size;
    int passes;
} overwrite_config_t;

uint64_t ow_min_u64(uint64_t a, uint64_t b);
uint64_t ow_align_down(uint64_t value, uint64_t alignment);
uint64_t ow_align_up(uint64_t value, uint64_t alignment);
bool ow_is_power_of_two(uint64_t value);
int ow_cpu_count(void);
int ow_thread_count(int thread_override);
char *ow_strdup(const char *s);
int ow_parse_u64(const char *s, uint64_t *out);
int ow_parse_size(const char *s, uint64_t *out);

#endif

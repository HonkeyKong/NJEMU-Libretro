/* -------------------------------------------------------------------------
    Optional (non-standard) extension API for enhanced frontends.
    Author: Ryan Souders (ryan@deadsignallabs.com)

    C port of the libretro-mame libretro_ext interface. The structures and
    function table below are ABI-identical to libretro-mame's
    libretro_ext.h (main API ABI 5, rollback API ABI 1), so a frontend can
    drive either core through the same code.

    This is NOT part of the libretro API and is safe for other frontends:
    - If a frontend doesn't know about it, it will never call it.
    - The core remains a valid libretro core.

    ABI rules:
    - C ABI exports.
    - Versioned, fixed-size struct.
    - Functions must be safe to call when no machine is running.
-------------------------------------------------------------------------- */

#ifndef NJEMU_LIBRETRO_EXT_H
#define NJEMU_LIBRETRO_EXT_H

#include <stdbool.h>
#include <stdint.h>
#include "libretro.h"

#if defined(_WIN32)
  #define LIBRETRO_EXT_EXPORT __declspec(dllexport)
#else
  #define LIBRETRO_EXT_EXPORT __attribute__((visibility("default")))
#endif

enum libretro_ext_watch_access
{
    LIBRETRO_EXT_WATCH_READ  = 1,
    LIBRETRO_EXT_WATCH_WRITE = 2
};

struct libretro_ext_watch_hit
{
    bool hit;
    char cpuTag[64];
    uint64_t address;
    uint64_t pc;
    uint64_t frame;
    uint64_t totalCycles;
    uint32_t value;
    uint8_t access;
    uint8_t width;
    uint32_t historyCount;
    uint64_t pcHistory[256];
};

enum libretro_ext_cpu_state_id
{
    LIBRETRO_EXT_STATE_GENPC = -1,
    LIBRETRO_EXT_STATE_GENPCBASE = -2,
    LIBRETRO_EXT_STATE_GENFLAGS = -3
};

/* DIP switch descriptor structs (versioned, fixed-size, C ABI safe) */
#define LIBRETRO_EXT_DIP_NAME_LEN      128
#define LIBRETRO_EXT_DIP_PORT_TAG_LEN   64
#define LIBRETRO_EXT_DIP_MAX_SETTINGS   64

/* One named setting (e.g. "Normal" = 0x04) within a DIP switch field. */
struct libretro_ext_dip_setting
{
    char     name[LIBRETRO_EXT_DIP_NAME_LEN]; /* human-readable label */
    uint32_t value;                           /* raw masked value for this setting */
};

/* Complete descriptor for one logical DIP switch field. */
struct libretro_ext_dip_info
{
    char     name[LIBRETRO_EXT_DIP_NAME_LEN];         /* e.g. "Difficulty" */
    char     port_tag[LIBRETRO_EXT_DIP_PORT_TAG_LEN]; /* e.g. ":DSWB" */
    uint32_t mask;                                    /* bit mask within the port */
    uint32_t current_value;                           /* current value (within mask) */
    uint32_t default_value;                           /* default value (within mask) */
    uint32_t setting_count;                           /* number of valid entries in settings[] */
    struct libretro_ext_dip_setting settings[LIBRETRO_EXT_DIP_MAX_SETTINGS];
};

/* Immutable/static region metadata for rollback-aware frontends. */
struct libretro_ext_static_region_info
{
    char name[128];
    char region_tag[64];
    char cpu_tag[64];
    char space[32];
    uint64_t offset;
    uint64_t size;
    uint32_t flags;
};

enum
{
    LIBRETRO_EXT_STATIC_REGION_IMMUTABLE_AFTER_STARTUP = 1U << 0,
    LIBRETRO_EXT_STATIC_REGION_SAFE_ROLLBACK_BASELINE  = 1U << 1,
    LIBRETRO_EXT_STATIC_REGION_OPTIONAL_HASH_IDENTITY  = 1U << 2
};

/* Shared-memory import/export metadata (mutable RAM / save data). */
struct libretro_ext_share_info
{
    char name[128];
    uint64_t size;
    uint32_t flags;
};

enum
{
    LIBRETRO_EXT_SHARE_IMMUTABLE_AFTER_STARTUP = 1U << 0,
    LIBRETRO_EXT_SHARE_SAFE_ROLLBACK_BASELINE  = 1U << 1,
    LIBRETRO_EXT_SHARE_EXPORTABLE              = 1U << 2,
    LIBRETRO_EXT_SHARE_IMPORTABLE              = 1U << 3
};

enum
{
    RETRO_EXT_ROLLBACK_FIXED_SIZE           = 1U << 0,
    RETRO_EXT_ROLLBACK_DETERMINISTIC_LAYOUT = 1U << 1,
    RETRO_EXT_ROLLBACK_CORE_DELTA_SUPPORTED = 1U << 2,
    RETRO_EXT_ROLLBACK_XOR_DELTA            = 1U << 3,
    RETRO_EXT_ROLLBACK_IN_PLACE_DELTA_APPLY = 1U << 4,
    RETRO_EXT_ROLLBACK_EXCLUDES_STATIC_DATA = 1U << 5
};

struct retro_ext_rollback_info
{
    uint32_t interface_version;
    uint32_t flags;
    uint32_t state_format_version;
    uint32_t delta_format_version;
    uint64_t compatibility_id;
    uint64_t state_size;
    uint32_t preferred_block_size;
    uint32_t maximum_delta_size;
};

struct libretro_ext_rollback_api
{
    uint32_t abi_version;
    uint32_t sizeof_struct;
    bool (*get_info)(struct retro_ext_rollback_info *info);
    uint64_t (*get_state_size)(void);
    bool (*serialize)(void *destination, uint64_t destination_size, uint64_t frame_number);
    bool (*unserialize)(const void *source, uint64_t source_size);
    uint64_t (*get_delta_max_size)(void);
    bool (*create_delta)(const void *from_state, uint64_t from_state_size, uint64_t from_frame,
                         const void *to_state, uint64_t to_state_size, uint64_t to_frame,
                         void *delta_destination, uint64_t delta_capacity, uint64_t *delta_size);
    bool (*apply_delta)(void *state_in_out, uint64_t state_size,
                        const void *delta, uint64_t delta_size,
                        uint64_t expected_from_frame, uint64_t *resulting_frame);
    bool (*rollback_self_test)(void);
    void (*set_diagnostics_enabled)(bool enabled);
    bool (*get_diagnostics_enabled)(void);
};

struct libretro_ext_api
{
    /* ABI version tracks the fixed v5 base layout. New optional fields are appended
       and must be discovered via sizeof_struct and null checks by frontends. */
    uint32_t abi_version;
    uint32_t sizeof_struct;

    const char *(*get_driver_name)(void);
    int         (*get_cpu_count)(void);
    const char *(*get_cpu_tag)(int cpu_index);
    uint64_t    (*get_cpu_pc)(int cpu_index);

    uint8_t  (*read_u8)(const char *cpu_tag, const char *space, uint64_t addr);
    uint16_t (*read_u16)(const char *cpu_tag, const char *space, uint64_t addr);
    uint32_t (*read_u32)(const char *cpu_tag, const char *space, uint64_t addr);

    void (*write_u8)(const char *cpu_tag, const char *space, uint64_t addr, uint8_t v);
    void (*write_u16)(const char *cpu_tag, const char *space, uint64_t addr, uint16_t v);
    void (*write_u32)(const char *cpu_tag, const char *space, uint64_t addr, uint32_t v);

    int         (*get_region_count)(void);
    const char *(*get_region_tag)(int index);
    uint64_t    (*get_region_size)(const char *region_tag);

    uint64_t (*read_region)(const char *region_tag, uint64_t offset, void *dst, uint64_t bytes);
    uint64_t (*write_region)(const char *region_tag, uint64_t offset, const void *src, uint64_t bytes);

    uint64_t (*get_frame_number)(void);
    uint64_t (*get_time_attoseconds)(void);
    uint64_t (*get_cpu_total_cycles_by_tag)(const char *cpu_tag);

    void (*clear_watch_rules)(void);
    void (*add_watch_rule)(const char *cpuTag, uint64_t start, uint64_t end, uint8_t access, uint8_t width);
    bool (*get_last_watch_hit)(struct libretro_ext_watch_hit *outHit);
    void (*clear_last_watch_hit)(void);
    void (*check_watch_hit)(const char *cpuTag, uint64_t pc, uint64_t address, uint32_t value, uint8_t access, uint8_t width, uint64_t totalCycles);

    int  (*get_dip_count)(void);
    bool (*get_dip_info)(int index, struct libretro_ext_dip_info *out);
    bool (*set_dip_value)(int index, uint32_t value);

    void (*set_debug_extensions_enabled)(bool enabled);
    bool (*get_debug_extensions_enabled)(void);

    /* Restart the current driver's timing capture window if supported. */
    bool (*trigger_timing_capture)(void);

    /* Extended CPU accessors - added after v5 base; check sizeof_struct before using. */
    uint64_t (*get_cpu_pc_by_tag)(const char *cpu_tag);
    bool     (*read_cpu_state_u64_by_index)(int cpu_index, int state_id, uint64_t *out_value);
    bool     (*read_cpu_state_u64_by_tag)(const char *cpu_tag, int state_id, uint64_t *out_value);
    bool     (*read_cpu_register_by_tag)(const char *cpu_tag, const char *reg_name, uint64_t *out_value);

    bool     (*write_cpu_state_u64_by_index)(int cpu_index, int state_id, uint64_t value);
    bool     (*write_cpu_state_u64_by_tag)(const char *cpu_tag, int state_id, uint64_t value);
    bool     (*write_cpu_register_by_tag)(const char *cpu_tag, const char *reg_name, uint64_t value);

    /* Pre-read inject rules - tail fields appended after ABI v5 base. */
    void (*add_inject_rule)(const char *cpuTag, uint64_t start, uint64_t end,
                            uint32_t value, uint8_t width, bool oneShot);
    void (*clear_inject_rules)(void);
    void (*add_inject_rule_ex)(const char *cpuTag, uint64_t start, uint64_t end,
                               uint32_t value, uint8_t width, bool oneShot,
                               bool hasMatchValue, uint32_t matchValue, uint32_t matchMask);

    int  (*get_static_region_count)(void);
    bool (*get_static_region_info)(int index, struct libretro_ext_static_region_info *out);

    int         (*get_share_count)(void);
    const char *(*get_share_tag)(int index);
    uint64_t    (*get_share_size)(const char *share_tag);
    uint32_t    (*get_share_flags)(const char *share_tag);
    bool        (*get_share_info)(int index, struct libretro_ext_share_info *out);
    uint64_t    (*read_share)(const char *share_tag, uint64_t offset, void *dst, uint64_t bytes);
    uint64_t    (*write_share)(const char *share_tag, uint64_t offset, const void *src, uint64_t bytes);
    uint64_t    (*get_rollback_serialize_size)(void);
    bool        (*rollback_serialize)(void *data, uint64_t size);
    bool        (*rollback_unserialize)(const void *data, uint64_t size);
    bool        (*rollback_self_test)(void);
};

LIBRETRO_EXT_EXPORT const struct libretro_ext_api *libretro_ext_get_api(void);
LIBRETRO_EXT_EXPORT const struct libretro_ext_api *libretro_ext_get_api_v5(void);
LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_abi_version(void);
LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_struct_size(void);
LIBRETRO_EXT_EXPORT const struct libretro_ext_rollback_api *libretro_ext_get_rollback_api(void);
LIBRETRO_EXT_EXPORT const struct libretro_ext_rollback_api *libretro_ext_get_rollback_api_v1(void);
LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_rollback_api_abi_version(void);
LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_rollback_api_struct_size(void);

/* Core-internal hooks (not exported). */
void lr_ext_reset(void);
void lr_ext_frame(void);
void lr_ext_apply_dip_overrides(void);
void lr_ext_set_memory_maps(retro_environment_t environ_cb);

#endif /* NJEMU_LIBRETRO_EXT_H */

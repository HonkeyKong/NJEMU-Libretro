#include "emumain.h"
#include "libretro.h"
#include <errno.h>
#include <ctype.h>
#ifdef _WIN32
#include <direct.h>
#define lr_mkdir(p) _mkdir(p)
#else
#define lr_mkdir(p) mkdir(p, 0777)
#endif
static retro_environment_t environment;
static retro_video_refresh_t video;
static retro_audio_sample_t audio;
static retro_audio_sample_batch_t audio_batch;
static retro_input_poll_t input_poll;
static retro_input_state_t input_state;
static retro_log_printf_t logger;
static int loaded, initialized;
static unsigned devices[4] = {RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
static int16_t pads[4][16];
static char system_root[PATH_MAX], save_root[PATH_MAX], content_path[PATH_MAX];
const char *lr_content_archive(void) { return content_path; }
unsigned lr_width = 384, lr_height = 224;
double lr_fps = FPS;
static struct retro_input_descriptor descriptors[4 * 16 + 1];
static char dip_options[3][2048];
#if EMU_SYSTEM == MVS
static char bios_options[2048];
#endif
static struct retro_variable options[9];

void lr_log(int level, const char *fmt, ...)
{
    char message[4096]; va_list ap;
    va_start(ap, fmt); vsnprintf(message, sizeof(message), fmt, ap); va_end(ap);
    if (logger) logger((enum retro_log_level)level, "%s", message);
    else fputs(message, stderr);
    if (level == RETRO_LOG_ERROR && environment) {
        struct retro_message msg = {message, 300};
        environment(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
}
static bool path_join(char *out, size_t size, const char *root, const char *leaf)
{
    int n = snprintf(out, size, "%s%s%s", root, root[0] && root[strlen(root)-1] != '/' ? "/" : "", leaf);
    return n >= 0 && (size_t)n < size;
}
const char *lr_system_path(const char *relative)
{
    static char path[PATH_MAX];
    if (!path_join(path, sizeof(path), system_root, relative)) path[0] = 0;
    return path;
}
const char *lr_save_path(const char *relative)
{
    static char path[PATH_MAX];
    if (!path_join(path, sizeof(path), save_root, relative)) path[0] = 0;
    return path;
}
static const char *variable(const char *key, const char *fallback)
{
    struct retro_variable v = {key, NULL};
    return environment && environment(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value ? v.value : fallback;
}
#if EMU_SYSTEM == MVS
const char *lr_bios_choice(void) { return variable("njemu_bios", "auto"); }
#endif
void lr_options(void)
{
    option_vsync = option_speedlimit = option_autoframeskip = option_frameskip = option_showfps = 0;
    option_sound_enable = 1; option_samplerate = 2; option_sound_volume = 10;
#if EMU_SYSTEM == CPS1
    cps1_dipswitch[0] = strtoul(variable("njemu_dip_a", "ff"), NULL, 16) & 255;
    cps1_dipswitch[1] = strtoul(variable("njemu_dip_b", "ff"), NULL, 16) & 255;
    cps1_dipswitch[2] = strtoul(variable("njemu_dip_c", "ff"), NULL, 16) & 255;
    cps_raster_enable = 1;
#endif
#if EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2
    cps_rotate_screen = 0;
#endif
#if EMU_SYSTEM == MVS
    neogeo_region = atoi(variable("njemu_region", "0"));
    neogeo_machine_mode = atoi(variable("njemu_mode", "0"));
    neogeo_raster_enable = 1;
    neogeo_dipswitch = strtoul(variable("njemu_dip_a", "ff"), NULL, 16) & 255;
#endif
#if EMU_SYSTEM == NCDZ
    neogeo_region = atoi(variable("njemu_cd_region", "1"));
    /* Loading artwork executes recursive CPU frames in the legacy loader.
     * Fast data loading retains the actual file/mapping behavior. */
    neogeo_loadscreen = neogeo_cdspeed_limit = 0;
    neogeo_boot_bios = 0; neogeo_raster_enable = 1;
    option_mp3_enable = strcmp(variable("njemu_cdda", "enabled"), "disabled") != 0;
    option_mp3_volume = 10;
#endif
}
static void register_options(void)
{
    unsigned n = 0;
#if EMU_SYSTEM == CPS1 || EMU_SYSTEM == MVS
    static const char *keys[] = {"njemu_dip_a", "njemu_dip_b", "njemu_dip_c"};
    unsigned banks = EMU_SYSTEM == CPS1 ? 3 : 1;
    for (unsigned b = 0; b < banks; ++b) {
        size_t pos = snprintf(dip_options[b], sizeof(dip_options[b]), "DIP bank %c (hex; see game manual); ff", 'A'+b);
        for (unsigned v = 0; v < 255; ++v) pos += snprintf(dip_options[b]+pos, sizeof(dip_options[b])-pos, "|%02x", v);
        options[n++] = (struct retro_variable){keys[b], dip_options[b]};
    }
#endif
#if EMU_SYSTEM == MVS
    size_t pos = snprintf(bios_options, sizeof(bios_options), "BIOS (reload content); auto");
    for (unsigned i = 0; i < BIOS_MAX; ++i)
        pos += snprintf(bios_options + pos, sizeof(bios_options) - pos, "|%s", bios_name[i]);
    options[n++] = (struct retro_variable){"njemu_bios", bios_options};
    options[n++] = (struct retro_variable){"njemu_region", "Region (reset required); 0|1|2|3"};
    options[n++] = (struct retro_variable){"njemu_mode", "Machine (0=BIOS, 1=AES, 2=MVS; reset required); 0|1|2"};
#endif
#if EMU_SYSTEM == NCDZ
    options[n++] = (struct retro_variable){"njemu_cd_region", "Region (0=Japan, 1=USA, 2=Europe; reset required); 1|0|2"};
    options[n++] = (struct retro_variable){"njemu_cdda", "CDDA; enabled|disabled"};
#endif
    options[n] = (struct retro_variable){NULL, NULL};
    if (environment) environment(RETRO_ENVIRONMENT_SET_VARIABLES, options);
}
static void describe_inputs(void)
{
    static const unsigned directions[] = {RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT};
    static const char *direction_names[] = {"Up", "Down", "Left", "Right"};
#if EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2
    static const unsigned buttons[] = {RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_R};
    static const char *button_names[] = {"Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6"};
    unsigned players = loaded ? input_max_players : 2;
    unsigned count = loaded ? input_max_buttons : 6;
#else
    static const unsigned buttons[] = {RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X};
    static const char *button_names[] = {"A", "B", "C", "D"};
    unsigned players = 2, count = 4;
#endif
    unsigned n = 0;
    if (players > 4) players = 4;
    for (unsigned p = 0; p < players; ++p) {
#define DESCRIBE(id, name) descriptors[n++] = (struct retro_input_descriptor){p, RETRO_DEVICE_JOYPAD, 0, id, name}
        for (unsigned i = 0; i < 4; ++i) { DESCRIBE(directions[i], direction_names[i]); }
        for (unsigned i = 0; i < count; ++i) { DESCRIBE(buttons[i], button_names[i]); }
        DESCRIBE(RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin / Select");
        DESCRIBE(RETRO_DEVICE_ID_JOYPAD_START, "Start");
#if EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2
        DESCRIBE(RETRO_DEVICE_ID_JOYPAD_L2, "Dial left");
        DESCRIBE(RETRO_DEVICE_ID_JOYPAD_R2, "Dial right");
#endif
#if EMU_SYSTEM != NCDZ
        if (!p) {
            DESCRIBE(RETRO_DEVICE_ID_JOYPAD_L3, "Service coin");
            DESCRIBE(RETRO_DEVICE_ID_JOYPAD_R3, "Test / Service");
        }
#endif
#undef DESCRIBE
    }
    descriptors[n] = (struct retro_input_descriptor){0,0,0,0,NULL};
    if (environment) environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors);
}

int16_t lr_input(unsigned port, unsigned id) { return port < 4 && id < 16 ? pads[port][id] : 0; }
void lr_audio_submit(const int16_t *samples, unsigned frames)
{
    if (audio_batch) {
        /* A short write drops the unaccepted tail. Never grow a backlog or spin. */
        audio_batch(samples, frames);
    } else if (audio) for (unsigned i = 0; i < frames; ++i) audio(samples[2*i], samples[2*i+1]);
}
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environment = cb;
    bool no_game = false;
    if (cb) cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    register_options();
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state = cb; }
RETRO_API void retro_init(void)
{
    struct retro_log_callback log = {0};
    if (initialized) return;
    if (environment && environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) logger = log.log;
    ui_text_data = ui_text_driver->init();
    if (!ui_text_data) { lr_log(3, "Cannot allocate UI text table.\n"); return; }
    sound_thread_init(); initialized = 1;
    describe_inputs();
}
RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "NJEMU " TARGET_STR;
    info->library_version = "2.4-libretro";
#if EMU_SYSTEM == NCDZ
    info->valid_extensions = "zip|txt";
#else
    info->valid_extensions = "zip";
#endif
    info->need_fullpath = true; info->block_extract = true;
}
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
#if EMU_SYSTEM == MVS || EMU_SYSTEM == NCDZ
    lr_width = 320;
#endif
    info->geometry.base_width = lr_width; info->geometry.base_height = lr_height;
    info->geometry.max_width = 384; info->geometry.max_height = 224;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = lr_fps; info->timing.sample_rate = lr_audio_rate;
}
static bool setup_paths(const char *content)
{
    const char *system = NULL, *save = NULL;
    char root[PATH_MAX], dir[PATH_MAX];
    if (!content || !*content || strlen(content) > PATH_MAX - 512) return false;
    /* Full paths avoid CWD changes and ambiguity during parent/clone lookup. */
    if (content[0] != '/' && !(isalpha((unsigned char)content[0]) && content[1] == ':' && (content[2] == '/' || content[2] == '\\'))) {
        lr_log(3, "Content requires an absolute path.\n"); return false;
    }
    snprintf(content_path, sizeof(content_path), "%s", content);
#ifdef _WIN32
    for (char *p = content_path; *p; ++p) if (*p == '\\') *p = '/';
#endif
    content = content_path;
    strcpy(dir, content);
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    for (char *p = dir; *p; ++p) if (*p == '\\') *p = '/';
    slash = strrchr(dir, '/');
#endif
    if (!slash) return false;
    *slash = 0;
    if (!*dir) strcpy(dir, "/");
    const char *base = strrchr(content, '/'); base = base ? base+1 : content;
    char stem[PATH_MAX]; snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (!dot) return false;
#if EMU_SYSTEM == NCDZ
    if (strcasecmp(dot, ".zip") && strcasecmp(base, "IPL.TXT")) return false;
    if (!strcasecmp(base, "IPL.TXT")) snprintf(game_dir, sizeof(game_dir), "%s", dir);
    else snprintf(game_dir, sizeof(game_dir), "%s", content);
    snprintf(mp3_dir, sizeof(mp3_dir), "%s/mp3", dir);
#else
    if (strcasecmp(dot, ".zip")) return false;
    snprintf(game_dir, sizeof(game_dir), "%s", dir);
#endif
    *dot = 0;
#if EMU_SYSTEM == NCDZ
    strcpy(game_name, "neocd"); /* The media loader identifies/renames the game. */
#else
    if (strlen(stem) >= sizeof(game_name)) return false;
    for (char *p = stem; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '_') return false;
    for (char *p = stem; *p; ++p) *p = tolower((unsigned char)*p);
    strcpy(game_name, stem);
#endif
    parent_name[0] = 0;
    if (!environment || !environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system) || !system || !*system ||
        !environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) || !save || !*save) {
        lr_log(3, "Frontend must supply system and save directories.\n"); return false;
    }
    if (strlen(system) > PATH_MAX - 256 || strlen(save) > PATH_MAX - 256) return false;
    if (!path_join(system_root, sizeof(system_root), system, "njemu") ||
        !path_join(root, sizeof(root), save, "njemu")) return false;
    if (lr_mkdir(root) && errno != EEXIST) return false;
    if (!path_join(save_root, sizeof(save_root), root, TARGET_STR)) return false;
    if (lr_mkdir(save_root) && errno != EEXIST) return false;
    snprintf(launchDir, sizeof(launchDir), "%s/", save_root);
    const char *folders[] = {"nvram", "memcard"
#if (EMU_SYSTEM != CPS2) || USE_CACHE
        , "cache"
#endif
    };
    for (unsigned i = 0; i < sizeof(folders) / sizeof(folders[0]); ++i)
        if (lr_mkdir(lr_save_path(folders[i])) && errno != EEXIST) return false;
#if USE_CACHE
    struct stat st;
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", dir);
    if (stat(cache_dir, &st) || !S_ISDIR(st.st_mode)) snprintf(cache_dir, sizeof(cache_dir), "%s", lr_save_path("cache"));
    cache_parent_name[0] = 0;
#endif
    return true;
}
RETRO_API void retro_unload_game(void)
{
    lr_machine_unload(); loaded = 0;
    if (video_data) { video_driver->free(video_data); video_data = NULL; }
    memset(pads, 0, sizeof(pads));
}
RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    retro_unload_game();
    if (!initialized) retro_init();
    if (!initialized) return false;
    if (!game || !setup_paths(game->path)) { lr_log(3, "Invalid content path or missing frontend directories.\n"); return false; }
    struct stat st;
    if (stat(game->path, &st) || !S_ISREG(st.st_mode)) { lr_log(3, "Content file not found: %s\n", game->path); return false; }
    if (!environment || !environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) { lr_log(3, "Frontend must support XRGB8888.\n"); return false; }
    const char *ext = strrchr(game->path, '.');
    if (ext && !strcasecmp(ext, ".zip")) {
        if (zip_open(game->path) < 0) { zip_close(); lr_log(3, "Invalid ZIP archive: %s\n", game->path); return false; }
        zip_close();
    }
#if EMU_SYSTEM == MVS
    neogeo_bios = -1; /* Re-evaluate the selected BIOS for each content load. */
#endif
    lr_options(); lr_frame_number = 0; frames_displayed = 0;
    video_data = video_driver->init(emu_layer_textures, emu_layer_textures_count, &emu_clut_info);
    if (!video_data) { lr_log(3, "Cannot allocate video buffers.\n"); return false; }
    if (!lr_machine_load()) { lr_log(3, "Failed to load %s; check ROMs, BIOS, rominfo and caches.\n", game->path); retro_unload_game(); return false; }
    lr_options(); loaded = 1;
    unsigned rotation = 0;
#if EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2
    rotation = machine_screen_type ? 1 : 0;
#else
    lr_width = 320;
#endif
    environment(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);
    describe_inputs();
    struct retro_system_av_info av; retro_get_system_av_info(&av);
    environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
    return true;
}
RETRO_API void retro_reset(void) { if (loaded) { lr_options(); lr_machine_reset(); } }
RETRO_API void retro_run(void)
{
    if (!loaded) return;
    bool changed = false;
    if (environment && environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed) && changed) lr_options();
    if (input_poll) input_poll();
    for (unsigned p = 0; p < 4; ++p) for (unsigned id = 0; id < 16; ++id)
        pads[p][id] = input_state && devices[p] == RETRO_DEVICE_JOYPAD ? input_state(p, RETRO_DEVICE_JOYPAD, 0, id) : 0;
    lr_machine_frame(); ++lr_frame_number;
    lr_video_submit();
    if (video) video(lr_framebuffer, lr_width, lr_height, lr_width * sizeof(uint32_t));
    if (fatal_error && environment) environment(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}
RETRO_API void retro_deinit(void)
{
    retro_unload_game();
    if (ui_text_data) { ui_text_driver->free(ui_text_data); ui_text_data = NULL; }
    sound_thread_exit(); initialized = 0; logger = NULL;
}
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { if (port < 4) devices[port] = device & RETRO_DEVICE_MASK; }
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
/* Existing persistence stays file-backed; there is no competing frontend SRAM. */
RETRO_API void *retro_get_memory_data(unsigned id)
{
    if (!loaded || id != RETRO_MEMORY_SYSTEM_RAM) return NULL;
#if EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2
    return cps1_ram;
#elif EMU_SYSTEM == MVS
    return neogeo_ram;
#else
    return memory_region_cpu1;
#endif
}
RETRO_API size_t retro_get_memory_size(unsigned id)
{
    if (!loaded || id != RETRO_MEMORY_SYSTEM_RAM) return 0;
#if EMU_SYSTEM == NCDZ
    return memory_length_cpu1;
#else
    return 0x10000;
#endif
}
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t count) { (void)type; (void)info; (void)count; return false; }

#include "emumain.h"
#include "libretro.h"
char game_name[16], parent_name[16], game_dir[PATH_MAX];
char launchDir[PATH_MAX], screenshotDir[PATH_MAX];
#if USE_CACHE
char cache_parent_name[16], cache_dir[PATH_MAX];
#endif
bool systembuttons_available;
int option_showfps, option_speedlimit, option_autoframeskip, option_frameskip, option_vsync, option_stretch;
int option_sound_enable = 1, option_samplerate = 2, option_sound_volume = 10;
int machine_driver_type, machine_init_type, machine_input_type, machine_screen_type, machine_sound_type;
uint32_t frames_displayed;
int fatal_error, platform_cpuclock;
volatile int Loop = LOOP_BROWSER, Sleep;
void *platform_data, *power_data, *ticker_data;
uint8_t gulist[GULIST_SIZE];
uint64_t lr_frame_number;
void autoframeskip_reset(void) {} /* frontend pacing; keep cache/CDDA frame stamps monotonic */
uint8_t skip_this_frame(void) { return 0; }
void update_screen(void) {}
void waitVBlank(void) {}
void video_wait_vsync(void) {}
void save_snapshot(void) {}
void show_fatal_error(void) {}
void emu_main(void) {}
void fatalerror(const char *fmt, ...)
{
    char message[1024]; va_list ap;
    va_start(ap, fmt); vsnprintf(message, sizeof(message), fmt, ap); va_end(ap);
    lr_log(RETRO_LOG_ERROR, "%s\n", message);
    fatal_error = 1; Loop = LOOP_BROWSER;
}
static int32_t clock_max(void *p) { (void)p; return 333; }
static void set_clock(void *p, int32_t value) { (void)p; (void)value; }
static power_driver_t power = {.ident="libretro", .getHighestCpuClock=clock_max, .setCpuClock=set_clock};
power_driver_t *power_drivers[] = {&power, NULL};
static uint64_t ticks(void *p) { (void)p; return (uint64_t)(lr_frame_number * 1000000.0 / lr_fps); }
static ticker_driver_t ticker = {.ident="libretro", .currentUs=ticks};
ticker_driver_t *ticker_drivers[] = {&ticker, NULL};

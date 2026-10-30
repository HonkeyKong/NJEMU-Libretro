#ifndef NJEMU_LIBRETRO_PLATFORM_H
#define NJEMU_LIBRETRO_PLATFORM_H
#include <stdint.h>
#define SCR_WIDTH 512
#define SCR_HEIGHT 272
#define BUF_WIDTH 512
#define REFRESH_RATE FPS
/* CPU renderer uses NJEMU's native BGR5551 until final conversion. */
void lr_log(int level, const char *fmt, ...);
const char *lr_content_archive(void);
const char *lr_system_path(const char *relative);
const char *lr_save_path(const char *relative);
uint32_t lr_pad(unsigned player);
#if defined(BUILD_MVS)
const char *lr_bios_choice(void);
#endif
void lr_input_flags(uint8_t *flags, unsigned player);
int lr_machine_load(void);
void lr_machine_frame(void);
void lr_machine_reset(void);
void lr_machine_unload(void);
void lr_audio_frame(void);
void lr_audio_reset(void);
void lr_audio_submit(const int16_t *samples, unsigned frames);
void lr_video_submit(void);
void lr_options(void);
extern unsigned lr_width, lr_height;
extern uint64_t lr_frame_number;
extern unsigned lr_audio_rate;
extern double lr_fps;
extern int lr_initialized_machine;
extern uint32_t lr_framebuffer[384 * 224];
#if defined(BUILD_NCDZ)
void lr_mp3_mix(int16_t *samples, unsigned frames, unsigned rate);
#endif
#endif

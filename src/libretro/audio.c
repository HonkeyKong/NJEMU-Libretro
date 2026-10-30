#include "emumain.h"
static struct sound_t info;
struct sound_t *sound = &info;
unsigned lr_audio_rate = 44100;
static int enabled;
static double remainder;
static int16_t output[SOUND_BUFFER_SIZE];
void sound_thread_init(void) { memset(&info, 0, sizeof(info)); enabled = 1; remainder = 0; }
void sound_thread_exit(void) { enabled = 0; }
int sound_thread_start(void) { lr_audio_rate = sound->frequency; enabled = 1; remainder = 0; return 1; }
void sound_thread_stop(void) { enabled = 0; }
void sound_thread_set_volume(void) {}
void sound_thread_enable(int on) { enabled = on; }
void lr_audio_reset(void) { remainder = 0; }
/* sndintrf's legacy thread renders blocks spanning ~two video frames. The
 * libretro branch renders the exact (fractional) one-frame chip sample count. */
void lr_audio_frame(void)
{
    remainder += (double)lr_audio_rate / lr_fps;
    unsigned frames = (unsigned)remainder;
    remainder -= frames;
    if (frames * 2 > SOUND_BUFFER_SIZE) { fatalerror("Audio buffer overflow"); return; }
    sound->samples = frames;
    extern void lr_sound_update(int16_t *, unsigned);
    lr_sound_update(output, frames);
    if (!enabled || !option_sound_enable) memset(output, 0, frames * 2 * sizeof(int16_t));
#if EMU_SYSTEM == NCDZ
    lr_mp3_mix(output, frames, lr_audio_rate);
#endif
    lr_audio_submit(output, frames);
}

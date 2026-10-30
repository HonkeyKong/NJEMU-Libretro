/* minimp3 is CC0 (see minimp3.h). CDDA decoding shares the emulation thread. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"
#include "emumain.h"
char mp3_dir[PATH_MAX];
int option_mp3_enable = 1, option_mp3_volume = 10;
static FILE *track;
static mp3dec_t decoder;
static uint8_t compressed[16384];
static size_t used;
static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static unsigned pos, count, channels, rate, frame;
static double phase;
static int status, loop_has_samples;
static char track_name[PATH_MAX];
static unsigned seek_frame;
static int decode(void)
{
    mp3dec_frame_info_t info;
    while (track) {
        if (used < sizeof(compressed)) used += fread(compressed + used, 1, sizeof(compressed) - used, track);
        if (!used) {
            if (cdda_autoloop && loop_has_samples) { rewind(track); mp3dec_init(&decoder); loop_has_samples = 0; continue; }
            mp3_stop(); return 0;
        }
        int samples = mp3dec_decode_frame(&decoder, compressed, used, pcm, &info);
        unsigned consumed = info.frame_bytes > 0 ? (unsigned)info.frame_bytes : (unsigned)used;
        if (consumed > used) { mp3_stop(); return 0; }
        used -= consumed;
        memmove(compressed, compressed + consumed, used);
        if (samples > 0) {
            loop_has_samples = 1; count = samples; pos = 0; channels = info.channels; rate = info.hz; ++frame;
            return 1;
        }
    }
    return 0;
}
int mp3_thread_start(void) { status = MP3_STOP; cdda_command_ack = 1; return 1; }
void mp3_stop(void) { if (track) fclose(track); track = NULL; used = pos = count = 0; phase = 0; status = MP3_STOP; }
void mp3_thread_stop(void) { mp3_stop(); }
void mp3_set_volume(void) {}
int mp3_play(const char *name)
{
    mp3_stop();
    cdda_command_ack = 1; /* Opening/command acceptance is synchronous. */
    loop_has_samples = 0;
    if (!name || strlen(name) >= sizeof(track_name)) return 0;
    strcpy(track_name, name);
    track = fopen(name, "rb");
    if (!track) { lr_log(2, "CDDA track missing: %s\n", name); return 0; }
    frame = 0; mp3dec_init(&decoder); status = MP3_PLAY;
    return 1;
}
void mp3_pause(int pause) { if (track) status = pause ? MP3_PAUSE : MP3_PLAY; }
void mp3_seek_set(const char *name, uint32_t target) { if (name) snprintf(track_name, sizeof(track_name), "%s", name); seek_frame = target; }
void mp3_seek_start(void)
{
    char name[PATH_MAX]; strcpy(name, track_name);
    if (mp3_play(name)) while (frame < seek_frame && decode()) pos = count;
}
uint32_t mp3_get_current_frame(void) { return frame; }
int mp3_get_status(void) { return status; }
void lr_mp3_mix(int16_t *out, unsigned frames, unsigned output_rate)
{
    if (!option_mp3_enable || status != MP3_PLAY) return;
    for (unsigned i = 0; i < frames; ++i) {
        if (pos >= count && !decode()) break;
        for (unsigned c = 0; c < 2; ++c) {
            int value = out[2*i+c] + pcm[pos*channels + (channels == 2 ? c : 0)] * option_mp3_volume / 10;
            Limit(value, 32767, -32768); out[2*i+c] = value;
        }
        phase += (double)rate / output_rate;
        while (phase >= 1.0) {
            phase -= 1.0;
            if (++pos >= count && !decode()) return;
        }
    }
}

#undef NDEBUG
#include "emumain.h"
#include <assert.h>
volatile int cdda_autoloop, cdda_command_ack;
void lr_log(int level, const char *fmt, ...) { (void)level; (void)fmt; }
static int16_t samples[16384];
int main(int argc, char **argv)
{
    assert(argc==2 && mp3_thread_start());
    cdda_command_ack=0;
    assert(mp3_play(argv[1]) && cdda_command_ack);
    lr_mp3_mix(samples,8192,44100);
    unsigned energy=0;
    for (unsigned i=0;i<16384;++i) energy+=samples[i]!=0;
    assert(energy>1000 && mp3_get_status()==MP3_STOP);
    assert(mp3_play(argv[1])); mp3_pause(1);
    memset(samples,0,sizeof(samples)); lr_mp3_mix(samples,4096,24000);
    for (unsigned i=0;i<8192;++i) assert(samples[i]==0);
    mp3_pause(0); cdda_autoloop=1;
    lr_mp3_mix(samples,8192,24000); assert(mp3_get_status()==MP3_PLAY);
    mp3_thread_stop(); mp3_thread_stop();
    cdda_command_ack=0; assert(!mp3_play("/nonexistent/track.mp3") && cdda_command_ack);
    /* A looping empty/invalid track must stop instead of spinning forever. */
    char empty[]="/tmp/njemu-empty-track-XXXXXX";
    int fd=mkstemp(empty); assert(fd>=0); close(fd);
    assert(mp3_play(empty)); lr_mp3_mix(samples,8192,24000);
    assert(mp3_get_status()==MP3_STOP); unlink(empty);
    return 0;
}

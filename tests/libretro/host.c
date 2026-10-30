#undef NDEBUG
#define _XOPEN_SOURCE 700
#include "libretro.h"
#include <assert.h>
#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
static char system_dir[4096], save_dir[4096];
static unsigned videos, polls, rotation, messages, batches;
static size_t samples;
static int reject_pixels, short_audio;
static struct retro_system_av_info av;
static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_WARN && !getenv("NJEMU_TEST_LOG")) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
static bool env(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log=log_cb; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data=system_dir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data=save_dir; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: assert(*(enum retro_pixel_format *)data==RETRO_PIXEL_FORMAT_XRGB8888); return !reject_pixels;
    case RETRO_ENVIRONMENT_SET_ROTATION: rotation=*(unsigned *)data; return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: av=*(struct retro_system_av_info *)data; return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: ++messages; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data=false; return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
        const struct retro_input_descriptor *p=data;
        unsigned n=0; while (p[n].description) { assert(p[n].port < 4); assert(++n <= 64); } return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES: return true;
    case RETRO_ENVIRONMENT_SHUTDOWN: fprintf(stderr,"Unexpected core shutdown\n"); abort();
    default: return false;
    }
}
static void vid(const void *data, unsigned w, unsigned h, size_t pitch)
{
    assert(data); assert(w==320 || w==384); assert(h==224); assert(pitch==w*4);
    const uint32_t *p=data;
    for (unsigned i=0; i<w*h; ++i) assert(!(p[i]&0xff000000));
    ++videos;
}
static size_t batch(const int16_t *data,size_t frames)
{
    assert(data); assert(frames>300 && frames<900); samples+=frames; ++batches;
    return short_audio ? frames/2 : frames;
}
static void poll(void) { ++polls; }
static int16_t state(unsigned p,unsigned device,unsigned index,unsigned id)
{ assert(p<4); assert(device==RETRO_DEVICE_JOYPAD && index==0 && id<16); return p==1 && id==RETRO_DEVICE_ID_JOYPAD_START; }
/* Empty stored member in a real ZIP: exercise machine allocation/load cleanup. */
static void missing_rom_zip(const char *path)
{
    uint8_t zip[112]={0};
    const uint8_t local[]={0x50,0x4b,3,4,20,0}; memcpy(zip,local,sizeof(local));
    zip[26]=7; memcpy(zip+30,"missing",7);
    const uint8_t central[]={0x50,0x4b,1,2,20,0,20,0}; memcpy(zip+37,central,sizeof(central));
    zip[37+28]=7; memcpy(zip+37+46,"missing",7);
    const uint8_t end[]={0x50,0x4b,5,6}; memcpy(zip+90,end,4);
    zip[90+8]=zip[90+10]=1; zip[90+12]=53; zip[90+16]=37;
    FILE *fp=fopen(path,"wb"); assert(fp); assert(fwrite(zip,1,sizeof(zip),fp)==sizeof(zip)); fclose(fp);
}
static unsigned count_fds(void)
{
    DIR *d=opendir("/proc/self/fd");
    if (!d) return 0;
    unsigned n=0; struct dirent *entry;
    while ((entry=readdir(d))) if (entry->d_name[0]!='.') ++n;
    closedir(d); return n;
}
#define FN(ret, name, args) ret (*name) args; *(void **)(&name)=dlsym(handle,#name); assert(name)
int main(int argc, char **argv)
{
    assert(argc>=3);
    char temp[]="/tmp/njemu-host-XXXXXX"; assert(mkdtemp(temp));
    snprintf(system_dir,sizeof(system_dir),"%s/system",temp); assert(!mkdir(system_dir,0700));
    snprintf(save_dir,sizeof(save_dir),"%s/save",temp); assert(!mkdir(save_dir,0700));
    char resources[4096]; assert(realpath(argv[2],resources));
    char link[4096]; snprintf(link,sizeof(link),"%s/njemu",system_dir); assert(!symlink(resources,link));
    void *handle=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if (!handle) { fprintf(stderr,"dlopen: %s\n",dlerror()); return 1; }
    FN(unsigned,retro_api_version,(void));
    FN(void,retro_set_environment,(retro_environment_t));
    FN(void,retro_set_video_refresh,(retro_video_refresh_t));
    FN(void,retro_set_audio_sample,(retro_audio_sample_t));
    FN(void,retro_set_audio_sample_batch,(retro_audio_sample_batch_t));
    FN(void,retro_set_input_poll,(retro_input_poll_t));
    FN(void,retro_set_input_state,(retro_input_state_t));
    FN(void,retro_init,(void)); FN(void,retro_deinit,(void));
    FN(void,retro_get_system_info,(struct retro_system_info *));
    FN(void,retro_get_system_av_info,(struct retro_system_av_info *));
    FN(bool,retro_load_game,(const struct retro_game_info *));
    FN(void,retro_unload_game,(void)); FN(void,retro_reset,(void)); FN(void,retro_run,(void));
    FN(unsigned,retro_get_region,(void)); FN(void,retro_set_controller_port_device,(unsigned,unsigned));
    FN(size_t,retro_serialize_size,(void)); FN(bool,retro_serialize,(void *,size_t));
    FN(bool,retro_unserialize,(const void *,size_t));
    FN(void *,retro_get_memory_data,(unsigned)); FN(size_t,retro_get_memory_size,(unsigned));
    FN(void,retro_cheat_reset,(void)); FN(void,retro_cheat_set,(unsigned,bool,const char *));
    FN(bool,retro_load_game_special,(unsigned,const struct retro_game_info *,size_t));
    assert(retro_api_version()==1);
    retro_set_environment(env); retro_set_video_refresh(vid); retro_set_audio_sample(NULL);
    retro_set_audio_sample_batch(batch); retro_set_input_poll(poll); retro_set_input_state(state);
    char cwd[4096],after[4096]; assert(getcwd(cwd,sizeof(cwd)));
    retro_deinit(); /* deinit before init and repeated deinit must be safe */
    retro_init(); retro_init();
    unsigned initial_fds=count_fds();
    struct retro_system_info info; retro_get_system_info(&info);
    assert(info.need_fullpath && info.block_extract);
    retro_get_system_av_info(&av); assert(av.timing.fps>59 && av.timing.fps<=60);
    assert(!retro_serialize_size()); char invalid[4]={0};
    assert(!retro_serialize(invalid,sizeof(invalid)) && !retro_unserialize(invalid,sizeof(invalid)));
    assert(!retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) && !retro_get_memory_size(RETRO_MEMORY_SAVE_RAM));
    assert(!retro_load_game_special(0,NULL,0));
    retro_set_controller_port_device(0,RETRO_DEVICE_JOYPAD); retro_cheat_reset(); retro_cheat_set(0,false,"");
    char bad[4096]; snprintf(bad,sizeof(bad),"%s/1941.zip",temp);
    FILE *fp=fopen(bad,"wb"); assert(fp); fputs("corrupt archive",fp); fclose(fp);
    char missing[4096];
    const char *name = strstr(info.library_name,"CPS2") ? "19xx" : strstr(info.library_name,"MVS") ? "mslug" : "1941";
    snprintf(missing,sizeof(missing),"%s/%s.zip",temp,name);
    /* Keep the corrupt archive separate from the supported-name test ZIP. */
    snprintf(bad,sizeof(bad),"%s/corrupt.zip",temp);
    fp=fopen(bad,"wb"); assert(fp); fputs("corrupt archive",fp); fclose(fp);
    missing_rom_zip(missing);
    struct retro_game_info game={bad,NULL,0,NULL};
    for (unsigned i=0;i<4;++i) {
        assert(!retro_load_game(NULL)); retro_unload_game();
        game.path="/nonexistent/njemu.zip"; assert(!retro_load_game(&game));
        game.path=bad; assert(!retro_load_game(&game));
        game.path=missing; assert(!retro_load_game(&game));
        retro_reset(); retro_run(); retro_unload_game();
    }
    reject_pixels=1; assert(!retro_load_game(&game)); reject_pixels=0;
    assert(messages > 0);
    if (argc>4 && !strcmp(argv[4],"reject")) {
        game.path=argv[3];
        for (unsigned i=0;i<4;++i) { assert(!retro_load_game(&game)); retro_unload_game(); }
    } else if (argc>3) {
        game.path=argv[3];
        for (unsigned round=0;round<3;++round) {
            assert(retro_load_game(&game));
            unsigned start_videos=videos, start_polls=polls, start_batches=batches;
            size_t start_samples=samples;
            short_audio=round==1;
            uint16_t *ram=retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
            assert(ram && retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM)>=0x10000);
            uint16_t previous=ram[0];
            for (unsigned i=0;i<180;++i) {
                retro_run();
                /* Both original fixtures increment word ff0000 in a CPU loop. */
                if (i==0) assert(ram[0]!=previous);
            }
            assert(videos-start_videos==180 && polls-start_polls==180 && batches-start_batches==180);
            assert(fabs((double)(samples-start_samples)-180*av.timing.sample_rate/av.timing.fps)<2);
            retro_reset(); retro_run(); retro_unload_game(); retro_unload_game();
        }
    }
    assert(count_fds()==initial_fds);
    retro_deinit(); retro_deinit(); retro_init(); retro_deinit();
    assert(getcwd(after,sizeof(after)) && !strcmp(cwd,after));
    printf("PASS %s: symbols, lifecycle, failure cleanup, cwd; %u frames, %zu audio frames, rotation=%u (test files %s)\n",info.library_name,videos,samples,rotation,temp);
    assert(!dlclose(handle));
    return 0;
}

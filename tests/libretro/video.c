#undef NDEBUG
#include "emumain.h"
#include <assert.h>
unsigned lr_width=384,lr_height=224;
int cps_flip_screen;
int main(void)
{
    layer_texture_info_t layers[]={{512,512,1},{512,512,2}};
    clut_info_t c={0};
    video_data=video_driver->init(layers,2,&c); assert(video_data);
    uint8_t *tex=video_driver->textureLayer(video_data,0);
    uint16_t pal[256]={0}; pal[1]=MAKECOL15(255,0,0); pal[2]=0x8000;
    tex[0]=1; tex[1]=2; tex[2]=1; tex[3]=1;
    struct Vertex v[2]={{.u=0,.v=0,.x=64,.y=16,.z=2},{.u=4,.v=1,.x=68,.y=17,.z=2}};
    video_driver->startWorkFrame(video_data,MAKECOL32(0,0,255));
    video_driver->scissor(video_data,64,16,448,240);
    video_driver->blitTexture(video_data,0,pal,0,2,v);
#if EMU_SYSTEM == MVS || EMU_SYSTEM == NCDZ
    /* This test intentionally draws at the CPS raster origin. */
    lr_width=384;
    v[0].x=16; v[1].x=20;
    video_driver->startWorkFrame(video_data,MAKECOL32(0,0,255));
    video_driver->scissor(video_data,16,16,400,240);
    video_driver->blitTexture(video_data,0,pal,0,2,v);
#endif
    lr_video_submit(); assert(lr_framebuffer[0]==0xff0000); assert(lr_framebuffer[1]==0x0000ff);
    /* Reverse UV, clipped scaled sprite and direct-color high-priority tile. */
    v[0].u=4; v[1].u=0;
    video_driver->blitTexture(video_data,0,pal,0,2,v);
    lr_video_submit(); assert(lr_framebuffer[2]==0xff0000);
    uint16_t *direct=video_driver->textureLayer(video_data,1); direct[0]=MAKECOL15(0,255,0);
    v[0].u=0; v[1].u=1; v[1].x=v[0].x+1;
    video_driver->clearDepthBuffer(video_data); video_driver->enableDepthTest(video_data);
    video_driver->blitTexture(video_data,1,NULL,0,2,v);
    v[0].z=v[1].z=1;
    video_driver->blitTexture(video_data,0,pal,0,2,v);
    lr_video_submit(); assert(lr_framebuffer[0]==0x00ff00);
    video_driver->clearColorBuffer(video_data);
    video_driver->blitTexture(video_data,0,pal,0,2,v);
    lr_video_submit(); assert(lr_framebuffer[0]==0); /* mask remains after color clear */
    video_driver->disableDepthTest(video_data);
    struct PointVertex point={.x=v[0].x,.y=16,.color=MAKECOL15(0,0,255)};
    video_driver->blitPoints(video_data,1,&point);
    lr_video_submit(); assert(lr_framebuffer[0]==0x0000ff);
    video_driver->free(video_data); return 0;
}

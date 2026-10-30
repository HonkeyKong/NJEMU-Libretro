#undef NDEBUG
#include "cps2/cps2.h"
#include <assert.h>

/* The decoder is linked into this test with unrelated machine sections removed. */
uint8_t *memory_region_gfx1;
uint32_t memory_length_gfx1;
uint32_t gfx_total_elements[3];
uint8_t *gfx_pen_usage[3];

int main(void)
{
    memory_length_gfx1 = 0x1000000;
    memory_region_gfx1 = malloc(memory_length_gfx1);
    assert(memory_region_gfx1);
    memset(memory_region_gfx1, 0xff, memory_length_gfx1);
    memset(memory_region_gfx1, 0, 0x200000);
    memset(memory_region_gfx1 + 0x800000, 0, 0x200000);
    /* The first eight-byte word stays at the beginning during unshuffle. */
    memory_region_gfx1[0] = 0x80;
    gfx_total_elements[TILE08] = 0x10000;
    gfx_total_elements[TILE16] = memory_length_gfx1 >> 7;
    gfx_total_elements[TILE32] = (memory_length_gfx1 - 0x800000) >> 9;
    for (int i = 0; i < 3; ++i) {
        gfx_pen_usage[i] = calloc(gfx_total_elements[i], 1);
        assert(gfx_pen_usage[i]);
    }
    cps2_gfx_decode();
    assert(memory_region_gfx1[0] == 1);
    assert(memory_region_gfx1[1] == 0);
    assert(gfx_pen_usage[TILE16][0] == SPRITE_TRANSPARENT);
    assert(gfx_pen_usage[TILE16][1] == SPRITE_OPAQUE);
    assert(gfx_pen_usage[TILE16][0x200000 / 128] == 0);
    assert(gfx_pen_usage[TILE08][0] == SPRITE_OPAQUE);
    assert(gfx_pen_usage[TILE08][0x200000 / 64] == 0);
    assert(gfx_pen_usage[TILE32][0] == SPRITE_OPAQUE);
    assert(gfx_pen_usage[TILE32][0x200000 / 512] == 0);
    for (int i = 0; i < 3; ++i) free(gfx_pen_usage[i]);
    free(memory_region_gfx1);
    return 0;
}

#include "emumain.h"
#include "libretro.h"
extern int16_t lr_input(unsigned port, unsigned id);
uint32_t lr_pad(unsigned player)
{
    static const unsigned ids[] = {RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A,
        RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X,
        RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
        RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START};
    uint32_t value = 0;
    for (unsigned i = 0; i < sizeof(ids)/sizeof(*ids); ++i) if (lr_input(player, ids[i])) value |= 1u << i;
    return value;
}
void lr_input_flags(uint8_t *f, unsigned player)
{
    memset(f, 0, MAX_INPUTS);
#define PRESSED(flag, id) f[flag] = lr_input(player, RETRO_DEVICE_ID_JOYPAD_##id) != 0
    PRESSED(P1_UP, UP); PRESSED(P1_DOWN, DOWN); PRESSED(P1_LEFT, LEFT); PRESSED(P1_RIGHT, RIGHT);
    PRESSED(P1_START, START);
#if EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2
    PRESSED(P1_COIN, SELECT);
    PRESSED(P1_BUTTON1, Y); PRESSED(P1_BUTTON2, X); PRESSED(P1_BUTTON3, L);
    PRESSED(P1_BUTTON4, B); PRESSED(P1_BUTTON5, A); PRESSED(P1_BUTTON6, R);
    PRESSED(P1_DIAL_L, L2); PRESSED(P1_DIAL_R, R2);
    if (player == 0) { PRESSED(SERV_COIN, L3); PRESSED(SERV_SWITCH, R3); }
#else
    PRESSED(P1_BUTTONA, B); PRESSED(P1_BUTTONB, A); PRESSED(P1_BUTTONC, Y); PRESSED(P1_BUTTOND, X);
#if EMU_SYSTEM == MVS
    PRESSED(P1_COIN, SELECT);
    if (player == 0) { PRESSED(SERV_COIN, L3); PRESSED(TEST_SWITCH, R3); }
#else
    PRESSED(P1_SELECT, SELECT);
#endif
#endif
#undef PRESSED
}
uint32_t poll_gamepad(void) { return lr_pad(0); }
uint32_t poll_gamepad_analog(void) { return lr_pad(0); }
uint32_t poll_gamepad_fatfursp(void) { return lr_pad(0); }
void pad_init(void) {}
void pad_exit(void) {}
void pad_wait_clear(void) {}
void pad_wait_press(int msec) { (void)msec; } /* errors return to frontend */
void pad_update(void) {}
bool pad_pressed(uint32_t code) { return (lr_pad(0) & code) != 0; }
bool pad_pressed_any(void) { return lr_pad(0) != 0; }

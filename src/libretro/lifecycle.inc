/* Included in each machine translation unit to keep its private reset/init logic. */
#ifdef LIBRETRO
static int lr_memory, lr_sound, lr_input, lr_ready;
int lr_initialized_machine;
void lr_machine_unload(void)
{
#if EMU_SYSTEM == NCDZ
    cdda_stop();
    cdda_shutdown();
    cdrom_shutdown();
#endif
    /* Only a successfully loaded machine may commit persistent data. */
    if (lr_ready) LR_EXIT();
    if (lr_input) input_shutdown();
    if (lr_sound) sound_exit();
    if (lr_memory) memory_shutdown();
#if EMU_SYSTEM != NCDZ
    file_close();
#endif
    zip_close();
    lr_memory = lr_sound = lr_input = lr_ready = 0;
    lr_initialized_machine = 0;
    Loop = LOOP_BROWSER;
}
int lr_machine_load(void)
{
    fatal_error = 0;
    Loop = LOOP_EXEC;
    lr_memory = 1;
    if (!memory_init() || fatal_error) goto fail;
    lr_sound = 1;
    if (!sound_init() || fatal_error) goto fail;
#if EMU_SYSTEM == NCDZ
    if (!cdda_init()) goto fail;
    cdrom_init();
#endif
    lr_input = 1;
    if (!input_init() || fatal_error) goto fail;
    if (!LR_INIT() || fatal_error) goto fail;
    lr_ready = lr_initialized_machine = 1;
    lr_machine_reset();
    return 1;
fail:
    lr_machine_unload();
    return 0;
}
void lr_machine_reset(void)
{
    if (!lr_ready) return;
#if EMU_SYSTEM == NCDZ
    cdda_stop();
#endif
    LR_RESET();
#if EMU_SYSTEM == CPS2
    qsound_sh_reset();
#elif EMU_SYSTEM == CPS1
    if (machine_sound_type == SOUND_QSOUND) qsound_sh_reset();
#endif
    lr_options();
    lr_audio_reset();
}
void lr_machine_frame(void)
{
    if (!lr_ready || fatal_error) return;
    if (Loop == LOOP_RESET) lr_machine_reset();
    if (Loop != LOOP_EXEC) return;
    update_inputport();
    timer_update_cpu();
#if EMU_SYSTEM == NCDZ
    if (--watchdog_counter <= 0) Loop = LOOP_RESET;
#endif
#if EMU_SYSTEM == NCDZ
    neogeo_cdda_check();
    if (neogeo_loadfinished) {
        neogeo_loadfinished = 0;
        video_enable_w(1); fix_disable_w(0); spr_disable_w(0);
    }
#endif
    lr_audio_frame();
}
#endif

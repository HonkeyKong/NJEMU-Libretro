/* -------------------------------------------------------------------------
    Optional (non-standard) extension API for enhanced frontends.
    Author: Ryan Souders (ryan@deadsignallabs.com)

    NJEMU implementation of the libretro-mame libretro_ext interface. The
    exported tables match libretro-mame's ABI; the behaviour is mapped onto
    NJEMU's fixed machine layout:

    - CPUs:    ":maincpu" (68000) and ":audiocpu" (Z80).
    - Spaces:  "program" for both CPUs, "io" for the Z80.
    - Regions: NJEMU's ROM/memory regions (":maincpu", ":audiocpu", ":gfx1"...).
    - Shares:  the machine's work/video/save RAM.
    - DIPs:    derived from the per-game DIP switch tables in cpsN/mvs dipsw.c
               (CPS1 and MVS; CPS2 and NCDZ have none).
    - Watch and inject rules hook the CPU cores' data-access callbacks while
      debug extensions are enabled; they cost nothing otherwise.
    - No savestates exist in this port, so the rollback surfaces report
      themselves unsupported.
-------------------------------------------------------------------------- */

#include "emumain.h"
#include "libretro.h"
#include "libretro_ext.h"
#include <ctype.h>

#define EXT_CPU_MAIN	0
#define EXT_CPU_AUDIO	1
#define EXT_CPU_COUNT	2
#define EXT_MAX_RULES	64
#define EXT_PC_RING		256

/* MAME device-state numbering, so frontends can share register ids across cores. */
enum
{
	MAME_M68K_SP = 1, MAME_M68K_ISP, MAME_M68K_USP, MAME_M68K_MSP, MAME_M68K_SR,
	MAME_M68K_D0 = 14, MAME_M68K_A0 = 22, MAME_M68K_A7 = 29
};
enum
{
	MAME_Z80_SP = 1, MAME_Z80_A, MAME_Z80_F, MAME_Z80_B, MAME_Z80_C, MAME_Z80_D, MAME_Z80_E,
	MAME_Z80_H, MAME_Z80_L, MAME_Z80_AF, MAME_Z80_BC, MAME_Z80_DE, MAME_Z80_HL,
	MAME_Z80_IX, MAME_Z80_IY, MAME_Z80_AF2, MAME_Z80_BC2, MAME_Z80_DE2, MAME_Z80_HL2,
	MAME_Z80_R, MAME_Z80_I, MAME_Z80_IM, MAME_Z80_IFF1, MAME_Z80_IFF2, MAME_Z80_HALT
};

typedef struct
{
	int cpu;
	uint64_t start, end;
	uint8_t access, width;
	bool enabled;
} ext_watch_rule;

typedef struct
{
	int cpu;
	uint64_t start, end;
	uint32_t value;
	uint8_t width;
	bool enabled, one_shot, has_match;
	uint32_t match_value, match_mask;
} ext_inject_rule;

typedef struct
{
	uint64_t pcs[EXT_PC_RING];
	uint32_t head;
	bool filled;
} ext_pc_ring;

typedef struct
{
	const char *tag;
	uint8_t **base;
	uint32_t *length;
} ext_region;

typedef struct
{
	const char *tag;
	void *base;
	uint32_t size;
} ext_share;

static const char *const ext_cpu_tags[EXT_CPU_COUNT] = { ":maincpu", ":audiocpu" };

static bool ext_debug_enabled;
static struct libretro_ext_watch_hit ext_last_hit;
static ext_watch_rule ext_watch[EXT_MAX_RULES];
static unsigned ext_watch_count;
static ext_inject_rule ext_inject[EXT_MAX_RULES];
static unsigned ext_inject_count;
static ext_pc_ring ext_pc_history[EXT_CPU_COUNT];
static uint32_t ext_inst_pc[EXT_CPU_COUNT];

static uint8_t  (*ext_orig_m68k_r8)(uint32_t);
static uint16_t (*ext_orig_m68k_r16)(uint32_t);
static void     (*ext_orig_m68k_w8)(uint32_t, uint8_t);
static void     (*ext_orig_m68k_w16)(uint32_t, uint16_t);
static uint8_t  (*ext_orig_z80_r8)(uint32_t);
static void     (*ext_orig_z80_w8)(uint32_t, uint8_t);

static struct retro_memory_descriptor ext_mmap_descs[8];


/******************************************************************************
	Machine layout
******************************************************************************/

static const ext_region ext_regions[] =
{
	{ ":maincpu",  &memory_region_cpu1,   &memory_length_cpu1   },
	{ ":audiocpu", &memory_region_cpu2,   &memory_length_cpu2   },
	{ ":gfx1",     &memory_region_gfx1,   &memory_length_gfx1   },
#if (EMU_SYSTEM == MVS || EMU_SYSTEM == NCDZ)
	{ ":gfx2",     &memory_region_gfx2,   &memory_length_gfx2   },
	{ ":gfx3",     &memory_region_gfx3,   &memory_length_gfx3   },
#endif
#if (EMU_SYSTEM == MVS)
	{ ":gfx4",     &memory_region_gfx4,   &memory_length_gfx4   },
#endif
	{ ":sound1",   &memory_region_sound1, &memory_length_sound1 },
#if (EMU_SYSTEM == MVS)
	{ ":sound2",   &memory_region_sound2, &memory_length_sound2 },
#endif
	{ ":user1",    &memory_region_user1,  &memory_length_user1  },
#if ((EMU_SYSTEM == CPS1 || EMU_SYSTEM == MVS) && !RELEASE) || EMU_SYSTEM == NCDZ
	{ ":user2",    &memory_region_user2,  &memory_length_user2  },
#endif
#if (EMU_SYSTEM == MVS)
	{ ":user3",    &memory_region_user3,  &memory_length_user3  },
#endif
};

#define EXT_REGION_TABLE_SIZE	(int)(sizeof(ext_regions) / sizeof(ext_regions[0]))

static int ext_shares(ext_share *out)
{
	int n = 0;
#if (EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2)
	out[n++] = (ext_share){ ":mainram", cps1_ram, sizeof(cps1_ram) };
	out[n++] = (ext_share){ ":gfxram", cps1_gfxram, sizeof(cps1_gfxram) };
#endif
#if (EMU_SYSTEM == CPS2)
	out[n++] = (ext_share){ ":cps2_ram", cps2_ram, 0x4000 };
	out[n++] = (ext_share){ ":objram1", cps2_objram[0], sizeof(cps2_objram[0]) };
	out[n++] = (ext_share){ ":objram2", cps2_objram[1], sizeof(cps2_objram[1]) };
#endif
#if (EMU_SYSTEM == MVS)
	out[n++] = (ext_share){ ":mainram", neogeo_ram, sizeof(neogeo_ram) };
	out[n++] = (ext_share){ ":saveram", neogeo_sram16, sizeof(neogeo_sram16) };
	out[n++] = (ext_share){ ":memcard", neogeo_memcard, sizeof(neogeo_memcard) };
#endif
#if (EMU_SYSTEM == NCDZ)
	if (memory_region_cpu1)
		out[n++] = (ext_share){ ":mainram", memory_region_cpu1, memory_length_cpu1 };
	out[n++] = (ext_share){ ":memcard", neogeo_memcard, sizeof(neogeo_memcard) };
#endif
	return n;
}

#define EXT_MAX_SHARES	8


/******************************************************************************
	Helpers
******************************************************************************/

static bool ext_running(void)
{
	return lr_initialized_machine != 0;
}

static void ext_copy_string(char *dst, size_t size, const char *src)
{
	if (!dst || !size) return;
	snprintf(dst, size, "%s", src ? src : "");
}

static bool ext_tag_equals(const char *tag, const char *name)
{
	if (!tag || !name) return false;
	if (tag[0] == ':') tag++;
	if (name[0] == ':') name++;
	return !strcmp(tag, name);
}

static int ext_cpu_from_tag(const char *tag)
{
	for (int i = 0; i < EXT_CPU_COUNT; i++)
		if (ext_tag_equals(tag, ext_cpu_tags[i]))
			return i;
	return -1;
}

static uint64_t ext_cpu_total_cycles(int cpu)
{
	if (!ext_running()) return 0;
	return cpu == EXT_CPU_MAIN ? m68000_get_total_cycles() : z80_get_total_cycles();
}

static uint32_t ext_cpu_pc(int cpu)
{
	return cpu == EXT_CPU_MAIN ? C68k_Get_Reg(&C68K, C68K_PC) : Cz80_Get_Reg(&CZ80, CZ80_PC);
}


/******************************************************************************
	Watch / inject rules
******************************************************************************/

static void ext_check_watch(int cpu, uint64_t pc, uint64_t address, uint32_t value,
							uint8_t access, uint8_t width, uint64_t total_cycles)
{
	if (!ext_debug_enabled || cpu < 0)
		return;

	for (unsigned i = 0; i < ext_watch_count; i++)
	{
		const ext_watch_rule *rule = &ext_watch[i];

		if (!rule->enabled || rule->cpu != cpu) continue;
		if (!(rule->access & access)) continue;
		if (rule->width && rule->width != width) continue;
		if (address < rule->start || address > rule->end) continue;

		memset(&ext_last_hit, 0, sizeof(ext_last_hit));
		ext_last_hit.hit = true;
		ext_copy_string(ext_last_hit.cpuTag, sizeof(ext_last_hit.cpuTag), ext_cpu_tags[cpu]);
		ext_last_hit.address = address;
		ext_last_hit.pc = pc;
		ext_last_hit.frame = lr_frame_number;
		ext_last_hit.totalCycles = total_cycles;
		ext_last_hit.value = value;
		ext_last_hit.access = access;
		ext_last_hit.width = width;

		const ext_pc_ring *ring = &ext_pc_history[cpu];
		uint32_t count = ring->filled ? EXT_PC_RING : ring->head;
		ext_last_hit.historyCount = count;
		for (uint32_t j = 0; j < count; j++)
			ext_last_hit.pcHistory[j] = ring->pcs[ring->filled ? (ring->head + j) % EXT_PC_RING : j];
		break;
	}
}

/* Called from inside the CPU cores: the PC is the start of the current instruction. */
static void ext_core_watch(int cpu, uint32_t address, uint32_t value, uint8_t access, uint8_t width)
{
	if (!ext_watch_count)
		return;
	ext_check_watch(cpu, ext_inst_pc[cpu], address, value, access, width, ext_cpu_total_cycles(cpu));
}

static bool ext_inject_covers(int cpu, uint32_t address, uint8_t width)
{
	for (unsigned i = 0; i < ext_inject_count; i++)
	{
		const ext_inject_rule *rule = &ext_inject[i];
		if (rule->enabled && rule->cpu == cpu && address >= rule->start && address <= rule->end
				&& (!rule->width || rule->width == width))
			return true;
	}
	return false;
}

static uint32_t ext_apply_inject(int cpu, uint32_t address, uint32_t data, uint8_t width)
{
	for (unsigned i = 0; i < ext_inject_count; i++)
	{
		ext_inject_rule *rule = &ext_inject[i];

		if (!rule->enabled || rule->cpu != cpu) continue;
		if (address < rule->start || address > rule->end) continue;
		if (rule->width && rule->width != width) continue;
		if (rule->has_match && (data & rule->match_mask) != (rule->match_value & rule->match_mask))
			continue; /* no inject, oneShot not consumed */

		data = rule->value & (width == 1 ? 0xff : 0xffff);
		if (ext_last_hit.hit && ext_last_hit.address == address)
			ext_last_hit.value = data;
		ext_core_watch(cpu, address, data, LIBRETRO_EXT_WATCH_READ, width);
		if (rule->one_shot)
			rule->enabled = false;
	}
	return data;
}

/* 68000 data-access hooks. Like MAME's 16-bit read taps, inject rules see
   word-wide accesses at the aligned address. */
static uint16_t ext_hook_m68k_r16(uint32_t address)
{
	uint16_t data = ext_orig_m68k_r16(address);
	if (ext_inject_count)
		data = (uint16_t)ext_apply_inject(EXT_CPU_MAIN, address & ~1, data, 2);
	ext_core_watch(EXT_CPU_MAIN, address, data, LIBRETRO_EXT_WATCH_READ, 2);
	return data;
}

static uint8_t ext_hook_m68k_r8(uint32_t address)
{
	uint8_t data;

	if (ext_inject_count && ext_inject_covers(EXT_CPU_MAIN, address & ~1, 2))
	{
		uint16_t word = ext_orig_m68k_r16(address & ~1);
		word = (uint16_t)ext_apply_inject(EXT_CPU_MAIN, address & ~1, word, 2);
		data = (address & 1) ? (word & 0xff) : (word >> 8);
	}
	else
		data = ext_orig_m68k_r8(address);

	ext_core_watch(EXT_CPU_MAIN, address, data, LIBRETRO_EXT_WATCH_READ, 1);
	return data;
}

static void ext_hook_m68k_w8(uint32_t address, uint8_t data)
{
	ext_core_watch(EXT_CPU_MAIN, address, data, LIBRETRO_EXT_WATCH_WRITE, 1);
	ext_orig_m68k_w8(address, data);
}

static void ext_hook_m68k_w16(uint32_t address, uint16_t data)
{
	ext_core_watch(EXT_CPU_MAIN, address, data, LIBRETRO_EXT_WATCH_WRITE, 2);
	ext_orig_m68k_w16(address, data);
}

/* Z80 hooks. Outside CPS1 the Z80 core reads ROM/RAM directly, so only its
   writes (and CPS1 reads) pass through here. */
static uint8_t ext_hook_z80_r8(uint32_t address)
{
	uint8_t data = ext_orig_z80_r8(address);
	if (ext_inject_count)
		data = (uint8_t)ext_apply_inject(EXT_CPU_AUDIO, address, data, 1);
	ext_core_watch(EXT_CPU_AUDIO, address, data, LIBRETRO_EXT_WATCH_READ, 1);
	return data;
}

static void ext_hook_z80_w8(uint32_t address, uint8_t data)
{
	ext_core_watch(EXT_CPU_AUDIO, address, data, LIBRETRO_EXT_WATCH_WRITE, 1);
	ext_orig_z80_w8(address, data);
}

static void ext_pc_push(int cpu, uint32_t pc)
{
	ext_pc_ring *ring = &ext_pc_history[cpu];
	ext_inst_pc[cpu] = pc;
	ring->pcs[ring->head] = pc;
	ring->head = (ring->head + 1) % EXT_PC_RING;
	if (ring->head == 0)
		ring->filled = true;
}

static void ext_trace_m68k(uint32_t pc) { ext_pc_push(EXT_CPU_MAIN, pc); }
static void ext_trace_z80(uint32_t pc) { ext_pc_push(EXT_CPU_AUDIO, pc); }

#define EXT_HOOK(field, hook, orig, install)	\
	do {										\
		if (install) {							\
			if (field && field != hook) { orig = field; field = hook; }	\
		} else if (field == hook) field = orig;	\
	} while (0)

/* Machine (re)initialisation rebuilds the CPU structs, so this runs every frame. */
static void ext_update_hooks(void)
{
	bool trace = ext_debug_enabled && ext_running();
	bool mem = trace && (ext_watch_count || ext_inject_count);

	EXT_HOOK(C68K.Read_Byte,  ext_hook_m68k_r8,  ext_orig_m68k_r8,  mem);
	EXT_HOOK(C68K.Read_Word,  ext_hook_m68k_r16, ext_orig_m68k_r16, mem);
	EXT_HOOK(C68K.Write_Byte, ext_hook_m68k_w8,  ext_orig_m68k_w8,  mem);
	EXT_HOOK(C68K.Write_Word, ext_hook_m68k_w16, ext_orig_m68k_w16, mem);
	EXT_HOOK(CZ80.Read_Byte,  ext_hook_z80_r8,   ext_orig_z80_r8,   mem);
	EXT_HOOK(CZ80.Write_Byte, ext_hook_z80_w8,   ext_orig_z80_w8,   mem);
	C68K.Trace_CallBack = trace ? ext_trace_m68k : NULL;
	CZ80.Trace_Callback = trace ? ext_trace_z80 : NULL;
}

static void ext_clear_watch_rules_impl(void)
{
	ext_watch_count = 0;
	ext_update_hooks();
}

static void ext_clear_inject_rules_impl(void)
{
	ext_inject_count = 0;
	ext_update_hooks();
}

static void ext_add_watch_rule_impl(const char *cpuTag, uint64_t start, uint64_t end, uint8_t access, uint8_t width)
{
	int cpu = ext_cpu_from_tag(cpuTag);

	if (!ext_debug_enabled || !cpuTag || !cpuTag[0] || !ext_running())
		return;
	if (cpu < 0)
	{
		lr_log(RETRO_LOG_INFO, "libretro_ext: no device for tag %s\n", cpuTag);
		return;
	}
	for (unsigned i = 0; i < ext_watch_count; i++)
	{
		const ext_watch_rule *r = &ext_watch[i];
		if (r->enabled && r->cpu == cpu && r->start == start && r->end == end && r->access == access && r->width == width)
			return;
	}
	if (ext_watch_count >= EXT_MAX_RULES)
	{
		lr_log(RETRO_LOG_WARN, "libretro_ext: watch rule limit (%d) reached\n", EXT_MAX_RULES);
		return;
	}
	ext_watch[ext_watch_count++] = (ext_watch_rule){ cpu, start, end, access, width, true };
	ext_update_hooks();
	lr_log(RETRO_LOG_INFO, "libretro_ext: installed watch rule cpu=%s start=%llX end=%llX access=%u width=%u\n",
		   ext_cpu_tags[cpu], (unsigned long long)start, (unsigned long long)end, (unsigned)access, (unsigned)width);
}

static void ext_add_inject_rule_ex_impl(const char *cpuTag, uint64_t start, uint64_t end,
										uint32_t value, uint8_t width, bool oneShot,
										bool hasMatchValue, uint32_t matchValue, uint32_t matchMask)
{
	int cpu = ext_cpu_from_tag(cpuTag);

	if (!ext_debug_enabled || !cpuTag || !cpuTag[0])
		return;
	if (!ext_running())
	{
		lr_log(RETRO_LOG_WARN, "libretro_ext: add_inject_rule called with no running machine\n");
		return;
	}
	if (cpu < 0)
	{
		lr_log(RETRO_LOG_WARN, "libretro_ext: inject rule: no AS_PROGRAM space for cpu=%s\n", cpuTag);
		return;
	}
	for (unsigned i = 0; i < ext_inject_count; i++)
	{
		const ext_inject_rule *r = &ext_inject[i];
		if (r->enabled && r->cpu == cpu && r->start == start && r->end == end && r->value == value
				&& r->width == width && r->one_shot == oneShot && r->has_match == hasMatchValue
				&& r->match_value == matchValue && r->match_mask == matchMask)
			return;
	}
	if (ext_inject_count >= EXT_MAX_RULES)
	{
		lr_log(RETRO_LOG_WARN, "libretro_ext: inject rule limit (%d) reached\n", EXT_MAX_RULES);
		return;
	}
	ext_inject[ext_inject_count++] = (ext_inject_rule){ cpu, start, end, value, width, true, oneShot,
													   hasMatchValue, matchValue, matchMask };
	ext_update_hooks();
	lr_log(RETRO_LOG_INFO, "libretro_ext: inject rule installed cpu=%s start=%llX end=%llX value=0x%X width=%u oneShot=%d hasMatch=%d matchValue=0x%X matchMask=0x%X\n",
		   ext_cpu_tags[cpu], (unsigned long long)start, (unsigned long long)end, (unsigned)value,
		   (unsigned)width, (int)oneShot, (int)hasMatchValue, (unsigned)matchValue, (unsigned)matchMask);
}

static void ext_add_inject_rule_impl(const char *cpuTag, uint64_t start, uint64_t end,
									 uint32_t value, uint8_t width, bool oneShot)
{
	ext_add_inject_rule_ex_impl(cpuTag, start, end, value, width, oneShot, false, 0, 0xffffffff);
}

static bool ext_get_last_watch_hit_impl(struct libretro_ext_watch_hit *outHit)
{
	if (!outHit || !ext_last_hit.hit)
		return false;
	*outHit = ext_last_hit;
	return true;
}

static void ext_clear_last_watch_hit_impl(void)
{
	memset(&ext_last_hit, 0, sizeof(ext_last_hit));
}

static void ext_check_watch_hit_impl(const char *cpuTag, uint64_t pc, uint64_t address, uint32_t value,
									 uint8_t access, uint8_t width, uint64_t totalCycles)
{
	ext_check_watch(ext_cpu_from_tag(cpuTag), pc, address, value, access, width, totalCycles);
}

static void ext_reset_debug_state(void)
{
	ext_watch_count = ext_inject_count = 0;
	memset(&ext_last_hit, 0, sizeof(ext_last_hit));
	memset(ext_pc_history, 0, sizeof(ext_pc_history));
	memset(ext_inst_pc, 0, sizeof(ext_inst_pc));
	ext_update_hooks();
}

static void ext_set_debug_extensions_enabled_impl(bool enabled)
{
	if (ext_debug_enabled == enabled)
		return;
	ext_debug_enabled = enabled;
	if (!enabled)
		ext_reset_debug_state();
	else
		ext_update_hooks();
}

static bool ext_get_debug_extensions_enabled_impl(void)
{
	return ext_debug_enabled;
}

static bool ext_trigger_timing_capture_impl(void)
{
	return false; /* no driver timing-capture windows in NJEMU */
}


/******************************************************************************
	CPU access
******************************************************************************/

static const char *ext_get_driver_name_impl(void)
{
	return ext_running() ? game_name : NULL;
}

static int ext_cpu_count_impl(void)
{
	return ext_running() ? EXT_CPU_COUNT : 0;
}

static const char *ext_get_cpu_tag_impl(int cpu_index)
{
	if (!ext_running() || cpu_index < 0 || cpu_index >= EXT_CPU_COUNT)
		return NULL;
	return ext_cpu_tags[cpu_index];
}

static uint64_t ext_get_cpu_pc_impl(int cpu_index)
{
	if (!ext_running() || cpu_index < 0 || cpu_index >= EXT_CPU_COUNT)
		return 0;
	return ext_cpu_pc(cpu_index);
}

static uint64_t ext_get_cpu_pc_by_tag_impl(const char *cpu_tag)
{
	return ext_get_cpu_pc_impl(ext_cpu_from_tag(cpu_tag));
}

static uint64_t ext_get_cpu_total_cycles_by_tag_impl(const char *cpu_tag)
{
	int cpu = ext_cpu_from_tag(cpu_tag);
	return cpu < 0 ? 0 : ext_cpu_total_cycles(cpu);
}

static bool ext_m68k_get_state(int state_id, uint64_t *out)
{
	switch (state_id)
	{
	case LIBRETRO_EXT_STATE_GENPC:
	case LIBRETRO_EXT_STATE_GENPCBASE: *out = C68k_Get_Reg(&C68K, C68K_PC); return true;
	case LIBRETRO_EXT_STATE_GENFLAGS:
	case MAME_M68K_SR:  *out = C68k_Get_Reg(&C68K, C68K_SR); return true;
	case MAME_M68K_SP:  *out = C68k_Get_Reg(&C68K, C68K_A7); return true;
	case MAME_M68K_ISP:
	case MAME_M68K_MSP: *out = C68k_Get_Reg(&C68K, C68K_MSP); return true;
	case MAME_M68K_USP: *out = C68k_Get_Reg(&C68K, C68K_USP); return true;
	}
	if (state_id >= MAME_M68K_D0 && state_id <= MAME_M68K_A7)
	{
		*out = C68k_Get_Reg(&C68K, C68K_D0 + (state_id - MAME_M68K_D0));
		return true;
	}
	return false;
}

static bool ext_m68k_set_state(int state_id, uint32_t value)
{
	switch (state_id)
	{
	case LIBRETRO_EXT_STATE_GENPC:
	case LIBRETRO_EXT_STATE_GENPCBASE: C68k_Set_Reg(&C68K, C68K_PC, value); return true;
	case LIBRETRO_EXT_STATE_GENFLAGS:
	case MAME_M68K_SR:  C68k_Set_Reg(&C68K, C68K_SR, value); return true;
	case MAME_M68K_SP:  C68k_Set_Reg(&C68K, C68K_A7, value); return true;
	case MAME_M68K_ISP:
	case MAME_M68K_MSP: C68k_Set_Reg(&C68K, C68K_MSP, value); return true;
	case MAME_M68K_USP: C68k_Set_Reg(&C68K, C68K_USP, value); return true;
	}
	if (state_id >= MAME_M68K_D0 && state_id <= MAME_M68K_A7)
	{
		C68k_Set_Reg(&C68K, C68K_D0 + (state_id - MAME_M68K_D0), value);
		return true;
	}
	return false;
}

/* MAME packs AF as A:F; CZ80 stores it as F:A. */
static uint32_t ext_swap8(uint32_t v) { return ((v & 0xff) << 8) | ((v >> 8) & 0xff); }

static bool ext_z80_pair(int state_id, int *reg, int *part)
{
	static const int pairs[] = { CZ80_FA, CZ80_BC, CZ80_DE, CZ80_HL };

	*part = 0;
	if (state_id >= MAME_Z80_A && state_id <= MAME_Z80_L)
	{
		int i = state_id - MAME_Z80_A;
		*reg = pairs[i / 2];
		*part = (i & 1) ? 2 : 1; /* 1 = high byte (A,B,D,H), 2 = low byte (F,C,E,L) */
		return true;
	}
	switch (state_id)
	{
	case LIBRETRO_EXT_STATE_GENPC:
	case LIBRETRO_EXT_STATE_GENPCBASE: *reg = CZ80_PC; return true;
	case MAME_Z80_SP:   *reg = CZ80_SP; return true;
	case MAME_Z80_AF:   *reg = CZ80_FA; return true;
	case MAME_Z80_BC:   *reg = CZ80_BC; return true;
	case MAME_Z80_DE:   *reg = CZ80_DE; return true;
	case MAME_Z80_HL:   *reg = CZ80_HL; return true;
	case MAME_Z80_IX:   *reg = CZ80_IX; return true;
	case MAME_Z80_IY:   *reg = CZ80_IY; return true;
	case MAME_Z80_AF2:  *reg = CZ80_FA2; return true;
	case MAME_Z80_BC2:  *reg = CZ80_BC2; return true;
	case MAME_Z80_DE2:  *reg = CZ80_DE2; return true;
	case MAME_Z80_HL2:  *reg = CZ80_HL2; return true;
	case MAME_Z80_R:    *reg = CZ80_R; return true;
	case MAME_Z80_I:    *reg = CZ80_I; return true;
	case MAME_Z80_IM:   *reg = CZ80_IM; return true;
	case MAME_Z80_IFF1: *reg = CZ80_IFF1; return true;
	case MAME_Z80_IFF2: *reg = CZ80_IFF2; return true;
	case MAME_Z80_HALT: *reg = CZ80_HALT; return true;
	}
	if (state_id == LIBRETRO_EXT_STATE_GENFLAGS)
	{
		*reg = CZ80_FA;
		*part = 2;
		return true;
	}
	return false;
}

static bool ext_z80_get_state(int state_id, uint64_t *out)
{
	int reg, part;
	uint32_t v;

	if (!ext_z80_pair(state_id, &reg, &part))
		return false;
	v = Cz80_Get_Reg(&CZ80, reg);
	if (reg == CZ80_FA || reg == CZ80_FA2)
		v = ext_swap8(v);
	if (part == 1) v >>= 8;
	if (part == 2) v &= 0xff;
	*out = v;
	return true;
}

static bool ext_z80_set_state(int state_id, uint32_t value)
{
	int reg, part;
	bool swapped;
	uint32_t v;

	if (!ext_z80_pair(state_id, &reg, &part) || reg == CZ80_HALT)
		return false;
	swapped = reg == CZ80_FA || reg == CZ80_FA2;
	if (part)
	{
		v = Cz80_Get_Reg(&CZ80, reg);
		if (swapped) v = ext_swap8(v);
		v = part == 1 ? (v & 0x00ff) | ((value & 0xff) << 8) : (v & 0xff00) | (value & 0xff);
		value = v;
	}
	Cz80_Set_Reg(&CZ80, reg, swapped ? ext_swap8(value) : value);
	return true;
}

static bool ext_read_state(int cpu, int state_id, uint64_t *out_value)
{
	if (!out_value || !ext_running() || cpu < 0 || cpu >= EXT_CPU_COUNT)
		return false;
	return cpu == EXT_CPU_MAIN ? ext_m68k_get_state(state_id, out_value) : ext_z80_get_state(state_id, out_value);
}

static bool ext_write_state(int cpu, int state_id, uint64_t value)
{
	if (!ext_running() || cpu < 0 || cpu >= EXT_CPU_COUNT)
		return false;
	return cpu == EXT_CPU_MAIN ? ext_m68k_set_state(state_id, (uint32_t)value) : ext_z80_set_state(state_id, (uint32_t)value);
}

static bool ext_read_cpu_state_u64_by_index_impl(int cpu_index, int state_id, uint64_t *out_value)
{
	return ext_read_state(cpu_index, state_id, out_value);
}

static bool ext_read_cpu_state_u64_by_tag_impl(const char *cpu_tag, int state_id, uint64_t *out_value)
{
	return ext_read_state(ext_cpu_from_tag(cpu_tag), state_id, out_value);
}

static bool ext_write_cpu_state_u64_by_index_impl(int cpu_index, int state_id, uint64_t value)
{
	return ext_write_state(cpu_index, state_id, value);
}

static bool ext_write_cpu_state_u64_by_tag_impl(const char *cpu_tag, int state_id, uint64_t value)
{
	return ext_write_state(ext_cpu_from_tag(cpu_tag), state_id, value);
}

/* Same name set as libretro-mame: D0-D7/A0-A7/SP/SR need the 68000, PC works on any CPU. */
static bool ext_register_name_to_state_id(const char *reg_name, int *out_state_id, bool *out_requires_m68k)
{
	char name[16];
	size_t n = 0;

	if (!reg_name || !reg_name[0])
		return false;
	for (const unsigned char *p = (const unsigned char *)reg_name; *p && n < sizeof(name) - 1; p++)
		if (!isspace(*p))
			name[n++] = (char)toupper(*p);
	name[n] = 0;

	*out_requires_m68k = true;
	if (n == 2 && name[0] == 'D' && name[1] >= '0' && name[1] <= '7') { *out_state_id = MAME_M68K_D0 + (name[1] - '0'); return true; }
	if (n == 2 && name[0] == 'A' && name[1] >= '0' && name[1] <= '7') { *out_state_id = MAME_M68K_A0 + (name[1] - '0'); return true; }
	if (!strcmp(name, "SP")) { *out_state_id = MAME_M68K_A7; return true; }
	if (!strcmp(name, "SR")) { *out_state_id = MAME_M68K_SR; return true; }

	*out_requires_m68k = false;
	if (!strcmp(name, "PC") || !strcmp(name, "GENPC")) { *out_state_id = LIBRETRO_EXT_STATE_GENPC; return true; }
	if (!strcmp(name, "GENPCBASE")) { *out_state_id = LIBRETRO_EXT_STATE_GENPCBASE; return true; }
	return false;
}

static bool ext_read_cpu_register_by_tag_impl(const char *cpu_tag, const char *reg_name, uint64_t *out_value)
{
	int cpu = ext_cpu_from_tag(cpu_tag), state_id;
	bool requires_m68k;

	if (!ext_register_name_to_state_id(reg_name, &state_id, &requires_m68k))
		return false;
	if (requires_m68k && cpu != EXT_CPU_MAIN)
		return false;
	return ext_read_state(cpu, state_id, out_value);
}

static bool ext_write_cpu_register_by_tag_impl(const char *cpu_tag, const char *reg_name, uint64_t value)
{
	int cpu = ext_cpu_from_tag(cpu_tag), state_id;
	bool requires_m68k;

	if (!ext_register_name_to_state_id(reg_name, &state_id, &requires_m68k))
		return false;
	if (requires_m68k && cpu != EXT_CPU_MAIN)
		return false;
	return ext_write_state(cpu, state_id, value);
}


/******************************************************************************
	Address-space access (through the machine's handlers, bypassing ext hooks)
******************************************************************************/

enum { EXT_SPACE_PROGRAM, EXT_SPACE_IO };

static int ext_space(int cpu, const char *space)
{
	if (cpu < 0 || !ext_running())
		return -1;
	if (!space || !space[0] || !strcmp(space, "program"))
		return EXT_SPACE_PROGRAM;
	if (cpu == EXT_CPU_AUDIO && !strcmp(space, "io") && CZ80.IN_Port && CZ80.OUT_Port)
		return EXT_SPACE_IO;
	return -1;
}

static uint8_t ext_bus_read8(int cpu, int space, uint32_t addr)
{
	if (cpu == EXT_CPU_MAIN)
		return m68000_read_memory_8(addr & 0xffffff);
	if (space == EXT_SPACE_IO)
		return CZ80.IN_Port((uint16_t)addr);
	return z80_read_memory_8(addr & 0xffff);
}

static void ext_bus_write8(int cpu, int space, uint32_t addr, uint8_t v)
{
	if (cpu == EXT_CPU_MAIN)
		m68000_write_memory_8(addr & 0xffffff, v);
	else if (space == EXT_SPACE_IO)
		CZ80.OUT_Port((uint16_t)addr, v);
	else
		z80_write_memory_8(addr & 0xffff, v);
}

static uint16_t ext_bus_read16(int cpu, int space, uint32_t addr)
{
	if (cpu == EXT_CPU_MAIN && !(addr & 1))
		return m68000_read_memory_16(addr & 0xffffff);
	if (cpu == EXT_CPU_MAIN)
		return (ext_bus_read8(cpu, space, addr) << 8) | ext_bus_read8(cpu, space, addr + 1);
	return ext_bus_read8(cpu, space, addr) | (ext_bus_read8(cpu, space, addr + 1) << 8);
}

static void ext_bus_write16(int cpu, int space, uint32_t addr, uint16_t v)
{
	if (cpu == EXT_CPU_MAIN && !(addr & 1))
		m68000_write_memory_16(addr & 0xffffff, v);
	else if (cpu == EXT_CPU_MAIN)
	{
		ext_bus_write8(cpu, space, addr, v >> 8);
		ext_bus_write8(cpu, space, addr + 1, v & 0xff);
	}
	else
	{
		ext_bus_write8(cpu, space, addr, v & 0xff);
		ext_bus_write8(cpu, space, addr + 1, v >> 8);
	}
}

static uint8_t ext_read_u8_impl(const char *cpu_tag, const char *space_name, uint64_t addr)
{
	int cpu = ext_cpu_from_tag(cpu_tag), space = ext_space(cpu, space_name);
	return space < 0 ? 0 : ext_bus_read8(cpu, space, (uint32_t)addr);
}

static uint16_t ext_read_u16_impl(const char *cpu_tag, const char *space_name, uint64_t addr)
{
	int cpu = ext_cpu_from_tag(cpu_tag), space = ext_space(cpu, space_name);
	return space < 0 ? 0 : ext_bus_read16(cpu, space, (uint32_t)addr);
}

static uint32_t ext_read_u32_impl(const char *cpu_tag, const char *space_name, uint64_t addr)
{
	int cpu = ext_cpu_from_tag(cpu_tag), space = ext_space(cpu, space_name);
	uint32_t a = (uint32_t)addr;

	if (space < 0)
		return 0;
	if (cpu == EXT_CPU_MAIN)
		return ((uint32_t)ext_bus_read16(cpu, space, a) << 16) | ext_bus_read16(cpu, space, a + 2);
	return ext_bus_read16(cpu, space, a) | ((uint32_t)ext_bus_read16(cpu, space, a + 2) << 16);
}

static void ext_write_u8_impl(const char *cpu_tag, const char *space_name, uint64_t addr, uint8_t v)
{
	int cpu = ext_cpu_from_tag(cpu_tag), space = ext_space(cpu, space_name);
	if (space >= 0)
		ext_bus_write8(cpu, space, (uint32_t)addr, v);
}

static void ext_write_u16_impl(const char *cpu_tag, const char *space_name, uint64_t addr, uint16_t v)
{
	int cpu = ext_cpu_from_tag(cpu_tag), space = ext_space(cpu, space_name);
	if (space >= 0)
		ext_bus_write16(cpu, space, (uint32_t)addr, v);
}

static void ext_write_u32_impl(const char *cpu_tag, const char *space_name, uint64_t addr, uint32_t v)
{
	int cpu = ext_cpu_from_tag(cpu_tag), space = ext_space(cpu, space_name);
	uint32_t a = (uint32_t)addr;

	if (space < 0)
		return;
	if (cpu == EXT_CPU_MAIN)
	{
		ext_bus_write16(cpu, space, a, v >> 16);
		ext_bus_write16(cpu, space, a + 2, v & 0xffff);
	}
	else
	{
		ext_bus_write16(cpu, space, a, v & 0xffff);
		ext_bus_write16(cpu, space, a + 2, v >> 16);
	}
}


/******************************************************************************
	Regions and shares
******************************************************************************/

static const ext_region *ext_region_at(int index)
{
	if (!ext_running() || index < 0)
		return NULL;
	for (int i = 0; i < EXT_REGION_TABLE_SIZE; i++)
		if (*ext_regions[i].base && *ext_regions[i].length && index-- == 0)
			return &ext_regions[i];
	return NULL;
}

static const ext_region *ext_find_region(const char *tag)
{
	const ext_region *region;
	for (int i = 0; (region = ext_region_at(i)) != NULL; i++)
		if (ext_tag_equals(tag, region->tag))
			return region;
	return NULL;
}

static int ext_get_region_count_impl(void)
{
	int n = 0;
	while (ext_region_at(n))
		n++;
	return n;
}

static const char *ext_get_region_tag_impl(int index)
{
	const ext_region *region = ext_region_at(index);
	return region ? region->tag : NULL;
}

static uint64_t ext_get_region_size_impl(const char *tag)
{
	const ext_region *region = ext_find_region(tag);
	return region ? *region->length : 0;
}

static uint64_t ext_clamp(uint64_t size, uint64_t offset, uint64_t bytes)
{
	if (offset >= size)
		return 0;
	return bytes < size - offset ? bytes : size - offset;
}

static uint64_t ext_read_region_impl(const char *tag, uint64_t offset, void *dst, uint64_t bytes)
{
	const ext_region *region = ext_find_region(tag);
	if (!region || !dst || !(bytes = ext_clamp(*region->length, offset, bytes)))
		return 0;
	memcpy(dst, *region->base + offset, (size_t)bytes);
	return bytes;
}

static uint64_t ext_write_region_impl(const char *tag, uint64_t offset, const void *src, uint64_t bytes)
{
	const ext_region *region = ext_find_region(tag);
	if (!region || !src || !(bytes = ext_clamp(*region->length, offset, bytes)))
		return 0;
	memcpy(*region->base + offset, src, (size_t)bytes);
	return bytes;
}

/* No region is certified immutable: MVS/NCDZ graphics stream from caches or the
   CD, and some program ROMs are patched at runtime. With no savestates there is
   no rollback consumer for this metadata either. */
static int ext_get_static_region_count_impl(void)
{
	return 0;
}

static bool ext_get_static_region_info_impl(int index, struct libretro_ext_static_region_info *out)
{
	(void)index; (void)out;
	return false;
}

static bool ext_share_at(int index, ext_share *out)
{
	ext_share shares[EXT_MAX_SHARES];
	int n;

	if (!ext_running() || index < 0)
		return false;
	n = ext_shares(shares);
	if (index >= n)
		return false;
	*out = shares[index];
	return true;
}

static bool ext_find_share(const char *tag, ext_share *out)
{
	for (int i = 0; ext_share_at(i, out); i++)
		if (ext_tag_equals(tag, out->tag))
			return true;
	return false;
}

static int ext_get_share_count_impl(void)
{
	ext_share shares[EXT_MAX_SHARES];
	return ext_running() ? ext_shares(shares) : 0;
}

static const char *ext_get_share_tag_impl(int index)
{
	ext_share share;
	return ext_share_at(index, &share) ? share.tag : NULL;
}

static uint64_t ext_get_share_size_impl(const char *share_tag)
{
	ext_share share;
	return ext_find_share(share_tag, &share) ? share.size : 0;
}

static uint32_t ext_get_share_flags_impl(const char *share_tag)
{
	ext_share share;
	return ext_find_share(share_tag, &share) ? LIBRETRO_EXT_SHARE_EXPORTABLE | LIBRETRO_EXT_SHARE_IMPORTABLE : 0;
}

static bool ext_get_share_info_impl(int index, struct libretro_ext_share_info *out)
{
	ext_share share;

	if (!out || !ext_share_at(index, &share))
		return false;
	memset(out, 0, sizeof(*out));
	ext_copy_string(out->name, sizeof(out->name), share.tag);
	out->size = share.size;
	out->flags = LIBRETRO_EXT_SHARE_EXPORTABLE | LIBRETRO_EXT_SHARE_IMPORTABLE;
	return true;
}

static uint64_t ext_read_share_impl(const char *share_tag, uint64_t offset, void *dst, uint64_t bytes)
{
	ext_share share;
	if (!dst || !ext_find_share(share_tag, &share) || !(bytes = ext_clamp(share.size, offset, bytes)))
		return 0;
	memcpy(dst, (uint8_t *)share.base + offset, (size_t)bytes);
	return bytes;
}

static uint64_t ext_write_share_impl(const char *share_tag, uint64_t offset, const void *src, uint64_t bytes)
{
	ext_share share;
	if (!src || !ext_find_share(share_tag, &share) || !(bytes = ext_clamp(share.size, offset, bytes)))
		return 0;
	memcpy((uint8_t *)share.base + offset, src, (size_t)bytes);
	return bytes;
}


/******************************************************************************
	Frame / time
******************************************************************************/

static uint64_t ext_get_frame_number_impl(void)
{
	return lr_frame_number;
}

/* Same packing as libretro-mame: seconds * 1e18 + attoseconds (wraps like it). */
static uint64_t ext_get_time_attoseconds_impl(void)
{
	double t;
	uint64_t seconds;

	if (!ext_running() || lr_fps <= 0)
		return 0;
	t = (double)lr_frame_number / lr_fps;
	seconds = (uint64_t)t;
	return seconds * 1000000000000000000ULL + (uint64_t)((t - (double)seconds) * 1e18);
}


/******************************************************************************
	DIP switches

	NJEMU keeps DIP settings as menu tables of setting indices, with per-game
	code converting between indices and switch bits. Each field's mask and raw
	setting values are recovered by probing that conversion: select each
	setting in turn, store it, and record which switch bits change.
******************************************************************************/

#if (EMU_SYSTEM == CPS1 || EMU_SYSTEM == MVS)

#define EXT_MAX_DIPS	32

#if (EMU_SYSTEM == CPS1)
#define EXT_DIP_BANKS	3
static const char *const ext_dip_port_tags[EXT_DIP_BANKS] = { ":DSWA", ":DSWB", ":DSWC" };
static const uint32_t ext_dip_defaults[EXT_DIP_BANKS] = { 0xff, 0xff, 0xff };
static uint32_t ext_dip_bank_get(int bank) { return cps1_dipswitch[bank] & 0xff; }
static void ext_dip_bank_set(int bank, uint32_t value) { cps1_dipswitch[bank] = value & 0xff; }
static dipswitch_t *ext_dip_load(void) { int sx = 0; return load_dipswitch(&sx); }
#else
/* Bank 1 is the PCB-board hard DIP (ms5pcb/svcpcb/kog); it takes effect on reset. */
#define EXT_DIP_BANKS	2
static const char *const ext_dip_port_tags[EXT_DIP_BANKS] = { ":DSW", ":HARDDIP" };
static const uint32_t ext_dip_defaults[EXT_DIP_BANKS] = { 0xff, 0x00 };
static uint32_t ext_dip_bank_get(int bank) { return (bank ? neogeo_hard_dipsw : neogeo_dipswitch) & 0xff; }
static void ext_dip_bank_set(int bank, uint32_t value) { if (bank) neogeo_hard_dipsw = value & 0xff; else neogeo_dipswitch = value & 0xff; }
static dipswitch_t *ext_dip_load(void) { return load_dipswitch(); }
#endif

typedef struct
{
	const char *label;
	int bank;
	uint32_t mask;
	uint32_t count;
	uint32_t values[LIBRETRO_EXT_DIP_MAX_SETTINGS];
	const char *names[LIBRETRO_EXT_DIP_MAX_SETTINGS];
} ext_dip_field;

static ext_dip_field ext_dips[EXT_MAX_DIPS];
static int ext_dip_count = -1;
static uint32_t ext_dip_override_mask[EXT_DIP_BANKS];
static uint32_t ext_dip_override_value[EXT_DIP_BANKS];

static void ext_dip_banks_read(uint32_t *banks)
{
	for (int b = 0; b < EXT_DIP_BANKS; b++)
		banks[b] = ext_dip_bank_get(b);
}

static void ext_dip_banks_write(const uint32_t *banks)
{
	for (int b = 0; b < EXT_DIP_BANKS; b++)
		ext_dip_bank_set(b, banks[b]);
}

static void ext_dip_build(void)
{
	uint32_t saved[EXT_DIP_BANKS], words[LIBRETRO_EXT_DIP_MAX_SETTINGS][EXT_DIP_BANKS];
	uint8_t base[64];
	dipswitch_t *table;
	int entries = 0;

	if (ext_dip_count >= 0 || !ext_running())
		return;
	ext_dip_count = 0;
	ext_dip_banks_read(saved);
	if (!(table = ext_dip_load()))
		return;

	while (entries < 64 && table[entries].label && table[entries].label[0])
	{
		base[entries] = table[entries].value;
		entries++;
	}

	for (int i = 0; i < entries && ext_dip_count < EXT_MAX_DIPS; i++)
	{
		dipswitch_t *dip = &table[i];
		ext_dip_field *field = &ext_dips[ext_dip_count];
		uint32_t settings, diff[EXT_DIP_BANKS] = {0};
		int bank = -1;

		if (!dip->enable || !dip->value_max || dip->label[0] == '\n')
			continue;
		settings = dip->value_max + 1u;
		if (settings > LIBRETRO_EXT_DIP_MAX_SETTINGS)
			settings = LIBRETRO_EXT_DIP_MAX_SETTINGS;

		for (uint32_t v = 0; v < settings; v++)
		{
			for (int j = 0; j < entries; j++)
				table[j].value = base[j];
			dip->value = (uint8_t)v;
			ext_dip_banks_write(saved);
			save_dipswitch();
			ext_dip_banks_read(words[v]);
			for (int b = 0; b < EXT_DIP_BANKS; b++)
				diff[b] |= words[v][b] ^ words[0][b];
		}
		for (int b = 0; b < EXT_DIP_BANKS && bank < 0; b++)
			if (diff[b])
				bank = b;
		if (bank < 0)
			continue;

		field->label = dip->label;
		field->bank = bank;
		field->mask = (dip->mask && !(diff[bank] & ~dip->mask)) ? dip->mask : diff[bank];
		field->count = settings;
		for (uint32_t v = 0; v < settings; v++)
		{
			field->values[v] = words[v][bank] & field->mask;
			field->names[v] = v < MAX_DIPSWITCHS ? dip->values_label[v] : NULL;
		}
		ext_dip_count++;
	}

	for (int j = 0; j < entries; j++)
		table[j].value = base[j];
	ext_dip_banks_write(saved);
}

static int ext_get_dip_count_impl(void)
{
	ext_dip_build();
	return ext_dip_count > 0 ? ext_dip_count : 0;
}

static bool ext_get_dip_info_impl(int index, struct libretro_ext_dip_info *out)
{
	const ext_dip_field *field;

	if (!out || index < 0 || index >= ext_get_dip_count_impl())
		return false;
	field = &ext_dips[index];
	memset(out, 0, sizeof(*out));
	ext_copy_string(out->name, sizeof(out->name), field->label);
	ext_copy_string(out->port_tag, sizeof(out->port_tag), ext_dip_port_tags[field->bank]);
	out->mask = field->mask;
	out->current_value = ext_dip_bank_get(field->bank) & field->mask;
	out->default_value = ext_dip_defaults[field->bank] & field->mask;
	out->setting_count = field->count;
	for (uint32_t v = 0; v < field->count; v++)
	{
		struct libretro_ext_dip_setting *setting = &out->settings[v];
		if (field->names[v])
			ext_copy_string(setting->name, sizeof(setting->name), field->names[v]);
		else
			snprintf(setting->name, sizeof(setting->name), "%u", (unsigned)v);
		setting->value = field->values[v];
	}
	return true;
}

/* The value survives core-option refreshes and resets until the next content load. */
static bool ext_set_dip_value_impl(int index, uint32_t value)
{
	const ext_dip_field *field;
	int bank;

	if (index < 0 || index >= ext_get_dip_count_impl())
		return false;
	field = &ext_dips[index];
	bank = field->bank;
	value &= field->mask;
	ext_dip_override_mask[bank] |= field->mask;
	ext_dip_override_value[bank] = (ext_dip_override_value[bank] & ~field->mask) | value;
	ext_dip_bank_set(bank, (ext_dip_bank_get(bank) & ~field->mask) | value);
	return true;
}

void lr_ext_apply_dip_overrides(void)
{
	for (int b = 0; b < EXT_DIP_BANKS; b++)
		if (ext_dip_override_mask[b])
			ext_dip_bank_set(b, (ext_dip_bank_get(b) & ~ext_dip_override_mask[b]) | ext_dip_override_value[b]);
}

static void ext_dip_reset(void)
{
	ext_dip_count = -1;
	memset(ext_dip_override_mask, 0, sizeof(ext_dip_override_mask));
	memset(ext_dip_override_value, 0, sizeof(ext_dip_override_value));
}

#else /* CPS2 / NCDZ: no DIP switches */

static int ext_get_dip_count_impl(void) { return 0; }
static bool ext_get_dip_info_impl(int index, struct libretro_ext_dip_info *out) { (void)index; (void)out; return false; }
static bool ext_set_dip_value_impl(int index, uint32_t value) { (void)index; (void)value; return false; }
void lr_ext_apply_dip_overrides(void) {}
static void ext_dip_reset(void) {}

#endif


/******************************************************************************
	Rollback (unsupported: this port has no savestates)
******************************************************************************/

static uint64_t ext_get_rollback_serialize_size_impl(void) { return 0; }
static bool ext_rollback_serialize_impl(void *data, uint64_t size) { (void)data; (void)size; return false; }
static bool ext_rollback_unserialize_impl(const void *data, uint64_t size) { (void)data; (void)size; return false; }
static bool ext_rollback_self_test_impl(void) { return false; }

static bool ext_rollback_get_info_impl(struct retro_ext_rollback_info *info) { (void)info; return false; }
static uint64_t ext_rollback_get_state_size_impl(void) { return 0; }
static bool ext_rollback_serialize_blob_impl(void *destination, uint64_t destination_size, uint64_t frame_number)
{
	(void)destination; (void)destination_size; (void)frame_number;
	return false;
}
static bool ext_rollback_unserialize_blob_impl(const void *source, uint64_t source_size)
{
	(void)source; (void)source_size;
	return false;
}
static uint64_t ext_rollback_get_delta_max_size_impl(void) { return 0; }
static bool ext_rollback_create_delta_impl(const void *from_state, uint64_t from_state_size, uint64_t from_frame,
										   const void *to_state, uint64_t to_state_size, uint64_t to_frame,
										   void *delta_destination, uint64_t delta_capacity, uint64_t *delta_size)
{
	(void)from_state; (void)from_state_size; (void)from_frame; (void)to_state; (void)to_state_size;
	(void)to_frame; (void)delta_destination; (void)delta_capacity;
	if (delta_size) *delta_size = 0;
	return false;
}
static bool ext_rollback_apply_delta_impl(void *state_in_out, uint64_t state_size, const void *delta, uint64_t delta_size,
										  uint64_t expected_from_frame, uint64_t *resulting_frame)
{
	(void)state_in_out; (void)state_size; (void)delta; (void)delta_size; (void)expected_from_frame;
	(void)resulting_frame;
	return false;
}
static bool ext_rollback_diagnostics;
static void ext_rollback_set_diagnostics_enabled_impl(bool enabled) { ext_rollback_diagnostics = enabled; }
static bool ext_rollback_get_diagnostics_enabled_impl(void) { return ext_rollback_diagnostics; }


/******************************************************************************
	Core lifecycle hooks
******************************************************************************/

void lr_ext_reset(void)
{
	ext_reset_debug_state();
	ext_dip_reset();
}

void lr_ext_frame(void)
{
	ext_update_hooks();
}

void lr_ext_set_memory_maps(retro_environment_t environ_cb)
{
	struct retro_memory_map mmap;
	unsigned n = 0;

#define EXT_MMAP(p, s, l, f) \
	ext_mmap_descs[n++] = (struct retro_memory_descriptor){ .flags = (f), .ptr = (p), .start = (s), .len = (l) }

	if (!environ_cb || !ext_running())
		return;
	memset(ext_mmap_descs, 0, sizeof(ext_mmap_descs));
#if (EMU_SYSTEM == CPS1 || EMU_SYSTEM == CPS2)
	EXT_MMAP(cps1_ram, 0xff0000, sizeof(cps1_ram), RETRO_MEMDESC_SYSTEM_RAM | RETRO_MEMDESC_BIGENDIAN);
	EXT_MMAP(cps1_gfxram, 0x900000, sizeof(cps1_gfxram), RETRO_MEMDESC_VIDEO_RAM | RETRO_MEMDESC_BIGENDIAN);
#endif
#if (EMU_SYSTEM == CPS2)
	EXT_MMAP(cps2_ram, 0x660000, 0x4000, RETRO_MEMDESC_SYSTEM_RAM | RETRO_MEMDESC_BIGENDIAN);
#endif
#if (EMU_SYSTEM == MVS)
	EXT_MMAP(neogeo_ram, 0x100000, sizeof(neogeo_ram), RETRO_MEMDESC_SYSTEM_RAM | RETRO_MEMDESC_BIGENDIAN);
	EXT_MMAP(neogeo_sram16, 0xd00000, sizeof(neogeo_sram16), RETRO_MEMDESC_SAVE_RAM | RETRO_MEMDESC_BIGENDIAN);
#endif
#if (EMU_SYSTEM == NCDZ)
	if (memory_region_cpu1)
		EXT_MMAP(memory_region_cpu1, 0x000000, memory_length_cpu1, RETRO_MEMDESC_SYSTEM_RAM | RETRO_MEMDESC_BIGENDIAN);
#endif
#undef EXT_MMAP

	if (!n)
		return;
	mmap.descriptors = ext_mmap_descs;
	mmap.num_descriptors = n;
	if (environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap))
		lr_log(RETRO_LOG_INFO, "libretro_ext: SET_MEMORY_MAPS registered %u descriptor(s)\n", n);
	else
		lr_log(RETRO_LOG_DEBUG, "libretro_ext: SET_MEMORY_MAPS not supported by frontend\n");
}


/******************************************************************************
	Exported tables
******************************************************************************/

static const struct libretro_ext_api ext_api =
{
	5,
	sizeof(struct libretro_ext_api),

	ext_get_driver_name_impl,
	ext_cpu_count_impl,
	ext_get_cpu_tag_impl,
	ext_get_cpu_pc_impl,

	ext_read_u8_impl,
	ext_read_u16_impl,
	ext_read_u32_impl,

	ext_write_u8_impl,
	ext_write_u16_impl,
	ext_write_u32_impl,

	ext_get_region_count_impl,
	ext_get_region_tag_impl,
	ext_get_region_size_impl,

	ext_read_region_impl,
	ext_write_region_impl,

	ext_get_frame_number_impl,
	ext_get_time_attoseconds_impl,

	ext_get_cpu_total_cycles_by_tag_impl,

	ext_clear_watch_rules_impl,
	ext_add_watch_rule_impl,
	ext_get_last_watch_hit_impl,
	ext_clear_last_watch_hit_impl,
	ext_check_watch_hit_impl,

	ext_get_dip_count_impl,
	ext_get_dip_info_impl,
	ext_set_dip_value_impl,

	ext_set_debug_extensions_enabled_impl,
	ext_get_debug_extensions_enabled_impl,

	ext_trigger_timing_capture_impl,

	ext_get_cpu_pc_by_tag_impl,
	ext_read_cpu_state_u64_by_index_impl,
	ext_read_cpu_state_u64_by_tag_impl,
	ext_read_cpu_register_by_tag_impl,

	ext_write_cpu_state_u64_by_index_impl,
	ext_write_cpu_state_u64_by_tag_impl,
	ext_write_cpu_register_by_tag_impl,

	ext_add_inject_rule_impl,
	ext_clear_inject_rules_impl,

	ext_add_inject_rule_ex_impl,
	ext_get_static_region_count_impl,
	ext_get_static_region_info_impl,
	ext_get_share_count_impl,
	ext_get_share_tag_impl,
	ext_get_share_size_impl,
	ext_get_share_flags_impl,
	ext_get_share_info_impl,
	ext_read_share_impl,
	ext_write_share_impl,
	ext_get_rollback_serialize_size_impl,
	ext_rollback_serialize_impl,
	ext_rollback_unserialize_impl,
	ext_rollback_self_test_impl
};

static const struct libretro_ext_rollback_api ext_rollback_api =
{
	1,
	sizeof(struct libretro_ext_rollback_api),
	ext_rollback_get_info_impl,
	ext_rollback_get_state_size_impl,
	ext_rollback_serialize_blob_impl,
	ext_rollback_unserialize_blob_impl,
	ext_rollback_get_delta_max_size_impl,
	ext_rollback_create_delta_impl,
	ext_rollback_apply_delta_impl,
	ext_rollback_self_test_impl,
	ext_rollback_set_diagnostics_enabled_impl,
	ext_rollback_get_diagnostics_enabled_impl
};

static void ext_log_api_signature_once(const char *entrypoint)
{
	static bool logged;
	if (logged)
		return;
	logged = true;
	lr_log(RETRO_LOG_INFO, "libretro_ext: %s exported ABI=%u struct_size=%u\n",
		   entrypoint, (unsigned)ext_api.abi_version, (unsigned)ext_api.sizeof_struct);
}

LIBRETRO_EXT_EXPORT const struct libretro_ext_api *libretro_ext_get_api(void)
{
	ext_log_api_signature_once("libretro_ext_get_api");
	return &ext_api;
}

LIBRETRO_EXT_EXPORT const struct libretro_ext_api *libretro_ext_get_api_v5(void)
{
	ext_log_api_signature_once("libretro_ext_get_api_v5");
	return &ext_api;
}

LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_abi_version(void)
{
	return ext_api.abi_version;
}

LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_struct_size(void)
{
	return ext_api.sizeof_struct;
}

LIBRETRO_EXT_EXPORT const struct libretro_ext_rollback_api *libretro_ext_get_rollback_api(void)
{
	return &ext_rollback_api;
}

LIBRETRO_EXT_EXPORT const struct libretro_ext_rollback_api *libretro_ext_get_rollback_api_v1(void)
{
	return &ext_rollback_api;
}

LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_rollback_api_abi_version(void)
{
	return ext_rollback_api.abi_version;
}

LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_rollback_api_struct_size(void)
{
	return ext_rollback_api.sizeof_struct;
}

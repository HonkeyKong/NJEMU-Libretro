> Libretro implementation and validation are documented in [LIBRETRO.md](LIBRETRO.md).
> The older platform status tables below describe the PSP/PS2/desktop port.

# NJEMU Porting Plan

This document outlines the remaining work needed to complete the cross-platform port of NJEMU.

---

## Current Status Summary

> **Milestone: All emulator cores are fully ported to all platforms (PSP, PS2, Desktop).** The next phase is GUI/menu system porting.

### Emulator Core Porting

| Emulator | PSP | PS2 | PC | Notes |
|----------|-----|-----|-----|-------|
| **MVS** | ✅ Complete | ✅ Core done | ✅ Core done | Sprite rendering ported |
| **NCDZ** | ✅ Complete | ✅ Core done | ✅ Core done | Sprite rendering ported |
| **CPS1** | ✅ Complete | ✅ Core done | ✅ Core done | Sprite rendering ported (incl. stars) |
| **CPS2** | ✅ Complete | ✅ Core done | ✅ Core done | Sprite rendering ported (incl. Z-buffer masking) |

### Platform Drivers

| Driver | PSP | PS2 | PC | Purpose |
|--------|-----|-----|-----|---------|
| `*_platform.c` | ✅ | ✅ | ✅ | Platform init/main loop |
| `*_video.c` | ✅ | ✅ | ✅ | Screen rendering |
| `*_audio.c` | ✅ | ✅ | ✅ | Sound output |
| `*_input.c` | ✅ | ✅ | ✅ | Controller input |
| `*_thread.c` | ✅ | ✅ | ✅ | Threading |
| `*_ticker.c` | ✅ | ✅ | ✅ | Timing |
| `*_power.c` | ✅ | ✅ | ✅ | Power management |
| `*_ui_text.c` | ✅ | ✅ | ✅ | Basic text output |
| `*_no_gui.c` | ✅ | ✅ | ✅ | Stub UI for testing |

### GUI/Menu System

| Component | PSP | PS2 | PC | Files |
|-----------|-----|-----|-----|-------|
| UI Framework | ✅ | ❌ | ❌ | `ui.c/h`, `ui_draw.c/h` |
| Menu System | ✅ | ❌ | ❌ | `ui_menu.c/h` |
| File Browser | ✅ | ❌ | ❌ | `filer.c/h` |
| Configuration | ✅ | ❌ | ❌ | `config.c/h`, `config/*.c` |
| Font Rendering | ✅ | ❌ | ❌ | `font/*.c` |
| PNG Loading | ✅ | ❌ | ❌ | `png.c/h` |
| Wallpapers | ✅ | ❌ | ❌ | `wallpaper.c/h` |
| Icons | ✅ | ❌ | ❌ | `icon/*.c` |

---

## Phase 1: Complete Emulator Core Ports

### Priority: HIGH

The emulator cores should be ported first since this is the primary functionality.

### 1.1 Port CPS1 Sprite Rendering ✅ COMPLETE

**Structure:**
- `src/cps1/sprite_common.h` - Shared declarations (constants, structures, extern variables)
- `src/cps1/sprite_common.c` - Platform-agnostic code (hash table management, software rendering)
- `src/cps1/psp_sprite.c` - PSP-specific rendering (GU commands, swizzling)
- `src/cps1/ps2_sprite.c` - PS2-specific rendering (GSKit primitives)
- `src/cps1/desktop_sprite.c` - Desktop-specific rendering (SDL2)

**Completed work:**
- PS2 sprite rendering with GSKit (sprites, scrolls, stars via `gsKit_prim_list_points`)
- Desktop sprite rendering with SDL2 (sprites, scrolls, stars via `SDL_RenderDrawPoint`)
- Fixed alignment crash: consolidated `ALIGN_DATA` → `ALIGN16_DATA` for x86_64 SSE compatibility
- Fixed frame clearing on Desktop (`SDL_RenderClear` in `startWorkFrame`)

#### CPS1 Key Differences from MVS/NCDZ

| Feature | MVS/NCDZ | CPS1 |
|---------|----------|------|
| Texture atlases | 2 (FIX + SPR) | 5 (OBJECT + SCROLL1/2/3 + SCROLLH) |
| Tile sizes | 8×8 and 16×16 only | 8×8, 16×16, and 32×32 |
| High priority layer | None | SCROLLH (16-bit direct color) |
| CLUT banks | 2 × 256 palettes | Per-layer palettes (0-31, 32-63, etc.) |
| Graphics format | Nibble-packed | Interleaved planar (0,4,1,5,2,6,3,7) |
| Per-line effects | None | SCROLL2 parallax scrolling |
| Priority masks | None | tpens bitmask for SCROLLH |
| Stars layer | None | Point rendering for star fields |

#### CPS1 Porting Considerations

1. **Multiple texture atlases:** Must allocate and manage 5 separate textures
2. **SCROLLH direct color:** Not indexed - must decode to RGB at cache time
3. **Software rendering path:** SCROLL2 parallax requires CPU rendering when clip < 16 lines
4. **Interleaved pixel format:** Decode pattern `dst[0,4,1,5,2,6,3,7]` is hardware-defined
5. **CLUT bank switching:** Each layer uses different palette ranges, track CLUT changes for batching

#### CPS1 Texture Atlas Summary

| Layer | Tile Size | Texture Size | Format | UV Calculation |
|-------|-----------|--------------|--------|----------------|
| OBJECT | 16×16 | 512×512 | 8-bit indexed | u=(idx&0x1f)<<4, v=(idx&0x3e0)>>1 |
| SCROLL1 | 8×8 | 512×512 | 8-bit indexed | u=(idx&0x3f)<<3, v=(idx&0xfc0)>>3 |
| SCROLL2 | 16×16 | 512×512 | 8-bit indexed | u=(idx&0x1f)<<4, v=(idx&0x3e0)>>1 |
| SCROLL3 | 32×32 | 512×512 | 8-bit indexed | u=(idx&0xf)<<5, v=(idx&0xf0)<<1 |
| SCROLLH | varies | 512×192 | 16-bit RGB | varies by tile size |

#### CPS1 CLUT Organization

| Layer | Palette Range | CLUT Bank 0 | CLUT Bank 1 |
|-------|---------------|-------------|-------------|
| OBJECT | 0-31 | clut[0<<4] | clut[16<<4] |
| SCROLL1 | 32-63 | clut[32<<4] | clut[48<<4] |
| SCROLL2 | 64-95 | clut[64<<4] | clut[80<<4] |
| SCROLL3 | 96-127 | clut[96<<4] | clut[112<<4] |

#### CPS1 Work Buffers Memory Layout

```
work_frame ─┬─ scrbitmap    (512 × 272 × 2 bytes) = 278,528 bytes
            ├─ tex_scrollh  (512 × 192 × 2 bytes) = 196,608 bytes
            ├─ tex_object   (512 × 512 × 1 byte)  = 262,144 bytes
            ├─ tex_scroll1  (512 × 512 × 1 byte)  = 262,144 bytes
            ├─ tex_scroll2  (512 × 512 × 1 byte)  = 262,144 bytes
            └─ tex_scroll3  (512 × 512 × 1 byte)  = 262,144 bytes
                                          Total ≈ 1.5 MB
```

### 1.2 Port CPS2 Sprite Rendering ✅ COMPLETE

**Structure (matching CPS1 pattern):**
- `src/cps2/sprite_common.h` - Shared declarations (constants, structures, extern variables)
- `src/cps2/sprite_common.c` - Platform-agnostic code (hash table management, software rendering)
- `src/cps2/psp_sprite.c` - PSP-specific rendering (GU commands, swizzling)
- `src/cps2/ps2_sprite.c` - PS2-specific rendering (GSKit primitives, Z-buffer via GS ZBUF register)
- `src/cps2/desktop_sprite.c` - Desktop-specific rendering (SDL2, struct Vertex arrays)

**Completed work:**
- Refactored `psp_sprite.c` into sprite_common + platform-specific files
- Added `emu_layer_textures[]`, `emu_layer_textures_count`, and `emu_clut_info` to `src/cps2/cps2.c`
- Added `TEXTURE_LAYER_INDEX` enum to `src/cps2/cps2.h`
- PS2 sprite rendering with GSKit: priority linked-lists, Z-buffer masking via GS ZBUF register (ZTST modes)
- Desktop sprite rendering with SDL2: priority linked-lists, CLUT batching, struct Vertex arrays
- Implemented `desktop_clearFrame` for Z-buffer masking support
- Implemented PS2 depth test functions (`enableDepthTest`, `disableDepthTest`, `clearDepthBuffer`, `clearColorBuffer`)
- Unified cache format across all platforms (web converter, romcnv)

#### CPS2 Key Differences from CPS1

| Feature | CPS1 | CPS2 |
|---------|------|------|
| Texture layers | 5 (incl. SCROLLH) | 4 (no SCROLLH) |
| Star field | Yes (blitPoints) | No |
| Object priority | Simple ordering | 8-level priority with Z-buffer masking |
| Object RAM | Single | Double-buffered (2 banks) |
| Palette delay | No | Yes (some games, `flags & 2`) |
| Palette entries | `6*32*16 = 3072` | `4*32*16 = 2048` |
| ROM encryption | No | Yes |
| Scroll3 kludges | None | SSF2T, XMCOTA, DIMAHOO base adjustments |

#### CPS2 Texture Atlas Summary

| Layer | Tile Size | Texture Size | Format | GFX Offset |
|-------|-----------|--------------|--------|------------|
| OBJECT | 16x16 | 512x512 | 8-bit indexed | `code << 7` (128 bytes/tile) |
| SCROLL1 | 8x8 | 512x512 | 8-bit indexed | `code << 6` (64 bytes/tile) |
| SCROLL2 | 16x16 | 512x512 | 8-bit indexed | `code << 7` (128 bytes/tile) |
| SCROLL3 | 32x32 | 512x512 | 8-bit indexed | `code << 9` (512 bytes/tile) |

#### CPS2 CLUT Organization

Two banks per layer selected by `attr & 0x10`. Each palette has 16 colors (4-bit tiles).

| Layer | Bank 0 offset | Bank 1 offset | Palette range |
|-------|---------------|---------------|---------------|
| OBJECT | `clut[0]` | `clut[16<<4]` | 0-31 |
| SCROLL1 | `clut[32<<4]` | `clut[48<<4]` | 32-63 |
| SCROLL2 | `clut[64<<4]` | `clut[80<<4]` | 64-95 |
| SCROLL3 | `clut[96<<4]` | `clut[112<<4]` | 96-127 |

#### CPS2 Work Buffers Memory Layout

```
work_frame ─┬─ scrbitmap    (512 × 272 × 2 bytes) = 278,528 bytes
            ├─ tex_object   (512 × 512 × 1 byte)  = 262,144 bytes
            ├─ tex_scroll1  (512 × 512 × 1 byte)  = 262,144 bytes
            ├─ tex_scroll2  (512 × 512 × 1 byte)  = 262,144 bytes
            └─ tex_scroll3  (512 × 512 × 1 byte)  = 262,144 bytes
                                          Total ≈ 1.3 MB
```

#### CPS2 Porting Considerations

1. **Simpler than CPS1:** No SCROLLH layer, no star field — 4 layers instead of 5
2. **Priority masking:** `cps2_has_mask` enables depth-buffer rendering for objects where priority reverses — must be handled per-platform
3. **Scroll2 dual rendering:** Small clip areas (<16 lines) use software rendering directly to scrbitmap; full tiles use hardware GPU — desktop/PS2 can simplify to always software or always hardware
4. **Object priority queuing:** Objects are queued into 8 priority buckets then rendered interleaved with scroll layers — the vidhrdw.c orchestration is platform-agnostic
5. **Palette delay:** Some games (xmcota, msh, mshvsf, mvsc, xmvsf) buffer object palettes one frame behind — handled in vidhrdw.c, transparent to sprite code
6. **Color conversion:** CPS2 uses BRGB 16-bit → `video_clut16[65536]` lookup → 15-bit RGB; this is in vidhrdw.c and is platform-agnostic

### 1.3 Update CMakeLists.txt ✅ COMPLETE

All four targets (MVS, CPS1, CPS2, NCDZ) are fully configured in CMakeLists.txt for all three platforms (PSP, PS2, Desktop). CPS2 uses `cps2/${PLATFORM_LOWER}_sprite.c` pattern like the other targets.

---

## Phase 2: GUI Abstraction Layer

### Priority: MEDIUM

Before porting the GUI, create an abstraction layer to separate platform-specific code.

### 2.1 Define GUI Driver Interface

**Create:** `src/common/gui_driver.h`

Define interfaces for:
```c
// Drawing primitives
void gui_draw_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_text(int x, int y, const char *text, uint32_t color);
void gui_draw_image(int x, int y, void *image);
void gui_fill_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_line(int x1, int y1, int x2, int y2, uint32_t color);

// Font handling
void gui_font_init(void);
int gui_font_get_width(const char *text);
int gui_font_get_height(void);

// Image loading
void *gui_load_png(const char *path);
void gui_free_image(void *image);

// Screen management
void gui_begin_frame(void);
void gui_end_frame(void);
```

### 2.2 Refactor PSP UI Code

**Goal:** Separate platform-agnostic logic from PSP-specific rendering

1. Move menu logic to `src/common/ui_menu_common.c`
2. Move file browser logic to `src/common/filer_common.c`
3. Keep PSP rendering in `src/psp/ui_draw.c`

### 2.3 Create Platform GUI Drivers

**Files to create:**
- `src/ps2/ps2_gui.c` - PS2 GSKit implementation
- `src/desktop/desktop_gui.c` - SDL2 implementation

---

## Phase 3: Port Menu/GUI System

### Priority: MEDIUM-LOW

Once the GUI abstraction is in place, port the actual menu system.

### 3.1 Port File Browser

**Dependencies:** GUI driver, font rendering, input handling

**Features:**
- Directory listing
- ROM file selection
- Parent ROM detection
- Preview images (optional)

### 3.2 Port Configuration Menus

**Files to adapt:**
- Game configuration (`config/mvs.c`, `config/cps.c`, `config/ncdz.c`)
- Key configuration
- Display settings
- Sound settings

### 3.3 Port In-Game Menu

**Features:**
- Save/Load state
- Reset game
- Return to browser
- Quick settings

### 3.4 Port Font Rendering

**Options:**
1. **SDL_ttf** (PC) - Use TrueType fonts
2. **Bitmap fonts** (PS2) - Port existing bitmap font system
3. **Unified approach** - Create bitmap font renderer for all platforms

### 3.5 Port Image Loading (PNG)

**Options:**
1. Use **libpng** directly (works on all platforms)
2. Use **stb_image.h** (single-header, no dependencies)
3. Use **SDL_image** (PC only)

**Recommendation:** `stb_image.h` for simplicity and portability

---

## Phase 4: Additional Features

### Priority: LOW

### 4.1 Save States

**Current status:** Implemented for PSP

**Files:** `src/common/state.c/h`

**Work needed:**
- Verify file I/O works on PS2/PC
- Test save/load functionality
- May need endianness handling for cross-platform saves

### 4.2 Cheats

**Current status:** Implemented for PSP

**Files:** `src/common/cheat.c/h` (if exists), cheat loading in `filer.c`

**Work needed:**
- Port cheat file loading
- Port cheat menu

### 4.3 Command Lists

**Current status:** PSP only

**Files:** `src/common/cmdlist.c/h`

**Work needed:**
- Port command list display
- May require font rendering first

---

## Phase 5: Platform-Specific Optimizations

### Priority: LOW (after everything works)

### 5.1 PS2 Optimizations

- Use VU0/VU1 for sprite transformations
- Optimize DMA transfers
- Memory management for limited RAM

### 5.2 PC Optimizations

- Consider OpenGL/Vulkan renderer
- Shader-based rendering
- Resolution scaling

---

## Recommended Task Order

### Immediate (Core Functionality)

1. ✅ ~~MVS core for PS2/PC~~ (DONE)
2. ✅ ~~NCDZ core for PS2/PC~~ (DONE)
3. ✅ ~~CPS1 sprite rendering for PS2~~ (DONE)
4. ✅ ~~CPS1 sprite rendering for PC~~ (DONE)
5. ✅ ~~CPS2 sprite rendering refactoring (extract sprite_common)~~ (DONE)
6. ✅ ~~CPS2 sprite rendering for PS2~~ (DONE)
7. ✅ ~~CPS2 sprite rendering for PC~~ (DONE)

### Short-term (Basic GUI)

7. 🔲 Define GUI driver interface
8. 🔲 Implement PS2 GUI driver (basic)
9. 🔲 Implement PC GUI driver (SDL2)
10. 🔲 Port file browser
11. 🔲 Port basic font rendering

### Medium-term (Full GUI)

12. 🔲 Port configuration menus
13. 🔲 Port in-game menu
14. 🔲 Port PNG loading
15. 🔲 Port wallpaper system

### Long-term (Polish)

16. 🔲 Save states
17. 🔲 Cheats
18. 🔲 Command lists
19. 🔲 Platform optimizations

---

## File Creation Checklist

### CPS1 Porting
- [x] `src/cps1/sprite_common.h` - Created (shared declarations)
- [x] `src/cps1/sprite_common.c` - Created (platform-agnostic code)
- [x] `src/cps1/ps2_sprite.c` - Created (PS2 GSKit rendering)
- [x] `src/cps1/desktop_sprite.c` - Created (SDL2 rendering)

### CPS2 Porting
- [x] `src/cps2/sprite_common.h` - Created (shared declarations, hash tables, texture sizes)
- [x] `src/cps2/sprite_common.c` - Created (platform-agnostic code, software rendering)
- [x] `src/cps2/ps2_sprite.c` - Created (PS2 GSKit rendering with Z-buffer masking)
- [x] `src/cps2/desktop_sprite.c` - Created (SDL2 rendering with priority linked-lists)

### GUI Abstraction
- [ ] `src/common/gui_driver.h`
- [ ] `src/common/gui_driver.c`
- [ ] `src/common/ui_menu_common.c`
- [ ] `src/common/filer_common.c`

### PS2 GUI
- [ ] `src/ps2/ps2_gui.c`
- [ ] `src/ps2/ps2_font.c`

### PC GUI
- [ ] `src/desktop/desktop_gui.c`
- [ ] `src/desktop/desktop_font.c`

---

## Notes

### Why start with sprite rendering?

The sprite rendering is the core visual output of each emulator. It's:
- Self-contained (doesn't depend on GUI)
- Essential for the emulator to be usable
- A good test of the platform driver architecture

### Why abstract the GUI?

The PSP GUI code is tightly coupled to PSP-specific APIs (GU). Creating an abstraction:
- Allows code reuse across platforms
- Makes maintenance easier
- Follows the same pattern used for the emulator core

### Alternative: Skip GUI for PS2/PC

If GUI porting is too complex, an alternative approach:
1. Keep `*_no_gui.c` approach
2. Use command-line arguments or config files
3. Focus on emulation accuracy instead

This is a valid approach for a "debug/development" platform.

---

## Resources

### PS2 Development
- [PS2DEV GitHub](https://github.com/ps2dev)
- [GSKit documentation](https://github.com/ps2dev/gsKit)

### SDL2 Development
- [SDL2 Wiki](https://wiki.libsdl.org/)
- [Lazy Foo SDL Tutorials](https://lazyfoo.net/tutorials/SDL/)

### Reference Implementations
- `src/mvs/ps2_sprite.c` - PS2 sprite rendering example
- `src/mvs/desktop_sprite.c` - SDL2 sprite rendering example
- `src/cps1/sprite_common.c` - Example of platform-agnostic sprite code separation
- `src/cps2/sprite_common.c` - CPS2 platform-agnostic sprite code (Z-buffer masking support)
- `src/cps2/ps2_sprite.c` - CPS2 PS2 rendering with GS Z-buffer
- `src/cps2/desktop_sprite.c` - CPS2 SDL2 rendering with priority linked-lists
- `src/ncdz/sprite_common.c` - Another example of sprite code separation


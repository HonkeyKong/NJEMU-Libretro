# NJEMU libretro cores

This platform builds the existing CPS1, CPS2, MVS and NCDZ machines as separate
frontend-driven shared libraries. It uses the official [libretro API header](https://github.com/libretro/libretro-common/blob/master/include/libretro.h)
and [core API documentation](https://docs.libretro.com/development/cores/developing-cores/).
The original emulator licensing and attribution remain in effect. The vendored
`libretro.h` retains its MIT license; `minimp3.h` retains its CC0 notice.

## Build

CMake 3.13 or newer, a C99 GCC/Clang toolchain, and zlib development files are required. SDL,
libmad, window-system libraries, and PSP/PS2 SDKs are not used by these cores.

```sh
for target in CPS1 CPS2 MVS NCDZ; do
  lower=$(printf '%s' "$target" | tr '[:upper:]' '[:lower:]')
  cmake -S . -B "build_libretro_$lower" \
    -DPLATFORM=LIBRETRO -DTARGET="$target" -DCMAKE_BUILD_TYPE=Release
  cmake --build "build_libretro_$lower" -j4
  ctest --test-dir "build_libretro_$lower" --output-on-failure
done
```

Outputs on Linux:

- `build_libretro_cps1/njemu_cps1_libretro.so`
- `build_libretro_cps2/njemu_cps2_libretro.so`
- `build_libretro_mvs/njemu_mvs_libretro.so`
- `build_libretro_ncdz/njemu_ncdz_libretro.so`

CMake uses the platform's shared-library extension (`.dll` on Windows, `.dylib`
on macOS), without a `lib` prefix. Linux x86_64 is the only platform built and
executed during this port. ARM64 and Windows/MinGW are intended targets, but
have not been built or tested. MSVC support in the legacy CPU cores is not
established. There are no architecture-specific compiler flags in this build.
`BUILD_TESTING=OFF` omits the POSIX test executables and generated fixtures.
`SAVE_STATE`, `ADHOC`, `COMMAND_LIST`, `LARGE_MEMORY` and `KERNEL_MODE` are rejected
for LIBRETRO; existing PSP/PS2/DESKTOP CMake paths keep their existing options.

## Content and directories

The frontend must provide an absolute content filename, system directory, and
writable save directory. The core never changes the working directory. Use the
repository's ROM definitions and matching ROM sets; another emulator's current
ROM-set version is not necessarily compatible.

```text
<system>/njemu/
    rominfo.cps1                 # from resources/cps1
    rominfo.cps2                 # from resources/cps2
    rominfo.mvs                  # from resources/mvs
    neogeo.zip                   # your supported MVS BIOS and supporting ROMs
    neocd.bin                    # your NCDZ BIOS (loader CRC df9de490)
    000-lo.lo                    # your NCDZ zoom ROM (loader CRC 5a86cff2)

<content directory>/
    <game>.zip
    <parent>.zip                 # required for split clone sets
    cache/                      # optional existing MVS converter output

<save>/njemu/<TARGET>/           # TARGET is uppercase: CPS1, CPS2, MVS, NCDZ
    nvram/<game>.nv              # CPS EEPROM / MVS SRAM
    memcard/<game>.bin           # MVS memory card
    backup.bin                  # NCDZ shared backup memory
    cache/                      # MVS cache fallback when content/cache is absent
```

CPS1, CPS2 and MVS accept ZIP archives. The filename stem identifies the game;
use a supported database name. Parent archives are searched next to the loaded
content. MVS BIOS ROMs can be in that directory's `neogeo.zip`, or in
`<system>/njemu/neogeo.zip`. No BIOS files are distributed with the core.

CPS2 loads its graphics ROMs from the selected archive and its parent archive,
then decodes them into memory during content loading. It does not require or
read CPS2 cache files. Startup may take longer and use more memory, especially
for large sets. Graphics are decoded in place; the largest CPS2 GFX1 region in
the supplied database is 32 MiB, in addition to program, audio, frontend and
other allocations. The existing cache path remains in the other platforms.
The full-memory decoder does not apply `romcnv`'s title-specific blank-tile
filtering and range pruning; their effect on real-game visuals needs comparison.

MVS retains its existing V24 graphics cache formats. Generate the cache using
[the MVS converter](romcnv/README_MVS.md) and place it in `content/cache`, or in
the per-target save `cache` directory when `content/cache` does not exist.
MVS uses `<game>_cache/` (`cache_info`, `crom`, optional `srom`/`vrom`) or ZIP
format. The runtime reads the cache synchronously and does not automatically
convert ROMs. Truncated cache reads fail cleanly.

NCDZ accepts either a ZIP containing the extracted disc files and `IPL.TXT`,
or the full path to an extracted `IPL.TXT`. Use the loader's original uppercase
filenames for extracted files on case-sensitive filesystems. Its PRG/FIX/SPR/
Z80/PCM/PAT data loaders are retained. This is **not** a CUE/BIN/ISO/CHD reader.
For example:

```text
<content>/my-disc/disc.zip        # IPL.TXT and disc data at archive root
<content>/my-disc/mp3/02.mp3
<content>/my-disc/mp3/03.mp3
# or:
<content>/my-disc/IPL.TXT
<content>/my-disc/<disc data files>
<content>/my-disc/mp3/02.mp3
```

MP3 tracks are discovered using the existing two-digit track filename pattern.
MP3 CDDA decoding, resampling and mixing are synchronous. Missing tracks are
silent and release command acknowledgement. There is one extracted disc per
load; no disk-control interface or image swapping is advertised. The legacy
animated loading screen and artificial CD speed limiter are disabled because
they execute nested CPU/display frames. Disc file transfers remain synchronous
and may make a frame slower while media data is loaded.

## Controls and options

Input is polled once per video frame. Every supported player has its own
RetroPad port. The existing game-specific port wiring and active-low values are
used, including CPS multiplayer/coin-chute arrangements and rotary inputs.
There are no keyboard requirements or PSP Start+Select/menu combinations.

| RetroPad | CPS1/CPS2 | MVS/NCDZ |
|---|---|---|
| D-pad | Directions | Directions; digital movement for special analog games |
| Y, X, L | Buttons 1, 2, 3 | C, D, unused |
| B, A, R | Buttons 4, 5, 6 | A, B, unused |
| Select | Coin | Coin (MVS), Select (NCDZ) |
| Start | Start | Start |
| L2 / R2 | Rotary left / right | Unused |
| P1 L3 | Service coin | Service coin (MVS) |
| P1 R3 | Test/service | Test (MVS) |

Input descriptors expose the game's supported maximum players/buttons after
loading. CPS2's EEPROM cabinet settings can further select the player/coin
configuration. Use the game's service screen for CPS2 EEPROM settings; these
persist with the game.

Core options:

- CPS1: DIP banks A/B/C, full hexadecimal byte values (`ff` default). Refer to
  the game's DIP manual or the existing `src/cps1/dipsw.c` definitions. Changes
  apply at the next frame; reset when the game requires it.
- MVS: DIP bank A; BIOS filename (`auto` scans the existing supported BIOS table;
  reload content to change); region `0=BIOS, 1=Japan, 2=USA, 3=Europe` and machine
  `0=BIOS, 1=AES, 2=MVS`. Reset to apply region/machine changes. Some BIOSes,
  including UniBIOS, retain their own region/mode controls.
- NCDZ: region `0=Japan, 1=USA, 2=Europe` (default USA; reset to apply), and CDDA
  enabled/disabled.

CPU scheduling, interrupts, timers, object buffering and palette updates remain
in the original machine code. A `retro_run()` invokes one timer/CPU frame;
there is no emulation or audio worker thread, internal frame limiter, automatic
frameskip, vsync wait, or standalone `main()` in the libretro build. The frontend
controls pacing. Audio is signed 16-bit interleaved stereo, with fractional
samples retained across frames. Unaccepted batch audio is dropped; it is never
accumulated into an unbounded queue.

| Machine | Visible pixels | Refresh rate | Audio rate |
|---|---|---|---|
| CPS1 | 384 x 224 | 60 Hz (existing machine timing) | 44100 Hz YM2151; 24000 Hz QSound |
| CPS2 | 384 x 224 | 59.633333 Hz | 24000 Hz |
| MVS / NCDZ | 320 x 224 | 59.185606 Hz | 44100 Hz |

The CPU work buffer is 512 x 272, separate from visible geometry and texture
atlases. All indexed, direct-color, point/star, clipping, scaling and depth
operations composite into that buffer; software row-scroll/sprite-line writes
share it. Native BGR5551 is explicitly converted to libretro XRGB8888 with
`width * 4` pitch. The frontend must accept XRGB8888. Aspect is 4:3 before
rotation. Vertical CPS games request frontend rotation (90 degrees counter-
clockwise); pixels are not also rotated internally. Hardware flip is applied
once. Neo Geo includes all 40 FIX columns and the full 320-pixel horizontal
raster rather than the desktop/PSP 304-pixel crop.

EEPROM, SRAM and memory-card persistence stays file-backed. Failed loads do not
commit machine persistent data. `RETRO_MEMORY_SAVE_RAM` is not exposed, avoiding
a second writer over the emulator's serial EEPROM / combined persistent files.
`RETRO_MEMORY_SYSTEM_RAM` exposes the main work RAM while content is loaded.

## State support and remaining validation

Save states are currently unsupported: `retro_serialize_size()` returns zero,
and both serialization functions return false. Rewind and run-ahead are not
advertised. The existing `SAVE_STATE` code was inspected: it includes CPU
registers/PC, RAM/banks, timer state/callback reconstruction, sound-chip state,
video state, and NCDZ/CDDA reload records, but combines memory-writing saves with
file-based loads and unvalidated mutation. It is not a safe in-memory API as-is.

Continuation checklist for states:

1. Replace the `state.h` save/load macros with bounded memory readers/writers,
   using a versioned per-content envelope, fixed size, and integrity checks.
2. Audit every machine's CPU/sound/timer/bank/interrupt and buffered object/
   palette coverage, including fractional sound timing and video cache rebuilds.
3. Reconstruct mappings and callback pointers rather than writing host pointers;
   validate an entire snapshot before applying it or provide rollback.
4. Extend NCDZ state handling to restore media files/transfers and the new MP3
   decoder/resampler position without serializing file handles or decoder
   platform objects.
5. Add save/load/replay tests before enabling serialization, rewind or run-ahead.

Real-game validation still needs user-supplied lawful CPS1/CPS2/MVS/NCDZ content
and BIOS files, including priority-mask scenes, stars/row scroll, palette delays,
Neo Geo raster/zoom/banking effects, multiple players, persistence across process
restart, and NCDZ in-game file transfers/CDDA commands. Generated CPU fixtures
exercise scheduling and callbacks; they do not establish commercial-game visual
or sound accuracy. ARM64, Windows, PSP and PS2 were not executed during this port.

## Validation performed (Linux x86_64, GCC 15.2)

The Release and ASan/UBSan runs each passed all 19 tests: CPS1 4/4, CPS2 10/10,
MVS 2/2, NCDZ 3/3. The original fixtures ran 1086 CPS1 frames (parent plus clone)
and 1086 CPS2 frames per complete test suite. Dynamic-symbol inspection found
exactly the 25 required libretro exports in each library, with no standalone
`main`, SDL, pthread, sleep or SDK symbols; Linux dependencies are libc, libm and
zlib. `git -c core.whitespace=cr-at-eol diff --check` passed.

The Release commands above build all four cores. Headless CTest checks load all
required ABI symbols with `RTLD_NOW`, callback registration, init/deinit,
null/missing/corrupt/unsupported content, partial initialization, repeated
load/unload/reset, pixel-format rejection, file-descriptor cleanup and unchanged
CWD. CPU renderer tests cover channel conversion, indexed transparency, flips,
direct-color tiles, points, and depth/color-mask behavior.

With Python available, original generated 68000/Z80 fixtures increment emulated
work RAM and run through the real CPU/timer path. Each frame test runs 543
frames over three loads, checking video/audio formats, one input poll per frame,
fractional audio counts, short audio batch writes and resets. CPS1 also tests
parent/clone loading. CPS2 loads both byte-lane and word-lane interleaved graphics from a parent and clone
without a cache, checks decoded pixels and tile usage, and rejects a missing
parent, invalid region size or ROM offset, corrupt archive member, and short
graphics member. NCDZ's standalone CDDA
test uses an original generated sine-wave MP3 when a local libmp3lame encoder is
available (the core itself does not link that encoder).

Sanitizer validation:

```sh
for target in CPS1 CPS2 MVS NCDZ; do
  lower=$(printf '%s' "$target" | tr '[:upper:]' '[:lower:]')
  cmake -S . -B "build_libretro_${lower}_asan" \
    -DPLATFORM=LIBRETRO -DTARGET="$target" -DCMAKE_BUILD_TYPE=Debug -DUSE_ASAN=ON
  cmake --build "build_libretro_${lower}_asan" -j4
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
    ctest --test-dir "build_libretro_${lower}_asan" --output-on-failure
done
```

LeakSanitizer is disabled here because this environment runs processes under
ptrace; ASan and UBSan remain enabled. These checks do not establish leak-free
behavior. Desktop regression commands:

```sh
for target in CPS1 CPS2 MVS; do
  lower=$(printf '%s' "$target" | tr '[:upper:]' '[:lower:]')
  cmake -S . -B "build_desktop_$lower" -DPLATFORM=DESKTOP -DTARGET="$target" -DCMAKE_BUILD_TYPE=Debug
  cmake --build "build_desktop_$lower" -j4
done
```

All three desktop targets build. Their pre-existing always-on CPS trace thread
is now opt-in via `NJEMU_DESKTOP_TRACE`, fixing the MVS link against CPS-only
symbols. Desktop NCDZ configuration was attempted with
`cmake -S . -B build_desktop_ncdz -DPLATFORM=DESKTOP -DTARGET=NCDZ` and is blocked
by the unavailable libmad development header/library. No gameplay ROM or BIOS
was downloaded, and no changes were pushed or published.

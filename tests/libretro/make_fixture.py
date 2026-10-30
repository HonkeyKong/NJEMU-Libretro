#!/usr/bin/env python3
"""Generate original 68000/Z80 test programs. Contains no arcade ROM data.

The existing 1941 machine definition supplies hardware wiring only. A private
rominfo database matches CRCs of our own code and blank graphics/audio.
"""
import pathlib
import struct
import sys
import zipfile
import zlib
root = pathlib.Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=True)
program = bytearray(0x10000)
program[:8] = struct.pack('>II', 0xfff000, 0x100)
program[0x100:0x10c] = bytes.fromhex('46fc2700 527900ff0000 60f8')
# ROM type 2 swaps big-endian 68000 words into the emulator's native layout.
parts = [('CPU1',0x200000,2,'test.prg',program),
         ('CPU2',0x10000,0,'test.z80',bytes([0x76])*0x10000),
         ('GFX1',0x200000,0,'test.gfx',bytes(0x200000)),
         ('SOUND1',0x40000,0,'test.pcm',bytes(0x40000))]
lines = ['// Generated original programs; not a commercial game.\n',
         'FILENAME( 1941, cps1, 0, 11, 0, 1 )\n']
with zipfile.ZipFile(root/'1941.zip','w',compression=zipfile.ZIP_DEFLATED) as archive:
    for region,size,kind,name,data in parts:
        archive.writestr(name,data)
        lines += [f'\tREGION( 0x{size:x}, {region}, 0 )\n',
                  f'\tROM( {kind}, {name}, 0x00000, 0x{len(data):x}, 0x{zlib.crc32(data):08x} )\n']
lines.append('END\n')
parent_db = ''.join(lines)
clone_db = parent_db.replace('FILENAME( 1941, cps1,', 'FILENAME( 1941j, 1941,')
(root/'rominfo.cps1').write_text(parent_db + clone_db)
with zipfile.ZipFile(root/'1941j.zip','w') as clone:
    clone.writestr('fixture.txt', 'Original program resides in parent 1941.zip')
print(root/'1941.zip')

# CPS2: an unencrypted test program using the 19xx board definition. The
# synthetic name has no decryption key; no commercial opcodes are included.
cps2 = root / 'cps2_direct'
cps2.mkdir(exist_ok=True)
parts2 = [('CPU1',0x400000,2,'test.prg',program),
          ('CPU2',0x50000,0,'test.z80',bytes([0x76])*0x50000),
          ('SOUND1',0x40000,0,'test.pcm',bytes(0x40000))]
lines2 = ['FILENAME( lrtest, 19xx, 0, 1, 0, 1 )\n']
graphics = []
for lane in range(4):
    data = bytearray([255]) * 0x400000
    data[:0x1000] = bytes(0x1000)
    data[0x2000] = 0xf0 + lane  # Each archive member has a distinct CRC.
    graphics.append(data)
gfx_a, gfx_b = graphics[:2]
with zipfile.ZipFile(cps2/'lrtest.zip','w',compression=zipfile.ZIP_DEFLATED) as archive:
    for region,size,kind,name,data in parts2:
        archive.writestr(name,data)
        lines2 += [f'\tREGION( 0x{size:x}, {region}, 0 )\n',
                   f'\tROM( {kind}, {name}, 0x0, 0x{len(data):x}, 0x{zlib.crc32(data):08x} )\n']
    for lane in range(2):
        archive.writestr(f'test-gfx-{chr(97+lane)}',graphics[lane])
with zipfile.ZipFile(cps2/'19xx.zip','w',compression=zipfile.ZIP_DEFLATED) as parent:
    for lane in range(2,4):
        parent.writestr(f'test-gfx-{chr(97+lane)}',graphics[lane])
lines2 += ['\tREGION( 0x1000000, GFX1, 0 )\n']
for lane,data in enumerate(graphics):
    lines2.append(f'\tROMX( 0, test-gfx-{chr(97+lane)}, {2*lane}, 0x400000, 0x{zlib.crc32(data):08x}, 2, 6 )\n')
lines2.append('END\n')
(cps2/'rominfo.cps2').write_text(''.join(lines2))

# A second set exercises the other actual CPS2 ROMX layout: eight byte lanes.
byte_lanes = root / 'cps2_byte_lanes'
byte_lanes.mkdir(exist_ok=True)
byte_db = lines2[:1]
with zipfile.ZipFile(byte_lanes/'lrtest.zip','w',compression=zipfile.ZIP_DEFLATED) as archive, \
     zipfile.ZipFile(byte_lanes/'19xx.zip','w',compression=zipfile.ZIP_DEFLATED) as parent:
    for region,size,kind,name,data in parts2:
        archive.writestr(name,data)
        byte_db += [f'\tREGION( 0x{size:x}, {region}, 0 )\n',
                    f'\tROM( {kind}, {name}, 0x0, 0x{len(data):x}, 0x{zlib.crc32(data):08x} )\n']
    byte_db.append('\tREGION( 0x1000000, GFX1, 0 )\n')
    for lane in range(8):
        data = bytearray([255]) * 0x200000
        data[:0x800] = bytes(0x800)
        data[0x2000] = 0xe0 + lane
        name = f'byte-gfx-{lane}'
        (archive if lane < 4 else parent).writestr(name, data)
        byte_db.append(f'\tROMX( 0, {name}, {lane}, 0x200000, 0x{zlib.crc32(data):08x}, 1, 7 )\n')
byte_db.append('END\n')
(byte_lanes/'rominfo.cps2').write_text(''.join(byte_db))

# Optional original stereo sine fixture. libmp3lame is an encoder used only by
# tests when locally installed; core decoding has no external MP3 dependency.
import ctypes
import ctypes.util
import math
lame_path = ctypes.util.find_library('mp3lame')
if lame_path:
    lib = ctypes.CDLL(lame_path)
    lib.lame_init.restype = ctypes.c_void_p
    ctx = lib.lame_init()
    for name, value in [('lame_set_in_samplerate',44100),('lame_set_num_channels',2),('lame_set_brate',128),('lame_set_quality',5)]:
        fn = getattr(lib,name)
        fn.argtypes = [ctypes.c_void_p,ctypes.c_int]
        fn(ctx,value)
    lib.lame_init_params.argtypes = [ctypes.c_void_p]
    lib.lame_init_params(ctx)
    pcm = (ctypes.c_short * (4410*2))()
    for i in range(4410):
        pcm[2*i] = pcm[2*i+1] = int(math.sin(2*math.pi*440*i/44100)*4000)
    out = (ctypes.c_ubyte * 20000)()
    lib.lame_encode_buffer_interleaved.argtypes = [ctypes.c_void_p,ctypes.POINTER(ctypes.c_short),ctypes.c_int,ctypes.POINTER(ctypes.c_ubyte),ctypes.c_int]
    n = lib.lame_encode_buffer_interleaved(ctx,pcm,4410,out,len(out))
    assert n >= 0
    encoded = bytes(out[:n])
    lib.lame_encode_flush.argtypes = [ctypes.c_void_p,ctypes.POINTER(ctypes.c_ubyte),ctypes.c_int]
    n = lib.lame_encode_flush(ctx,out,len(out))
    assert n >= 0
    encoded += bytes(out[:n])
    lib.lame_close.argtypes = [ctypes.c_void_p]
    lib.lame_close(ctx)
    (root/'tone.mp3').write_bytes(encoded)

# Failure cases must clean up allocations made before graphics loading.
import shutil
missing = root / 'cps2_missing_parent'
missing.mkdir(exist_ok=True)
shutil.copyfile(cps2/'rominfo.cps2',missing/'rominfo.cps2')
shutil.copyfile(cps2/'lrtest.zip',missing/'lrtest.zip')
invalid = root / 'cps2_invalid_graphics'
invalid.mkdir(exist_ok=True)
(invalid/'rominfo.cps2').write_text(''.join(lines2).replace('0x1000000, GFX1', '0x1000001, GFX1'))
shutil.copyfile(cps2/'lrtest.zip',invalid/'lrtest.zip')
shutil.copyfile(cps2/'19xx.zip',invalid/'19xx.zip')
corrupt = root / 'cps2_corrupt_graphics'
corrupt.mkdir(exist_ok=True)
shutil.copyfile(cps2/'rominfo.cps2',corrupt/'rominfo.cps2')
shutil.copyfile(cps2/'lrtest.zip',corrupt/'lrtest.zip')
shutil.copyfile(cps2/'19xx.zip',corrupt/'19xx.zip')
with zipfile.ZipFile(corrupt/'lrtest.zip') as archive:
    member = archive.getinfo('test-gfx-a')
with (corrupt/'lrtest.zip').open('r+b') as archive:
    archive.seek(member.header_offset + 26)
    name_len, extra_len = struct.unpack('<HH',archive.read(4))
    archive.seek(member.header_offset + 30 + name_len + extra_len + member.compress_size//2)
    byte = archive.read(1)
    archive.seek(-1, 1)
    archive.write(bytes([byte[0] ^ 0xff]))
short = root / 'cps2_short_graphics'
short.mkdir(exist_ok=True)
short_gfx = gfx_a[:0x10000]
(short/'rominfo.cps2').write_text(''.join(lines2).replace(
    f'0x{zlib.crc32(gfx_a):08x}', f'0x{zlib.crc32(short_gfx):08x}'))
with zipfile.ZipFile(short/'lrtest.zip','w',compression=zipfile.ZIP_DEFLATED) as archive:
    for _,_,_,name,data in parts2:
        archive.writestr(name,data)
    archive.writestr('test-gfx-a',short_gfx)
shutil.copyfile(cps2/'19xx.zip',short/'19xx.zip')
invalid_offset = root / 'cps2_invalid_offset'
invalid_offset.mkdir(exist_ok=True)
(invalid_offset/'rominfo.cps2').write_text(''.join(lines2).replace(
    'test-gfx-b, 2,', 'test-gfx-b, 0xffffff,'))
shutil.copyfile(cps2/'lrtest.zip',invalid_offset/'lrtest.zip')
shutil.copyfile(cps2/'19xx.zip',invalid_offset/'19xx.zip')

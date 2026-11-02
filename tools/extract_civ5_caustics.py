"""Copy Civilization V's caustic textures into textures/ for the release package.

  uv run tools/extract_civ5_caustics.py [Civ5Dir]

Reads fx_caustics_base_01.dds and _02.dds from Civ V's Resource/DX9/EffectTextures.fpk (default: the Steam folder)
and writes textures/water_caustics1.dds and water_caustics2.dds, the names the water looks for in a GraphicsUpgrade
folder.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CIV5 = r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V"
TEXTURES = {'fx_caustics_base_01.dds': 'water_caustics1.dds', 'fx_caustics_base_02.dds': 'water_caustics2.dds'}


def civ5_fpk_entries(data):
    """Civ 5's layout, as src/assets.cpp ReadFromCiv5Fpk: u32 6, "FPK_", u16, u32 count, then per entry u32 name
    length, name bytes, padding to 4, u64 time, u32 size, u32 offset."""
    if data[4:8] != b'FPK_':
        sys.exit('not a Civ 5 FPK')
    count, = struct.unpack_from('<I', data, 10)
    pos = 14
    for _ in range(count):
        n, = struct.unpack_from('<I', data, pos)
        name = data[pos + 4:pos + 4 + n].decode('latin-1')
        pos += 4 + n + (4 - n % 4) % 4
        size, offset = struct.unpack_from('<II', data, pos + 8)
        pos += 16
        yield name.lower(), data[offset:offset + size]


def main():
    fpk = Path(sys.argv[1] if len(sys.argv) > 1 else CIV5) / 'Resource' / 'DX9' / 'EffectTextures.fpk'
    found = {name: dds for name, dds in civ5_fpk_entries(fpk.read_bytes()) if name in TEXTURES}
    for src, dst in TEXTURES.items():
        if src not in found or found[src][:4] != b'DDS ':
            sys.exit(f'{src} not found in {fpk}')
        (ROOT / 'textures' / dst).write_bytes(found[src])
        print(f'{dst}: {len(found[src]):,d} bytes from {src}')


if __name__ == '__main__':
    main()

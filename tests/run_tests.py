# /// script
# requires-python = ">=3.12"
# ///
"""GraphicsUpgrade's tests: no game needed, only its files.

  uv run tests/run_tests.py

Builds GraphicsUpgrade (build.bat) and the test harness (tests/build_tests.bat), then runs the harness against the
DLL in many configurations (each in its own folder under build/test, with its own GraphicsUpgrade.ini) and checks
what it draws, logs, dumps and traces.

A few tests use files from the installed games, found at the Steam locations or set with the environment variables
GU_BTS (the "Sid Meier's Civilization IV Beyond the Sword" folder) and GU_COL (the Colonization folder):
  - BtS's compiled tree and river shaders, and Colonization's water shader, extracted from their Shaders/FXO effects
  - BtS's unit blob shadow texture (Assets/Art0.FPK) and a unit flag texture
Tests whose files are missing are skipped. GU_BTS_OLD_SHADERS may name the Shaders folder of an older (non-Steam)
BtS build: its tree and river shaders are then tested too.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, 'build')
DLL = os.path.join(BUILD, 'd3d9.dll')
TEST = os.path.join(BUILD, 'test')
OUT = os.path.join(TEST, 'out')
FAILS = []

STEAM = r'C:\Program Files (x86)\Steam\steamapps\common'
BTS = os.environ.get('GU_BTS', os.path.join(STEAM, "Sid Meier's Civilization IV Beyond the Sword"))
COL = os.environ.get('GU_COL', os.path.join(STEAM, 'Civilization IV Colonization'))
GAME_SHADERS = os.path.join(TEST, 'game_shaders')  # BtS's tree and river shaders
OLD_GAME_SHADERS = os.path.join(TEST, 'old_game_shaders')  # the same from GU_BTS_OLD_SHADERS
OLD_SHADER_DIR = os.environ.get('GU_BTS_OLD_SHADERS')
COL_SHADERS = os.path.join(TEST, 'col_shaders')    # Colonization's water shader


def read_fpk(fpk, wanted):
    """One file from an FPK archive: u32 4, "FPK_", u8, u32 count, then per entry u32 name length, name bytes
    stored +1, padding to 4, u64 time, u32 size, u32 offset."""
    with open(fpk, 'rb') as f:
        head = f.read(13)
        if head[4:8] != b'FPK_':
            return None
        (count,) = struct.unpack_from('<I', head, 9)
        for _ in range(count):
            (n,) = struct.unpack('<I', f.read(4))
            name = bytes((b - 1) & 0xFF for b in f.read(n)).decode('latin-1').lower()
            f.seek((4 - n % 4) % 4, 1)
            _, size, offset = struct.unpack('<QII', f.read(16))
            if name == wanted:
                f.seek(offset)
                return f.read(size)
    return None


def fpk_bytes(files):
    """An FPK archive (the layout read_fpk reads) holding files: {name under Assets: bytes}."""
    def stored(name):
        b = bytes((c + 1) & 0xFF for c in name.lower().encode('latin-1'))
        return struct.pack('<I', len(b)) + b + b'\0' * ((4 - len(b) % 4) % 4)

    offset = 13 + sum(len(stored(name)) + 16 for name in files)
    entries, data = b'', b''
    for name, content in files.items():
        entries += stored(name) + struct.pack('<QII', 0, len(content), offset + len(data))
        data += content
    return struct.pack('<I', 4) + b'FPK_' + b'\0' + struct.pack('<I', len(files)) + entries + data


def surface_dds(alpha):
    """An 8x8 DXT3 .dds of one colour (green) and one alpha (0..15): a texture for water on models."""
    head = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x1007, 8, 8, 0, 0, 1)
    head += struct.pack('<II4s20x', 32, 0x4, b'DXT3') + struct.pack('<I16x', 0x1000)
    block = bytes([alpha * 17] * 8) + struct.pack('<HHI', 0x07E0, 0x07E0, 0)  # 4-bit alphas, then colour (565)
    return head + block * 4


def light_rig_nif(sun, ambient, direction, dimmer=0.999):
    """A SunLight.nif like Warlords' (NIF 20.0.0.4): a root node with z-buffer and vertex colour properties, a node
    turning a directional light (sun: diffuse 0..1, times dimmer) to shine along direction, and an ambient light."""
    def sized(text):
        return struct.pack('<I', len(text)) + text.encode('latin-1')

    def refs(*items):
        return struct.pack('<I', len(items)) + b''.join(struct.pack('<i', r) for r in items)

    def av(name, rotation, props=()):
        return sized(name) + refs() + struct.pack('<i', -1) + struct.pack('<H', 16) + struct.pack('<3f', 0, 0, 0) + \
            struct.pack('<9f', *rotation) + struct.pack('<f', 1) + refs(*props) + struct.pack('<i', -1)

    def light(amb, dif):
        return av('', (1, 0, 0, 0, 1, 0, 0, 0, 1)) + b'\1' + refs() + struct.pack('<f', dimmer) + \
            struct.pack('<3f', *amb) + struct.pack('<3f', *dif) + struct.pack('<3f', *dif)

    # The node's rotation: its first column (the light's x axis, which it shines along) is the direction.
    x = [c / sum(v * v for v in direction) ** 0.5 for c in direction]
    up = (0, 0, 1) if abs(x[2]) < 0.9 else (1, 0, 0)
    y = [up[1] * x[2] - up[2] * x[1], up[2] * x[0] - up[0] * x[2], up[0] * x[1] - up[1] * x[0]]
    y = [v / sum(w * w for w in y) ** 0.5 for v in y]
    z = [x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]]
    turn = (x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2])  # row-major, columns x, y, z
    types = ['NiNode', 'NiZBufferProperty', 'NiVertexColorProperty', 'NiDirectionalLight', 'NiAmbientLight']
    blocks = [(0, av('Scene Root', (1, 0, 0, 0, 1, 0, 0, 0, 1), (1, 2)) + refs(3, 5) + refs(4, 5)),
              (1, sized('') + refs() + struct.pack('<i', -1) + struct.pack('<HI', 3, 3)),
              (2, sized('') + refs() + struct.pack('<i', -1) + struct.pack('<HII', 0, 0, 1)),
              (0, av('SunLightDir', turn) + refs(4) + refs()),
              (3, light((0, 0, 0), sun)),
              (4, light(ambient, (0, 0, 0)))]
    head = b'Gamebryo File Format, Version 20.0.0.4\n' + struct.pack('<IBII', 0x14000004, 1, 0, len(blocks))
    head += struct.pack('<H', len(types)) + b''.join(sized(t) for t in types)
    head += b''.join(struct.pack('<H', t) for t, _ in blocks) + struct.pack('<I', 0)
    return head + b''.join(b for _, b in blocks) + refs(0)


def bts_shaders(reader, shader_dir, out_dir):
    """BtS's tree and river shaders from a Shaders folder, saved by role (tree_vs.bin, river0_ps.bin...) for the
    harness."""
    import game_shaders
    for effect, technique, passes, roles in (('ContourShader.fx', 'Contour_Shader', [0], ['tree_vs', 'tree_ps']),
                                             ('River.fx', 'River_Shader', [0, 1],
                                              ['river0_vs', 'river0_ps', 'river1_vs', 'river1_ps'])):
        names = game_shaders.extract(reader, shader_dir, effect, technique, passes, out_dir) or []
        for name, role in zip(names, roles):
            shutil.copyfile(os.path.join(out_dir, name), os.path.join(out_dir, role + '.bin'))


def game_files():
    """Extracts what the tests use from the installed games; returns (blob .dds path or None, flag pole path or None)."""
    sys.path.insert(0, HERE)
    import game_shaders
    reader = game_shaders.EffectReader()
    for d in (GAME_SHADERS, OLD_GAME_SHADERS, COL_SHADERS):
        if os.path.isdir(d):
            shutil.rmtree(d)
    bts_shaders(reader, os.path.join(BTS, 'Beyond the Sword', 'Shaders'), GAME_SHADERS)
    if OLD_SHADER_DIR:
        bts_shaders(reader, OLD_SHADER_DIR, OLD_GAME_SHADERS)
    game_shaders.extract(reader, os.path.join(COL, 'Shaders'), 'Water.fx', 'Water_Shader', [0], COL_SHADERS)
    blob = None
    art = os.path.join(BTS, 'Assets', 'Art0.FPK')
    data = read_fpk(art, 'art\\units\\01_unitshadows\\unit_shadows.dds') if os.path.exists(art) else None
    if data:
        blob = os.path.join(TEST, 'unit_shadows.dds')
        open(blob, 'wb').write(data)
    flag = os.path.join(BTS, 'Beyond the Sword', 'Assets', 'Art', 'Units', 'Flags', 'Ancient_Flag_Pole.dds')
    return blob, flag if os.path.exists(flag) else None


# Every test's settings start from the main effects off (the release defaults turn them on), so a test only gets
# what it asks for. Merged key by key: Windows reads only the first copy of a section.
BASELINE = {'water': {'Enabled': '0'}, 'reflection': {'Enabled': '0'}, 'shadows': {'Enabled': '0'},
            'lighting': {'Enabled': '0', 'LightCycleOn': '0'}}


def with_baseline(text):
    lines = text.splitlines()
    sections = {}  # section (lower case) -> (index of its header line, keys set in it)
    current = None
    for i, line in enumerate(lines):
        m = re.match(r'\s*\[([^\]]+)\]', line)
        if m:
            current = m.group(1).strip().lower()
            sections.setdefault(current, (i, set()))
        elif current and '=' in line and not line.lstrip().startswith(';'):
            sections[current][1].add(line.split('=', 1)[0].strip().lower())
    inserts = {}
    tail = []
    for sec, keys in BASELINE.items():
        missing = [f'{k}={v}' for k, v in keys.items() if sec not in sections or k.lower() not in sections[sec][1]]
        if sec in sections:
            inserts[sections[sec][0]] = missing
        elif missing:
            tail += [f'[{sec}]'] + missing
    out = []
    for i, line in enumerate(lines):
        out.append(line)
        out += inserts.get(i, [])
    return '\n'.join(out + tail) + '\n'


class TestIni:
    """with TestIni(path) as f: f.write(...): the file gets the text with the baseline merged in."""

    def __init__(self, path):
        self.path, self.parts = path, []

    def __enter__(self):
        return self

    def write(self, text):
        self.parts.append(text)

    def __exit__(self, *exc):
        with open(self.path, 'w') as f:
            f.write(with_baseline(''.join(self.parts)))


def check(cond, what):
    print(('  ok   ' if cond else '  FAIL ') + what)
    if not cond:
        FAILS.append(what)


def fnv1a(data, h=1469598103934665603):
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def header_bytes(path):
    """Shader bytecode from an fxc /Fh header (const BYTE name[] = { 0, 2, 254, ... };, decimal)."""
    src = open(path, encoding='latin-1').read()
    body = src[src.index('{', src.index('const BYTE')) + 1:src.rindex('}')]
    return bytes(int(x, 0) for x in re.findall(r'\b(?:0x[0-9a-fA-F]+|\d+)\b', body))


def file_version(path):
    """(major, minor, build, private) from a file's VS_FIXEDFILEINFO, or None."""
    import ctypes
    ver = ctypes.WinDLL('version')
    size = ver.GetFileVersionInfoSizeW(path, None)
    if not size:
        return None
    buf = ctypes.create_string_buffer(size)
    ver.GetFileVersionInfoW(path, 0, size, buf)
    ptr, n = ctypes.c_void_p(), ctypes.c_uint()
    ver.VerQueryValueW(buf, '\\', ctypes.byref(ptr), ctypes.byref(n))  # root block = VS_FIXEDFILEINFO
    ms, ls = struct.unpack_from('<II', ctypes.string_at(ptr, n.value), 8)
    return ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF


def ghost_pixels(harness_output):
    """{'a': 0/1, ...} from the harness's 'GHOST a=1 ag=0 ...' line."""
    m = re.search(r'^GHOST (.*)$', harness_output, re.M)
    return {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        sys.exit(f'command failed: {cmd}')
    return r.stdout


def main():
    run(['cmd', '/c', os.path.join(ROOT, 'build.bat')])
    run(['cmd', '/c', os.path.join(HERE, 'build_tests.bat')])
    blob_dds, flagpole_dds = game_files()
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    shutil.copyfile(DLL, os.path.join(TEST, 'd3d9.dll'))
    with TestIni(os.path.join(TEST, 'GraphicsUpgrade.ini')) as f:
        f.write(f'[general]\nOutputDir={OUT}\nGames=*\n[debug]\nTraces=1\nDumpShaders=1\n'
                f'[trace]\nAutoFrame=2\nFrames=1\nHashTextures=1\n')
    request = os.path.join(OUT, 'test_proxy', 'trace.request')
    out = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(TEST, 'd3d9.dll'), '25', request])
    print('harness:', out.strip())

    game = os.path.join(OUT, 'test_proxy')
    log = open(os.path.join(game, 'session.log'), encoding='latin-1').read()
    print('session log:')
    check(log.count('Direct3DCreate9(32) -> wrapped') == 2, 'Direct3DCreate9 wrapped twice (probe, then device)')
    check(log.count('---- session start') == 1, 'proxy stayed loaded through FreeLibrary (one session)')
    check('device wrapped' in log, 'device wrapped')
    check('cleared D3DCREATE_PUREDEVICE' in log, 'pure device flag cleared')
    check('new vertex shader vs_2_0' in log and 'new pixel shader ps_2_0' in log, 'shader creations logged')
    check('new vertex declaration' in log and '0.0 t2 u0 i0, 0.12 t1 u5 i0' in log, 'vertex declaration logged')
    check(re.search(r'CreateTexture 128x128 levels 1 usage 0x1 fmt 21 pool 0 -> 0x00000000', log) is not None,
          'render-target texture creation logged')
    check('CreateCubeTexture 16' in log, 'cube texture creation logged')
    check('trace started at frame 2' in log and 'trace written: 3 draws' in log, 'trace of frame 3 written (3 draws)')
    check('trace requested by' in log and log.count('trace written') == 2 and not os.path.exists(request),
          'trigger file picked up, deleted, second trace written')
    check('device released' in log, 'device wrapper released')

    print('ghost:')
    off = ghost_pixels(out)
    check(off == {'a': 1, 'ag': 0, 'b': 1, 'bg': 0, 'c': 1, 'cg': 0, 'restored': 1},
          f'ghost off: originals drawn, no copies {off}')
    gdir = os.path.join(TEST, 'ghost')
    gout_dir = os.path.join(gdir, 'out')
    if os.path.isdir(gdir):
        shutil.rmtree(gdir)
    os.makedirs(gdir)
    shutil.copyfile(DLL, os.path.join(gdir, 'd3d9.dll'))
    with TestIni(os.path.join(gdir, 'GraphicsUpgrade.ini')) as f:
        f.write(f'[general]\nOutputDir={gout_dir}\nGames=*\n[debug]\nDumpShaders=1\n[ghost]\nEnabled=1\nOffset=0.9,0,0\n')
    gout = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(gdir, 'd3d9.dll'), '2'])
    on = ghost_pixels(gout)
    check(on == {'a': 1, 'ag': 1, 'b': 1, 'bg': 1, 'c': 1, 'cg': 0, 'restored': 1},
          f'ghost on: shader and FF-blended draws copied 0.9 right, plain FF not, state restored {on}')
    glog = open(os.path.join(gout_dir, 'test_proxy', 'session.log'), encoding='latin-1').read()
    check('ghost on from start (offset 0.9,0,0)' in glog, 'ghost enabled from the INI')
    check('mtxViewProj c60 x4' in glog and 'mtxWorldBones c0 x3' in glog, 'constant table parsed (CTAB names/registers)')

    print('reflection:')
    rdir = os.path.join(TEST, 'reflect')
    rout_dir = os.path.join(rdir, 'out')
    if os.path.isdir(rdir):
        shutil.rmtree(rdir)
    os.makedirs(rdir)
    shutil.copyfile(DLL, os.path.join(rdir, 'd3d9.dll'))
    with TestIni(os.path.join(rdir, 'GraphicsUpgrade.ini')) as f:
        f.write(f'[general]\nOutputDir={rout_dir}\nGames=*\n[debug]\nTraces=1\n[trace]\nAutoFrame=2\n'
                f'[reflection]\nEnabled=1\nOverlay=1\nWaterHeight=9\nSize=512\n')
    rout = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(rdir, 'd3d9.dll'), 'reflect'])
    m = re.search(r'REFLECT restored=(\d) reset=(\d)', rout)
    check(m and m.group(1) == '1', 'game state restored after each mirrored redraw (VP, VIEW, clip, render target)')
    check(m and m.group(2) == '1', 'device Reset succeeds with the reflection target allocated')
    check('final refs device 0, d3d 0' in rout, f'device and Direct3D fully released ({rout.strip().splitlines()[-1]})')
    rlog = open(os.path.join(rout_dir, 'test_proxy', 'session.log'), encoding='latin-1').read()
    check('reflection: water height 0.5 (from water mtxWorld)' in rlog, 'water height read from the water draw (INI said 9)')
    check(rlog.count('reflection: created 512x512') >= 2, 'reflection target recreated after Reset')
    rtraces = os.path.join(rout_dir, 'test_proxy', 'traces')
    bmp_path = os.path.join(rtraces, os.listdir(rtraces)[0], 'reflection.bmp')
    check(os.path.exists(bmp_path), 'reflection.bmp saved with the trace')
    if os.path.exists(bmp_path):
        bmp = open(bmp_path, 'rb').read()
        size = struct.unpack_from('<i', bmp, 18)[0]

        def alpha(x, y):
            px, py = int((x + 1) / 2 * size), int((1 - y) / 2 * size)
            return bmp[54 + (py * size + px) * 4 + 3]

        got = {k: int(alpha(*p) > 128) for k, p in
               {'A': (0.5, -0.5), 'B': (0.5, 0.5), 'C': (0.5, 0.0), 'D': (0.8, 0.0), 'E': (0.5, -0.8),
                'F': (0.2, 0.5), 'unflipped_A': (-0.5, -0.5)}.items()}
        check(got == {'A': 1, 'B': 0, 'C': 1, 'D': 0, 'E': 0, 'F': 0, 'unflipped_A': 0},
              f'reflection texture: above-water ships mirrored (x-flipped), below-water parts clipped, '
              f'post-water draws and unlit blended decals excluded {got}')

    print('sun shadows:')
    # BtS's blob shadow texture and a flag texture, named to GraphicsUpgrade by full path (the test folder has no
    # Assets folder to find them in).
    if not blob_dds:
        print(f'  skip  blob and weapon checks: unit_shadows.dds not found in {BTS}')
    textures_ini = (f'BlobShadowTexture={blob_dds}\n' if blob_dds else '') + \
                   (f'NoShadowTextures={flagpole_dds}\n' if flagpole_dds else '')
    shadow = {}

    def shadow_run(label, enabled, overlay='none', extra='', buildings=1, shadows_extra='', flagpole=False,
                   old=False, game_dir=None, flag_stage=0, mod_files=None, flag_dds=None):
        """Harness 'shadow' scene with shadows on or off; returns its SHADOW values and session log. old: the
        terrain and unit shaders named as in older BtS builds; game_dir: BtS's tree and river shaders from there;
        flag_stage: the texture stage the flag texture is on (flag_dds: another texture there); mod_files: {path in a
        mod folder: bytes}, run with mod= that folder."""
        sdir = os.path.join(TEST, f'shadow_{overlay}_{label}')
        # extra: more INI text (e.g. a [lighting] section)
        sout_dir = os.path.join(sdir, 'out')
        if os.path.isdir(sdir):
            shutil.rmtree(sdir)
        os.makedirs(sdir)
        shutil.copyfile(DLL, os.path.join(sdir, 'd3d9.dll'))
        with TestIni(os.path.join(sdir, 'GraphicsUpgrade.ini')) as f:
            f.write(f'[general]\nOutputDir={sout_dir}\nGames=*\n[reflection]\nWaterHeight=0\n'
                    f'[shadows]\nEnabled={enabled}\nSunDirection=-1,0,-1\nDarkness=0.5\nOverlay=1\nBuildingShadows={buildings}\n'
                    f'HillShadowLength=1\n{textures_ini}{shadows_extra}'
                    f'{extra}')
        mod_arg = []
        if mod_files is not None:
            mod = os.path.join(sdir, 'Mods', 'TestMod')
            for rel, content in mod_files.items():
                os.makedirs(os.path.dirname(os.path.join(mod, rel)), exist_ok=True)
                with open(os.path.join(mod, rel), 'wb') as f:
                    f.write(content)
            mod_arg = ['mod=' + mod]
        env = dict(os.environ, GU_TEST_OLD_SHADERS='1' if old else '0')
        sres = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(sdir, 'd3d9.dll'), 'shadow',
                    blob_dds if blob_dds and overlay == 'none' else '-', overlay, game_dir or river_dir] +
                   ([flag_dds or flagpole_dds, str(flag_stage)] if flagpole else []) + mod_arg, env=env)
        m = re.search(r'^SHADOW (.*)$', sres, re.M)
        values = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}
        values['released'] = 'final refs device 0, d3d 0' in sres
        return values, open(os.path.join(sout_dir, 'test_proxy', 'session.log'), encoding='latin-1').read()

    river_dir = GAME_SHADERS
    for label, enabled in (('off', 0), ('on', 1)):
        shadow[label], slog = shadow_run(label, enabled)
        check(shadow[label]['released'], f'device and Direct3D fully released (shadows {label})')
        if enabled:
            check(slog.count('shadows: created 2048x2048') >= 2, 'shadow targets created, and again after Reset')
            m = re.search(r'first sun camera centred at ([-\d.e]+),([-\d.e]+),[-\d.e]+, area ([\d.]+)x([\d.]+)', slog)
            check(m and abs(float(m.group(1))) < 1 and abs(float(m.group(2))) < 1 and
                  float(m.group(3)) == 1280 and float(m.group(4)) == 896,
                  f'sun camera fitted to the view: 1000x1000 of ground = 1000x707 seen from the 45-degree sun, '
                  f'+5% and 60 units, rounded up to 64 (1280x896) {m.groups() if m else ""}')
    off, on = shadow['off'], shadow['on']
    print(f'  shadows off {off}\n  shadows on  {on}')
    # Shadows on the water's surface: BtS-style water (magenta, Colonization's off) over the ground takes the shadow
    # at WaterShadow strength: Darkness 0.5 x 0.6 -> 255 x 0.7 at the shadowed point; WaterShadow=0 leaves it alone.
    w_on, _ = shadow_run('water', 1, 'water', '[water]\nEnabled=0\n')
    w_none, _ = shadow_run('water0', 1, 'water', '[water]\nEnabled=0\n', shadows_extra='WaterShadow=0\n')
    check(w_on.get('lit') == 255 and abs(w_on.get('shadowed2', 0) - round(255 * 0.7)) <= 2 and
          w_none.get('shadowed2') == 255 and w_on.get('restored') == 1 and w_on['released'],
          f"shadows on the water's surface at WaterShadow 0.6 ({w_on.get('shadowed2')}, expected {round(255 * 0.7)}; "
          f"unshadowed {w_on.get('lit')}); WaterShadow=0: none ({w_none.get('shadowed2')})")

    # A caster that goes (deleted in World Builder, or out of view) takes its shadow with it, even when nothing else
    # casts: a frame without casters mustn't leave the last shadows on the ground.
    gone, _ = shadow_run('vanish', 1, 'vanish')
    check(gone.get('shadowed', 0) < gone.get('lit', 0) - 20 and abs(gone.get('shadowed2', 0) - gone.get('lit2', 0)) <= 2,
          f"a caster's shadow goes when it does, with nothing else casting (frame 2: {gone.get('shadowed')} of "
          f"{gone.get('lit')}; after it goes: {gone.get('shadowed2')} of {gone.get('lit2')})")
    check(off.get('lit') == off.get('shadowed') and abs(off.get('lit', 0) - 161) <= 2,
          'shadows off: terrain uniform, BtS-style shading (~161)')
    check(abs(on.get('lit', 0) - off.get('lit', 0)) <= 2 and abs(on.get('lit2', 0) - off.get('lit', 0)) <= 2,
          'shadows on: unshadowed terrain matches BtS\'s own shader')
    check(on.get('shadowed', 999) <= on.get('lit', 0) * 0.6 and on.get('shadowed2', 999) <= on.get('lit2', 0) * 0.6,
          'shadows on: the caster\'s shadow darkens the terrain 100 units down-sun (also after Reset)')
    check(on.get('caster') == 255 and on.get('restored') == 1 and on.get('reset') == 1,
          'caster drawn normally; game state restored after each terrain and caster draw; Reset succeeds')
    if blob_dds:
        check(off.get('blob', 161) != 161 and abs(on.get('blob', 0) - 161) <= 2,
              f'BtS blob shadow recognised by its content and hidden only while shadows are on '
              f'(off {off.get("blob")}, on {on.get("blob")})')
        check(abs(off.get('part', 0) - 161) <= 2 and on.get('part', 999) <= 100,
              'rigid fixed-function part next to a blob (a weapon) casts a shadow')
        check(abs(on.get('far', 0) - 161) <= 2, 'rigid fixed-function mesh away from any blob casts nothing')

    # Ground decals over the terrain: BtS's own river shaders (from its Shaders/FXO) and a fixed-function decal.
    have_rivers = os.path.exists(os.path.join(river_dir, 'river0_vs.bin'))
    if not have_rivers:
        print(f'  skip  river checks: BtS river shaders not found under {BTS}')
    for overlay, name, expect in (('river0', 'river floodplain (River_Shader P0)', 86),
                                  ('river1', 'river (River_Shader P1)', 92),
                                  ('decal', 'fixed-function ground decal (roads: samples the fog of war)', 173),
                                  ('symbol', 'plot symbol (yield icons: like a road, but not depth-tested)', 173),
                                  ('overlay', 'fixed-function interface overlay on the ground (no fog of war)', 192)):
        if overlay.startswith('river') and not have_rivers:
            continue
        off_v, _ = shadow_run('off', 0, overlay)
        on_v, _ = shadow_run('on', 1, overlay)
        print(f'  {overlay}: off {off_v}\n  {overlay}: on  {on_v}')
        check(abs(off_v.get('lit', 0) - expect) <= 3 and off_v.get('shadowed') == off_v.get('lit'),
              f'{name}: drawn as expected without shadows ({off_v.get("lit")}, expected ~{expect})')
        check(abs(on_v.get('lit', 0) - off_v.get('lit', 0)) <= 2 and abs(on_v.get('lit2', 0) - off_v.get('lit', 0)) <= 2,
              f'{name}: unshadowed, our copy matches the original ({on_v.get("lit")} vs {off_v.get("lit")})')
        if overlay in ('overlay', 'symbol'):
            check(on_v.get('shadowed') == on_v.get('lit') == off_v.get('lit'),
                  f'{name}: left alone in the shadow ({on_v.get("shadowed")} = {on_v.get("lit")})')
        else:
            check(on_v.get('shadowed', 999) <= on_v.get('lit', 0) * 0.6 and
                  on_v.get('shadowed2', 999) <= on_v.get('lit2', 0) * 0.6,
                  f'{name}: darkened in the shadow ({on_v.get("shadowed")}), also after Reset')
        check(on_v.get('restored') == 1 and on_v['released'], f'{name}: game state restored; device released')

    # BtS draws roads unlit (their texture's colour, whatever the light): they take the terrain's change of light,
    # the light on flat ground over BtS's baked terrain light. Here the shadows' sun (-1,0,-1: 45 degrees up) and a
    # night's terrain colours, blue (50 + 90 x 0.707) / (133 + 222 x 0.700) = 0.394 of the decal's 173.
    night = '[lighting]\nEnabled=1\nTerrainSun=60,70,90\nTerrainAmbient=30,35,50\n'
    scale = (50 + 90 * 0.70711) / (133 + 222 * 0.700134)
    for overlay, label, enabled, ini, expect, name in (
            ('decal', 'night', 0, night, 173 * scale, 'unlit road at night: darkened as the terrain'),
            ('decal', 'nightsh', 1, night, 173 * scale, 'unlit road at night, shadows on: darkened as the terrain'),
            ('decal', 'baked', 0, '[lighting]\nEnabled=1\nMatchShadowDirection=0\n', 173,
             'unlit road with the terrain lit as BtS baked it: unchanged'),
            ('symbol', 'night', 0, night, 173, 'plot symbol (yield icon) at night: unchanged'),
            ('symbol', 'nightsh', 1, night, 173, 'plot symbol (yield icon) at night, shadows on: unchanged'),
            ('overlay', 'night', 0, night, 192, 'interface overlay on the ground at night: unchanged')):
        v, _ = shadow_run(label, enabled, overlay, ini)
        check(abs(v.get('lit', 0) - expect) <= 4 and v.get('restored') == 1 and v['released'],
              f"{name} ({v.get('lit')}, expected {expect:.0f}); game state restored")
        if enabled and overlay == 'symbol':
            check(v.get('shadowed') == v.get('lit'), f"{name}: not darkened in the shadow ({v.get('shadowed')})")
        elif enabled:
            check(v.get('shadowed', 999) <= v.get('lit', 0) * 0.6,
                  f"{name}: still darkened in the shadow ({v.get('shadowed')})")

    # The baked terrain light comes from the SunLight.nif the game uses (the running mod's first) for the BakedTerrain*
    # settings left out. Here a mod's light like Realism Invictus's (sun 255,235,188) from a direction 0.8 up: the
    # night road above is then (50 + 90 x 0.707) / (133 + 0.999 x 188 x 0.8) of 173.
    print('baked terrain light:')
    sunlight = 'Assets/Art/Terrain/Lights/SunLight.nif'
    rig = light_rig_nif((1.0, 235 / 255, 100 / 255), (89 / 255, 103 / 255, 133 / 255), (-0.6, 0, -0.8))
    rig_log = 'sun 255,235,100, ambient 89,103,133, direction -0.600,0.000,-0.800'
    rig_expect = 173 * (50 + 90 * 0.70711) / (133 + 0.999 * 100 * 0.8)
    _, rlog = shadow_run('night', 0, 'decal', night)
    check("SunLight.nif not found; baked terrain light as BtS's" in rlog and
          'baked terrain light: sun 246,239,222, ambient 89,103,133, direction -0.567,0.434,-0.700' in rlog,
          "no SunLight.nif anywhere: BtS's baked light, logged")
    for label, files, where in (
            ('rigloose', {sunlight: rig}, 'SunLight.nif)'),
            ('rigfpk', {'Assets/Mod0.FPK': fpk_bytes({'art\\other.dds': b'x', 'art\\terrain\\lights\\sunlight.nif': rig})},
             'Mod0.FPK)')):
        v, rlog = shadow_run(label, 0, 'decal', night, mod_files=files)
        check(f'{where}: {rig_log}' in rlog and abs(v.get('lit', 0) - rig_expect) <= 4,
              f"a mod's own SunLight.nif ({'in an FPK' if label == 'rigfpk' else 'loose'}) sets the baked light "
              f"({rig_log}); unlit road at night re-lit from it ({v.get('lit')}, expected {rig_expect:.0f})")
    v, rlog = shadow_run('rigset', 0, 'decal', night + 'BakedTerrainSun=255,255,30\n', mod_files={sunlight: rig})
    expect = 173 * (50 + 90 * 0.70711) / (133 + 30 * 0.8)
    check('baked terrain light: sun 255,255,30 (settings), ambient 89,103,133, direction -0.600,0.000,-0.800' in rlog and
          abs(v.get('lit', 0) - expect) <= 4,
          f"BakedTerrainSun in the settings wins over the mod's SunLight.nif, the rest comes from it ({v.get('lit')}, "
          f"expected {expect:.0f})")
    v, rlog = shadow_run('rigbad', 0, 'decal', night, mod_files={sunlight: rig[:-3]})
    check("isn't a light rig GraphicsUpgrade can read; baked terrain light as BtS's" in rlog and
          abs(v.get('lit', 0) - 173 * scale) <= 4, f"a truncated SunLight.nif: BtS's baked light ({v.get('lit')})")
    # The games' own: Warlords' (NIF 20.0.0.4, the one BtS uses) and Civilization IV's (NIF 10.1, in Art0.FPK).
    warlords = os.path.join(BTS, 'Warlords', 'Assets', 'Art', 'Terrain', 'Lights', 'SunLight.nif')
    art0 = os.path.join(BTS, 'Assets', 'Art0.FPK')
    for label, data, expect_log in (
            ('rigwarlords', open(warlords, 'rb').read() if os.path.exists(warlords) else None,
             'sun 246,239,222, ambient 89,103,133, direction -0.567,0.434,-0.700'),
            ('rigciv4', read_fpk(art0, 'art\\terrain\\lights\\sunlight.nif') if os.path.exists(art0) else None,
             'sun 255,255,255, ambient 63,60,77, direction -0.643,0.246,-0.725')):
        if not data:
            print(f"  skip  {label}: the game's SunLight.nif not found under {BTS}")
            continue
        _, rlog = shadow_run(label, 0, 'decal', night, mod_files={sunlight: data})
        check(expect_log in rlog, f"the game's own SunLight.nif read ({label[3:]}): {expect_log}")

    print('lighting:')

    def light_run(label, ini, ff=False, mod_ini=None, rt=False):
        """mod_ini: a mod's GraphicsUpgrade.ini (a GraphicsUpgrade folder in Mods\\TestMod, run with mod=...); rt: drawn
        into a render-target texture, like the interface's 3D views ('screen': one the size of the screen, like the map
        behind the Civilopedia)."""
        ldir = os.path.join(TEST, 'light_' + label)
        if os.path.isdir(ldir):
            shutil.rmtree(ldir)
        os.makedirs(ldir)
        shutil.copyfile(DLL, os.path.join(ldir, 'd3d9.dll'))
        with TestIni(os.path.join(ldir, 'GraphicsUpgrade.ini')) as f:
            f.write(f'[general]\nOutputDir={os.path.join(ldir, "out")}\nGames=*\n{ini}')
        extra = []
        if mod_ini is not None:
            mod_gu = os.path.join(ldir, 'Mods', 'TestMod', 'GraphicsUpgrade')
            os.makedirs(mod_gu)
            if mod_ini:
                with open(os.path.join(mod_gu, 'GraphicsUpgrade.ini'), 'w') as f:
                    f.write(mod_ini)
            extra = ['mod=' + os.path.join(ldir, 'Mods', 'TestMod')]
        res = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(ldir, 'd3d9.dll'), 'light'] + (['ff'] if ff else []) +
                  ([rt if isinstance(rt, str) else 'rt'] if rt else []) + extra)
        m = re.search(r'LIGHT pixel=([0-9a-f]{8}) restored=(\d)', res)
        rgb = tuple(int(m.group(1)[i:i + 2], 16) for i in (2, 4, 6)) if m else None
        return rgb, bool(m) and m.group(2) == '1' and 'final refs device 0, d3d 0' in res

    def near(a, b, tol=2):
        return a is not None and all(abs(x - y) <= tol for x, y in zip(a, b))

    lit = '[lighting]\nEnabled=1\nUnitSun=200,100,50\nUnitAmbient=20,40,60\n'
    for label, ini, expect, what in (
            ('off', '', (153, 178, 204), "lighting off: the game's own light (diffuse 0.5, ambient 0.1,0.2,0.3)"),
            ('colours', lit + 'MatchShadowDirection=0\n', (220, 140, 110),
             'INI colours (0-255) replace the unit light: sun 200,100,50 + ambient 20,40,60'),
            ('shared', '[lighting]\nEnabled=1\nSunColour=100,100,100\nAmbientColour=10,10,10\nMatchShadowDirection=0\n',
             (110, 110, 110), 'SunColour/AmbientColour apply to units when no Unit* key is set'),
            ('shadowdir', lit + '[shadows]\nSunDirection=0,0,-1\n', (220, 140, 110),
             "MatchShadowDirection: lit from the shadows' sun direction (straight down: full sun)"),
            ('lightdir', lit + 'LightDirection=1,0,0\n', (20, 40, 60),
             'LightDirection: lit from the INI direction (sideways: ambient only)')):
        rgb, ok = light_run(label, ini)
        check(near(rgb, expect) and ok, f'{what} -> {rgb}, expected {expect}; game constants restored')

    # Sun colour cycle: keyframes at steps 6 (red) and 18 (blue) of 24; a long cycle, so the run is at step ~0, halfway
    # from 18 round to 6: half red, half blue. A group with its own *Sun key keeps it; a bad entry is skipped.
    cycle = ('[lighting]\nEnabled=1\nAmbientColour=0,0,0\nMatchShadowDirection=0\nLightCycleOn=1\n'
             'LightChangeCycleTime=100000\nLightChangeStepCount=24\nLightCycleStartStep=0\nLightCycleSteps=6,255,0,0; 18,0,0,255;junk\n')
    rgb, ok = light_run('cycle', cycle)
    clog = open(os.path.join(TEST, 'light_cycle', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(near(rgb, (128, 0, 128)) and ok, f'sun colour cycle: step 0 is halfway from step 18 (blue) round to step 6 '
          f'(red) -> {rgb}, expected (128, 0, 128)')
    check('light cycle, 2 sun, 0 ambient, 0 direction and 0 sun angle keyframes over 24 steps, 100000 s per cycle' in clog and
          'LightCycleSteps entry "junk"' in clog, 'sun colour cycle logged; the malformed entry warned about and skipped')
    # By default the cycle starts at midday (step 12): there, the step-12 keyframe's colour exactly.
    rgb, ok = light_run('cycle_noon', '[lighting]\nEnabled=1\nAmbientColour=0,0,0\nMatchShadowDirection=0\nLightCycleOn=1\n'
                        'LightChangeCycleTime=100000\nLightChangeStepCount=24\nLightCycleSteps=12,255,0,0;0,0,0,255\n')
    nlog = open(os.path.join(TEST, 'light_cycle_noon', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(near(rgb, (255, 0, 0)) and ok and 'starting at step 12' in nlog,
          f'the light cycle starts at midday by default (LightCycleStartStep=12): the step-12 colour -> {rgb}')
    # The cycle's clock only runs while the world (terrain) is on screen: the light scene draws none, so even a 0.01 s
    # day stays at its start (menus and loading screens don't use up the day).
    rgb, ok = light_run('cycle_menu', '[lighting]\nEnabled=1\nAmbientColour=0,0,0\nMatchShadowDirection=0\nLightCycleOn=1\n'
                        'LightChangeCycleTime=0.01\nLightChangeStepCount=24\nLightCycleSteps=12,255,0,0;0,0,0,255\n')
    check(near(rgb, (255, 0, 0)) and ok, f'the light cycle stands still while no terrain is drawn (a 0.01 s day still '
          f'at midday) -> {rgb}')
    # Sun angles go round, not across: halfway from azimuth 90 (step 18) to 0 (step 6) at 30 degrees up is azimuth 45,
    # still 30 degrees up, so the upward-facing triangle gets sin 30 = 0.5 of the sun. (Blending the two directions as
    # vectors would lift it to 39 degrees: 0.63.) An entry below the horizon is skipped.
    rgb, ok = light_run('cycle_angles', lit + 'LightCycleOn=1\nLightChangeCycleTime=100000\nLightChangeStepCount=24\n'
                        'LightCycleStartStep=0\nLightCycleSunAngles=6,0,30;18,90,30;9,0,-5\n')
    alog = open(os.path.join(TEST, 'light_cycle_angles', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(near(rgb, (120, 90, 85)) and ok and 'LightCycleSunAngles entry "9,0,-5"' in alog,
          f'sun angles blend round the sky (30 degrees up halfway between azimuths 90 and 0) -> {rgb}, expected '
          '(120, 90, 85) = sun 200,100,50 x 0.5 + ambient 20,40,60; an entry below the horizon skipped')
    # The simple sun path: at midday the sun is SunMiddayHeight up (30: half the sun on the upward triangle); at
    # midnight the moon is MoonHeight up (90: all of it); a compass point it doesn't know is warned about.
    path = (lit + 'LightCycleOn=1\nLightChangeCycleTime=100000\nLightChangeStepCount=24\nSunMiddayFrom=SSE\n'
            'SunMiddayHeight=30\nSunriseHeight=10\nMoonHeight=90\n')
    rgb_noon, ok1 = light_run('path_noon', path)
    plog = open(os.path.join(TEST, 'light_path_noon', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    rgb_night, ok2 = light_run('path_night', path + 'LightCycleStartStep=0\n')
    rgb_bad, _ = light_run('path_bad', path.replace('SunMiddayFrom=SSE', 'SunMiddayFrom=up'))
    blog = open(os.path.join(TEST, 'light_path_bad', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(near(rgb_noon, (120, 90, 85)) and near(rgb_night, (220, 140, 110)) and ok1 and ok2 and
          'sun path from 157.5 degrees (compass) at midday' in plog and "SunMiddayFrom=up isn't a compass point" in blog,
          f'simple sun path: 30 degrees up at midday {rgb_noon} (expected (120, 90, 85)), the moon overhead at midnight '
          f'{rgb_night} (expected (220, 140, 110)); SSE read as 157.5; an unknown compass point warned about')
    rgb, ok = light_run('cycle_own', cycle + 'UnitSun=10,20,30\n')
    check(near(rgb, (10, 20, 30)) and ok, f'a group with its own *Sun key keeps it through the cycle -> {rgb}')
    # The ambient cycles the same way (no sun: the pixel is the ambient), unless the group has its own *Ambient key.
    amb_cycle = ('[lighting]\nEnabled=1\nSunColour=0,0,0\nMatchShadowDirection=0\nLightCycleOn=1\n'
                 'LightChangeCycleTime=100000\nLightChangeStepCount=24\nLightCycleStartStep=0\nLightCycleAmbientSteps=6,0,100,0;18,0,0,100\n')
    rgb, ok = light_run('cycle_amb', amb_cycle)
    check(near(rgb, (0, 50, 50)) and ok, f'ambient colour cycle: halfway from step 18 round to step 6 -> {rgb}, '
          'expected (0, 50, 50)')
    rgb, ok = light_run('cycle_amb_own', amb_cycle + 'UnitAmbient=10,20,30\n')
    check(near(rgb, (10, 20, 30)) and ok, f'a group with its own *Ambient key keeps it through the cycle -> {rgb}')
    # The sun's direction cycles too: halfway from (1,0,-1) at step 18 round to straight down at step 6 is 22.5
    # degrees off vertical, so the upward-facing triangle gets cos 22.5 = 0.924 of the sun.
    rgb, ok = light_run('cycle_dir', lit + 'LightCycleOn=1\nLightChangeCycleTime=100000\nLightChangeStepCount=24\nLightCycleStartStep=0\n'
                        'LightCycleDirections=6,0,0,-1;18,1,0,-1;9,0,0,1\n')
    dlog = open(os.path.join(TEST, 'light_cycle_dir', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(near(rgb, (205, 132, 106)) and ok, f'sun direction cycle: lit 22.5 degrees off vertical -> {rgb}, '
          'expected (205, 132, 106) = sun 200,100,50 x 0.924 + ambient 20,40,60')
    check('LightCycleDirections entry "9,0,0,1" isn\'t step,x,y,z (z below 0)' in dlog,
          'a direction shining upwards is warned about and skipped')

    # A mod's own settings: run with mod=<its folder>, its GraphicsUpgrade\\GraphicsUpgrade.ini overrides the game
    # folder's setting by setting (here the unit sun only; the ambient still comes from the game folder's).
    base = lit + 'MatchShadowDirection=0\n'
    rgb_mod, ok_mod = light_run('mod', base, mod_ini='[lighting]\nUnitSun=100,100,100\n')
    mlog = open(os.path.join(TEST, 'light_mod', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    rgb_nomod, _ = light_run('mod_none', base, mod_ini='')
    nlog = open(os.path.join(TEST, 'light_mod_none', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(near(rgb_mod, (120, 140, 160)) and ok_mod and 'overrides the game folder' in mlog and
          near(rgb_nomod, (220, 140, 110)) and 'no GraphicsUpgrade.ini' in nlog,
          f"a mod's GraphicsUpgrade.ini overrides setting by setting: its sun 100,100,100 with the game folder's ambient "
          f"-> {rgb_mod} (expected (120, 140, 160)); a mod with only a GraphicsUpgrade folder keeps the game folder's "
          f"-> {rgb_nomod}")

    # Hotkeys written as keys: letters (any case), F-keys, numpad, names, hex codes, empty = none, junk = none + warning.
    keys_ini = ('[water]\nToggleKey=Numpad5\n[shadows]\nToggleKey=F11\n[lighting]\nToggleKey=\nLightCycleToggleKey=f12\n'
                '[reflection]\nToggleKey=0x4C\nOverlayToggleKey=PrintScreen\n[ghost]\nToggleKey=bogus\n'
                '[debug]\nTraces=1\n[trace]\nKey=t\n')
    light_run('keys', keys_ini)
    klog = open(os.path.join(TEST, 'light_keys', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    expect_keys = ('water Numpad5, shadows F11, lighting none, light cycle F12, reflection L, reflection overlay '
                   'PrintScreen, shadow overlay none, ghost none, trace T')
    check(expect_keys in klog and "[ghost] ToggleKey=bogus isn't a key" in klog,
          f'hotkeys written as keys (t, F11, f12, Numpad5, PrintScreen, 0x4C, empty, a bogus name) read as {expect_keys}')

    # Buildings: fixed-function lights (a directional light and D3DRS_AMBIENT) take the Building colours.
    for label, ini, expect, what in (
            ('ff_off', '', (144, 160, 176), "fixed-function (building) lit by the game's light and ambient"),
            ('ff_on', '[lighting]\nEnabled=1\nBuildingSun=200,100,50\nBuildingAmbient=20,40,60\nMatchShadowDirection=0\n',
             (236, 172, 158), 'fixed-function lights take the INI Building colours, the way BtS uses them: sun as the '
             "light's diffuse (200,100,50), ambient as the light's own ambient (20,40,60) on top of the global "
             'ambient (16,32,48)'),
            ('ff_unit', '[lighting]\nEnabled=1\nUnitSun=200,100,50\nUnitAmbient=20,40,60\n', (144, 160, 176),
             'a building is left alone when only the Unit colours are set (unset groups keep the game\'s light)')):
        rgb, ok = light_run(label, ini, ff=True)
        check(near(rgb, expect) and ok, f'{what} -> {rgb}, expected {expect}; light and ambient restored')

    # The interface's 3D views (the unit in the lower-left corner, the Civilopedia's models) are drawn into textures of
    # their own: they keep the game's light, whatever the INI and the time of day on the map.
    building = '[lighting]\nEnabled=1\nBuildingSun=200,100,50\nBuildingAmbient=20,40,60\nMatchShadowDirection=0\n'
    for label, ini, ff, expect, what in (('rt_unit', lit + 'MatchShadowDirection=0\n', False, (153, 178, 204), 'a unit'),
                                         ('rt_building', building, True, (144, 160, 176), 'a building')):
        rgb, ok = light_run(label, ini, ff=ff, rt=True)
        check(near(rgb, expect) and ok, f"{what} drawn into a texture (an interface 3D view) keeps the game's light "
              f"-> {rgb}, expected {expect}")
    # ...but a texture the size of the screen is the map: BtS draws the map into one as the Civilopedia opens, to show
    # behind it, and it gets everything the map does.
    for label, ini, ff, expect, what in (('screen_unit', lit + 'MatchShadowDirection=0\n', False, (220, 140, 110), 'a unit'),
                                         ('screen_building', building, True, (236, 172, 158), 'a building')):
        rgb, ok = light_run(label, ini, ff=ff, rt='screen')
        check(near(rgb, expect) and ok, f"{what} drawn into a screen-sized texture (the map behind the Civilopedia) "
              f"takes the INI light -> {rgb}, expected {expect}")

    # The preset files as shipped: the DLL reads the colours they give (logged at device creation).
    for preset, unit in (('lighting-bts.ini', 'unit sun 204,204,176, ambient 88,88,72'),
                         ('lighting-colonization.ini', 'unit sun 255,235,188, ambient 89,103,133')):
        text = open(os.path.join(ROOT, 'presets', preset), encoding='utf-8').read()
        light_run('preset', text)
        plog = open(os.path.join(TEST, 'light_preset', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
        check(f'lighting on from start ({unit})' in plog, f'preset {preset} read: {unit}')

    # Terrain lightmap re-lit (shadows off, lighting on). BtS bakes texel = (ambient + sun x amount) / 2 with its
    # SunLight (sun 246,239,222, ambient 89,103,133); the harness lightmap is 204,204,204, so the amount is the mean
    # of (2 x 204 - ambient) / sun, and the texel becomes (ambient' + sun' x amount) / 2.
    sun, amb = (200 / 255, 100 / 255, 50 / 255), (20 / 255, 40 / 255, 60 / 255)
    amount = sum((2 * 204 - a) / s_ for a, s_ in zip((89, 103, 133), (246, 239, 222))) / 3
    lm_b = min(1.0, max(0.0, 0.5 * (amb[2] + sun[2] * amount)))
    expect_b = round(255 * min(1.0, max(0.0, (0xE6 / 255) * (0x66 / 255 + 0x80 / 255 + lm_b - 1))))
    terrain_light = '[lighting]\nEnabled=1\nTerrainSun=200,100,50\nTerrainAmbient=20,40,60\n'
    river_light = '[lighting]\nEnabled=1\nRiverSun=200,100,50\nRiverAmbient=20,40,60\n'
    tl, _ = shadow_run('lit', 0, 'none', terrain_light)
    check(abs(tl.get('lit', 0) - expect_b) <= 3 and tl.get('restored') == 1 and tl['released'],
          f'terrain lightmap re-lit with the INI colours (blue {tl.get("lit")}, expected {expect_b}, '
          f'BtS {shadow["off"].get("lit")})')
    # Terrain colours equal to the baked ones (the BtS preset): the lightmap exactly as BtS baked it.
    tb, _ = shadow_run('baked', 0, 'none', '[lighting]\nEnabled=1\nTerrainSun=246,239,222\nTerrainAmbient=89,103,133\n')
    check(tb.get('lit') == shadow['off'].get('lit') and tb.get('restored') == 1 and tb['released'],
          f'terrain colours equal to the baked ones leave the lightmap exactly as BtS baked it '
          f'({tb.get("lit")} = {shadow["off"].get("lit")})')
    # Trees: BtS's own (unlit) tree shaders; with Tree colours our lit copy of the vertex shader takes over, lit from
    # the shadows' sun (-1,0,-1 here: 0.707 of full sun on the upward-facing patch).
    have_trees = os.path.exists(os.path.join(river_dir, 'tree_vs.bin'))
    if have_trees:
        t_off, _ = shadow_run('lit', 0, 'tree')
        t_on, _ = shadow_run('lit', 0, 'tree', '[lighting]\nEnabled=1\nTreeSun=200,100,50\nTreeAmbient=20,40,60\n')
        unlit = round(255 * (0x80 / 255) * (0xE6 / 255))
        lit_b = round(255 * (0x80 / 255) * (0xE6 / 255) * (50 / 255 * 0.70711 + 60 / 255))
        check(abs(t_off.get('lit', 0) - unlit) <= 2, f'trees unlit as in BtS ({t_off.get("lit")}, expected {unlit})')
        check(abs(t_on.get('lit', 0) - lit_b) <= 3 and t_on.get('restored') == 1 and t_on['released'],
              f'trees lit with the INI Tree colours by our copy of their vertex shader ({t_on.get("lit")}, '
              f'expected {lit_b}); the game\'s shader restored')
        t_bts, _ = shadow_run('lit', 0, 'tree', '[lighting]\nEnabled=1\nTreeSun=0,0,0\nTreeAmbient=255,255,255\n')
        check(t_bts.get('lit') == t_off.get('lit'), f'BtS preset trees (no sun, full ambient) look exactly as BtS '
              f'({t_bts.get("lit")} = {t_off.get("lit")})')
        # BtS's painted tree shadows (tree cards using the atlas's shadow images) go while sun shadows are on, even
        # with the lighting off; real trees stay exactly as BtS draws them.
        ps_off, _ = shadow_run('painted', 0, 'treeshadow')
        ps_on, _ = shadow_run('painted', 1, 'treeshadow')
        tr_on, _ = shadow_run('trees', 1, 'tree')
        check(abs(ps_off.get('lit', 0) - unlit) <= 2 and abs(ps_on.get('lit', 0) - shadow['on'].get('lit', 0)) <= 2 and
              abs(tr_on.get('lit', 0) - unlit) <= 2 and ps_on.get('restored') == 1 and ps_on['released'],
              f"painted tree shadows hidden while sun shadows are on ({ps_on.get('lit')} = bare ground "
              f"{shadow['on'].get('lit')}; shadows off: drawn, {ps_off.get('lit')}); trees themselves unchanged "
              f"({tr_on.get('lit')} = {unlit})")
    else:
        print(f'  skip  tree checks: BtS tree shaders not found under {BTS}')

    # Terrain normals for per-pixel lighting: heights read from the terrain's vertex buffer (a 5x5 grid, 250 apart,
    # z = 0.2 x), normals from neighbouring heights: (-0.196, 0, 0.981) -> colour (102, 128, 252) at cell (0,0).
    _, nlog = shadow_run('normals', 0, 'slope', '[lighting]\nEnabled=1\nTerrainSun=255,255,255\n[debug]\nTraces=1\n'
                                                 '[trace]\nAutoFrame=1\n')
    ndir = os.path.join(TEST, 'shadow_slope_normals', 'out', 'test_proxy', 'traces')
    nbmp_path = os.path.join(ndir, os.listdir(ndir)[0], 'terrain_normals.bmp') if os.path.isdir(ndir) else ''
    if os.path.exists(nbmp_path):
        nb = open(nbmp_path, 'rb').read()
        px = nb[54:58]  # cell (0,0) = world (0,0): the first texel of the top-down BMP
        got = (px[2], px[1], px[0], px[3])
    else:
        got = None
    check(got is not None and abs(got[0] - 102) <= 2 and abs(got[1] - 128) <= 1 and abs(got[2] - 252) <= 2 and got[3] == 255
          and 'grid spacing 250.000' in nlog,
          f'terrain normals from the vertex buffer: spacing 250 found, normal of the z = 0.2x slope {got} '
          f'(expected 102,128,252)')

    # Per-pixel terrain lighting: BtS's colours, the sun moved overhead (LightDirection 0,0,-1). The slope's sun
    # amount changes by N.L(new) - N.L(BtS's sun), N from its normal map texel (102,128,253); BtS's sun direction
    # leaves it exactly as baked.
    import math
    baked = '[lighting]\nEnabled=1\nTerrainSun=246,239,222\nTerrainAmbient=89,103,133\n'
    pp, _ = shadow_run('pp_turned', 0, 'slope', baked + 'LightDirection=0,0,-1\n')
    pb, _ = shadow_run('pp_baked', 0, 'slope', baked + 'MatchShadowDirection=0\n')
    n = [102 / 255 * 2 - 1, 128 / 255 * 2 - 1, 253 / 255 * 2 - 1]
    d = [-0.567005, 0.433955, -0.700134]; dl = math.sqrt(sum(x * x for x in d)); lb = [-x / dl for x in d]
    delta = n[2] - sum(a * b for a, b in zip(n, lb))
    amb_b, sun_b = 133 / 255, 222 / 255
    amount = sum((2 * 204 / 255 - a / 255) / (s_ / 255) for a, s_ in zip((89, 103, 133), (246, 239, 222))) / 3
    lm = min(1.0, max(0.0, 0.5 * (amb_b + sun_b * (amount + delta))))
    expect_pp = round(255 * min(1.0, max(0.0, (0xE6 / 255) * (0x66 / 255 + 0x80 / 255 + lm - 1))))
    check(abs(pp.get('lit', 0) - expect_pp) <= 3 and pp.get('restored') == 1 and pp['released'],
          f'per-pixel terrain lighting: sun moved overhead, the slope re-lit from its normal '
          f'(blue {pp.get("lit")}, expected {expect_pp}, as baked {pb.get("lit")})')
    check(pb.get('lit') == shadow['off'].get('lit'),
          f'sun where BtS baked it: terrain exactly as baked ({pb.get("lit")} = {shadow["off"].get("lit")})')

    # Hill shadows: a 300-high ridge along x = 0, sun going towards -x at 45 degrees: the ground behind it (-250,0) is
    # shaded (Darkness 0.5), the sunny side (200,0) isn't; HillShadows=0 turns it off.
    h_on, hlog = shadow_run('hills', 1, 'ridge')
    sdir = os.path.join(TEST, 'shadow_ridge_nohills')
    if os.path.isdir(sdir):
        shutil.rmtree(sdir)
    os.makedirs(sdir)
    shutil.copyfile(DLL, os.path.join(sdir, 'd3d9.dll'))
    with TestIni(os.path.join(sdir, 'GraphicsUpgrade.ini')) as f:
        f.write(f'[general]\nOutputDir={os.path.join(sdir, "out")}\nGames=*\n[reflection]\nWaterHeight=0\n'
                f'[shadows]\nEnabled=1\nSunDirection=-1,0,-1\nDarkness=0.5\nHillShadows=0\nHillShadowLength=1\n')
    res = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(sdir, 'd3d9.dll'), 'shadow', '-', 'ridge', river_dir])
    m = re.search(r'hill=(\d+)', res)
    no_hill = int(m.group(1)) if m else None
    check(abs(h_on.get('hill', 999) - h_on.get('lit', 0) // 2) <= 3 and abs(h_on.get('lit', 0) - 161) <= 2 and
          h_on['released'],
          f"hills cast shadows: the ground behind a ridge is shaded ({h_on.get('hill')}), the sunny side isn't "
          f"({h_on.get('lit')})")
    check(no_hill is not None and abs(no_hill - 161) <= 2, f'HillShadows=0: no hill shadow ({no_hill})')

    # The hill shadow mask is made again only when the sun has moved enough to see (0.1 degrees), the darkness has
    # changed, or the view has. With the light cycle crawling (100000 s a cycle) that's hardly ever; racing (2 s a
    # cycle), every frame. The timing line logged as the device goes says how often, and what it cost.
    def hill_builds(log):
        m = re.search(r'hill shadow mask made (\d+) times', log)
        return int(m.group(1)) if m else None

    cycle = ('[lighting]\nEnabled=1\nLightCycleOn=1\nLightChangeStepCount=24\nLightCycleStartStep=9\n'
             'SunMiddayFrom=SSE\nLightChangeCycleTime={}\n')
    _, slow_log = shadow_run('hillslow', 1, 'ridge', cycle.format(100000))
    _, fast_log = shadow_run('hillfast', 1, 'ridge', cycle.format(2))
    slow, fast = hill_builds(slow_log), hill_builds(fast_log)
    check(slow is not None and fast is not None and slow <= 2 and fast >= slow + 3,
          f'hill shadow mask made again only when the sun has moved enough to see: {slow} times with the cycle '
          f'crawling, {fast} with it racing')
    timing = re.search(r'timing over \d+ frames: .*', hlog)
    check(timing is not None and 'GraphicsUpgrade CPU' in timing.group(0) and
          ('GPU hill shadow mask' in timing.group(0) or 'no timestamp queries' in hlog),
          f"timing logged as the device goes: {timing.group(0) if timing else None}")

    # Building shadows: a lit, depth-writing fixed-function mesh casts with BuildingShadows=1, not with 0.
    b_on, _ = shadow_run('on', 1, 'building', '', buildings=1)
    b_off, _ = shadow_run('on', 1, 'building', '', buildings=0)
    check(abs(b_on.get('bld', 0) - round(b_on.get('lit', 0) * 0.32)) <= 3 and b_on['released'],
          f'building (lit fixed-function mesh) casts a shadow at the default BuildingDarkness 0.68 '
          f'({b_on.get("bld")}, expected {round(b_on.get("lit", 0) * 0.32)})')
    b_light, _ = shadow_run('bdark', 1, 'building', '', buildings=1, shadows_extra='BuildingDarkness=0.25\n')
    check(abs(b_light.get('bld', 0) - round(b_light.get('lit', 0) * 0.75)) <= 3 and
          abs(b_light.get('shadowed', 0) - b_light.get('lit', 0) // 2) <= 2,
          f"BuildingDarkness=0.25: building shadow {b_light.get('bld')} (expected {round(b_light.get('lit', 0) * 0.75)}); "
          f"units keep their own Darkness 0.5 ({b_light.get('shadowed')})")
    if not flagpole_dds:
        print(f'  skip  unit flag check: Ancient_Flag_Pole.dds not found under {BTS}')
    else:
        b_flag, _ = shadow_run('flag', 1, 'building', '', buildings=1, flagpole=True)
        check(abs(b_flag.get('bld', 0) - b_flag.get('lit', 0)) <= 2 and b_flag['released'],
              f"a unit flag's pole (BtS's Ancient_Flag_Pole.dds on the same mesh) casts no shadow ({b_flag.get('bld')} = "
              f"lit {b_flag.get('lit')}; as a building it's {b_on.get('bld')})")
        b_cloth, _ = shadow_run('flagcloth', 1, 'building', '', buildings=1, flagpole=True, flag_stage=3)
        check(abs(b_cloth.get('bld', 0) - b_cloth.get('lit', 0)) <= 2 and b_cloth['released'],
              f"a unit flag's cloth (its texture on stage 3, as BtS draws it) casts no shadow ({b_cloth.get('bld')} = "
              f"lit {b_cloth.get('lit')})")
    check(abs(b_off.get('bld', 0) - b_off.get('lit', 0)) <= 2,
          f'BuildingShadows=0: no building shadow ({b_off.get("bld")})')
    # Water on models (an oasis' pool) is drawn like a building, but casts no shadow.
    pool_dds = os.path.join(TEST, 'pool.dds')
    with open(pool_dds, 'wb') as f:
        f.write(surface_dds(15))
    b_pool, _ = shadow_run('pool', 1, 'building', f'[water]\nSurfaceTextures={pool_dds}\n', buildings=1,
                           flagpole=True, flag_dds=pool_dds)
    b_notpool, _ = shadow_run('notpool', 1, 'building', '[water]\nSurfaceTextures=\n', buildings=1, flagpole=True,
                              flag_dds=pool_dds)
    check(abs(b_pool.get('bld', 0) - b_pool.get('lit', 0)) <= 2 and b_notpool.get('bld', 999) < b_notpool.get('lit', 0) * 0.5,
          f"a mesh drawn with one of [water] SurfaceTextures casts no shadow ({b_pool.get('bld')} = lit "
          f"{b_pool.get('lit')}); not listed, the same mesh does ({b_notpool.get('bld')})")
    # Shadows follow the light cycle's sun: halfway between white (step 6) and black (step 18) is half as bright as
    # the brightest keyframe, so shadows are half as dark (unit Darkness 0.5 -> 0.25, BuildingDarkness 0.68 -> 0.34).
    fade = ('[lighting]\nEnabled=1\nLightCycleOn=1\nLightChangeCycleTime=100000\nLightChangeStepCount=24\nLightCycleStartStep=0\n'
            'LightCycleSteps=6,255,255,255;18,0,0,0\n')
    f_on, _ = shadow_run('fade', 1, 'building', fade, buildings=1)
    f_off, _ = shadow_run('nofade', 1, 'building', fade + 'ShadowsFollowSun=0\n', buildings=1)
    check(abs(f_on.get('shadowed', 0) - round(f_on.get('lit', 0) * 0.75)) <= 3 and
          abs(f_on.get('bld', 0) - round(f_on.get('lit', 0) * 0.66)) <= 3 and
          abs(f_off.get('shadowed', 0) - f_off.get('lit', 0) // 2) <= 3,
          f"shadows fade with the cycle's sun: at half brightness unit shadow {f_on.get('shadowed')} of "
          f"{f_on.get('lit')} (expected {round(f_on.get('lit', 0) * 0.75)}), building {f_on.get('bld')} (expected "
          f"{round(f_on.get('lit', 0) * 0.66)}); ShadowsFollowSun=0 keeps Darkness 0.5 ({f_off.get('shadowed')} of "
          f"{f_off.get('lit')})")
    check(b_on.get('painted') == b_on.get('lit') and abs(b_off.get('painted', 0) - b_off.get('lit', 0) // 2) <= 2,
          f"BtS's painted building shadows (black decal textures) hidden while buildings cast real shadows "
          f"({b_on.get('painted')}), drawn otherwise ({b_off.get('painted')}); "
          f"{'at the address of an earlier non-shadow texture' if b_on.get('reused') == 1 else 'texture address not reused this run'}")
    check(abs(b_on.get('footing', 0) - b_on.get('lit', 0)) <= 2,
          f"a building's below-ground foundations don't shade the ground on its sunward side ({b_on.get('footing')})")
    check(abs(b_on.get('ui', 0) - b_on.get('lit', 0) // 2) <= 2,
          f"a black interface overlay (no fog of war) is not taken for a painted shadow ({b_on.get('ui')})")

    # River (P1, BtS's own shader): f3SunLightDiffuse/AmbientColor in its pixel shader take the River colours.
    if have_rivers:
        rl, _ = shadow_run('lit', 0, 'river1', river_light)
        expect_r = round(255 * (0xE6 / 255) * min(1.0, (0x80 / 255) * min(1.0, sun[2] + amb[2])))
        check(abs(rl.get('lit', 0) - expect_r) <= 3 and rl.get('restored') == 1 and rl['released'],
              f'river lit with the INI colours (blue {rl.get("lit")}, expected {expect_r}, BtS 92)')

    print('older (non-Steam) BtS builds:')
    # Their terrain shader names its base texture TerrainBase0, and their unit shaders take the model's own bones
    # with SkinWorldViewProj (the camera times the model's placement): the same shadows as with Steam's shaders.
    o_on, _ = shadow_run('old', 1, old=True)
    check(o_on.get('lit') == shadow['on'].get('lit') and o_on.get('shadowed') == shadow['on'].get('shadowed') and
          o_on.get('shadowed2') == shadow['on'].get('shadowed2') and o_on.get('restored') == 1 and o_on['released'],
          f"older builds' shaders: terrain drawn by our copy and the unit's shadow lands where it does with Steam's "
          f"(lit {o_on.get('lit')}, shadowed {o_on.get('shadowed')}/{o_on.get('shadowed2')}; Steam's "
          f"{shadow['on'].get('lit')}, {shadow['on'].get('shadowed')}/{shadow['on'].get('shadowed2')})")
    if OLD_SHADER_DIR and os.path.exists(os.path.join(OLD_GAME_SHADERS, 'river0_vs.bin')):
        # The same compiled code as Steam's, built by an older compiler: recognised all the same.
        for overlay in ('river0', 'river1'):
            ov, _ = shadow_run('old', 1, overlay, game_dir=OLD_GAME_SHADERS)
            sv, _ = shadow_run('on', 1, overlay)
            check(ov.get('lit') == sv.get('lit') and ov.get('shadowed') == sv.get('shadowed') and ov['released'],
                  f"older build's {overlay} shader shadowed as Steam's ({ov.get('shadowed')} of {ov.get('lit')}; "
                  f"Steam's {sv.get('shadowed')} of {sv.get('lit')})")
        tl_old, _ = shadow_run('old_lit', 0, 'tree', '[lighting]\nEnabled=1\nTreeSun=200,100,50\nTreeAmbient=20,40,60\n',
                               game_dir=OLD_GAME_SHADERS)
        tl_new, _ = shadow_run('lit', 0, 'tree', '[lighting]\nEnabled=1\nTreeSun=200,100,50\nTreeAmbient=20,40,60\n')
        check(tl_old.get('lit') == tl_new.get('lit') and tl_old['released'],
              f"older build's tree shader lit as Steam's ({tl_old.get('lit')} = {tl_new.get('lit')})")
    else:
        print('  skip  older build\'s tree and river shaders: set GU_BTS_OLD_SHADERS to its Shaders folder')

    print('water:')
    # The water textures as packaged (textures/, as a GraphicsUpgrade folder next to each test DLL); no Colonization
    # install needed (ColonizationDir points nowhere).
    water_assets = os.path.join(ROOT, 'textures')
    col_dir = 'C:\\nowhere'

    def install_water(folder, normal_name='water_normal.dds'):
        os.makedirs(os.path.join(folder, 'GraphicsUpgrade'), exist_ok=True)
        for fn, to in (('water_normal.dds', normal_name), ('water_env.dds', 'water_env.dds')):
            shutil.copyfile(os.path.join(water_assets, fn), os.path.join(folder, 'GraphicsUpgrade', to))

    if not os.path.exists(os.path.join(water_assets, 'water_env.dds')):
        print(f'  skip  water textures not in {water_assets}')
    else:
        wdir = os.path.join(TEST, 'water')
        wout_dir = os.path.join(wdir, 'out')
        if os.path.isdir(wdir):
            shutil.rmtree(wdir)
        os.makedirs(wdir)
        shutil.copyfile(DLL, os.path.join(wdir, 'd3d9.dll'))
        install_water(wdir)
        with TestIni(os.path.join(wdir, 'GraphicsUpgrade.ini')) as f:
            f.write(f'[general]\nOutputDir={wout_dir}\nGames=*\n[water]\nEnabled=1\nColonizationDir={col_dir}\n')
        wout = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(wdir, 'd3d9.dll'), 'water'])
        wlog = open(os.path.join(wout_dir, 'test_proxy', 'session.log'), encoding='latin-1').read()
        check('water: shaders vertex' in wlog and 'mtxShadow c30' in wlog and
              'f4WaterShadow c2' in wlog and 'f3SkyLight c3' in wlog,
              "our water shaders loaded, with their shadow and sky-light constants")
        check('water: ready' in wlog, 'normal map and environment cube loaded')
        # Our water shader against Colonization's own compiled one (from its Shaders/FXO): the same picture.
        col_shaders = COL_SHADERS
        if os.path.exists(os.path.join(col_shaders, 'ps_2_0_d03b0dfb6f9efa7c.bin')):
            # through a pass-through proxy (Games=none), so none of its effects touch these draws
            cdir = os.path.join(TEST, 'watercmp')
            if os.path.isdir(cdir):
                shutil.rmtree(cdir)
            os.makedirs(cdir)
            shutil.copyfile(DLL, os.path.join(cdir, 'd3d9.dll'))
            install_water(cdir)
            with TestIni(os.path.join(cdir, 'GraphicsUpgrade.ini')) as f:
                f.write(f'[general]\nOutputDir={os.path.join(cdir, "out")}\nGames=none\n')
            cout = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(cdir, 'd3d9.dll'), 'watercmp', col_shaders])
            mc = re.search(r'WATERCMP max=(\d+) differing=(\d+) pixels=(\d+) colours=(\d+)', cout)
            check(mc and int(mc.group(1)) <= 1 and int(mc.group(2)) <= int(mc.group(3)) // 1000 and int(mc.group(4)) > 10000,
                  f"our water shader draws what Colonization's does (random textures, every view angle: largest "
                  f"difference {mc.group(1) if mc else '?'} of 255, {mc.group(2) if mc else '?'} of "
                  f"{mc.group(3) if mc else '?'} pixels differ at all, {mc.group(4) if mc else '?'} colours)")
            ma = re.search(r'WATERADD shadow=(\d+) glint=(\d+) glintchanged=(\d+) sky=(\d+) skychanged=(\d+)', cout)
            check(ma and int(ma.group(1)) <= 1 and int(ma.group(2)) <= 1 and int(ma.group(3)) > 1000 and
                  int(ma.group(4)) <= 1 and int(ma.group(5)) > 1000,
                  f"our water shader's additions: a half-light shadow halves the picture exactly (worst "
                  f"{ma.group(1) if ma else '?'}); in full shadow the sun's glint goes exactly as with no sun (worst "
                  f"{ma.group(2) if ma else '?'}, {ma.group(3) if ma else '?'} channels changed); a sky light of 0 "
                  f"is exactly a black sky (worst {ma.group(4) if ma else '?'}, {ma.group(5) if ma else '?'} channels "
                  f"changed)")
        else:
            print(f"  skip  water shader comparison: Colonization's Water.fx not found under {COL}")
        m = re.search(r'WATER pixel=([0-9a-f]{8}) restored=(\d)', wout)
        pixel = int(m.group(1), 16) if m else 0
        check(m and (pixel & 0xFFFFFF) != 0xFF00FF and (pixel & 0xFFFFFF) != 0,
              f'water drawn by Colonization\'s shader, not BtS\'s (centre pixel {m.group(1) if m else "?"}, magenta = not replaced)')
        check(m and m.group(2) == '1', 'BtS water shaders, constants and textures restored after the draw')
        mc = re.search(r'circle=([0-9a-f]{8}) hidden=([0-9a-f]{8})', wout)
        check(mc and (int(mc.group(1), 16) & 0xFFFFFF) == 0xFF0000 and (int(mc.group(2), 16) & 0xFFFFFF) != 0xFF0000,
              f'selection circle drawn before the water ends up on top of it, not refracted beneath (pixel '
              f'{mc.group(1) if mc else "?"}), but stays behind a ship\'s hull in front of it, as in BtS (pixel '
              f'{mc.group(2) if mc else "?"}, not the circle\'s red)')
        check('final refs device 0, d3d 0' in wout, 'device and Direct3D fully released')

        # The water's sun colour comes from the lighting (WaterSun, else SunColour, else the cycle); without the
        # lighting, Colonization's own.
        def water_sun(label, light):
            sdir = os.path.join(TEST, 'water_sun_' + label)
            if os.path.isdir(sdir):
                shutil.rmtree(sdir)
            os.makedirs(sdir)
            shutil.copyfile(DLL, os.path.join(sdir, 'd3d9.dll'))
            install_water(sdir)
            with TestIni(os.path.join(sdir, 'GraphicsUpgrade.ini')) as f:
                f.write(f'[general]\nOutputDir={os.path.join(sdir, "out")}\nGames=*\n[water]\nEnabled=1\n'
                        f'ColonizationDir={col_dir}\n[lighting]\nEnabled=1\n{light}')
            run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(sdir, 'd3d9.dll'), 'water'])
            return open(os.path.join(sdir, 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
        sky_col = water_sun('sky_col', 'AmbientColour=89,103,133\n')
        sky_green = water_sun('sky_green', 'AmbientColour=20,90,25\n')
        check('sky tint 1.00,1.00,1.00' in sky_col and 'sky tint 0.05,0.76,0.04' in sky_green and
              'sky tint 1.00,1.00,1.00' in wlog,
              "the water's reflected sky follows the ambient: Colonization's ambient leaves it as it is (1, as with the "
              "lighting off); a green one (20,90,25) tints it green (0.05,0.76,0.04)")
        cycle_sky = ('LightCycleOn=1\nLightChangeCycleTime=100000\nLightChangeStepCount=24\n'
                     'LightCycleAmbientSteps=12,100,100,100;0,50,50,50\n')
        sky_noon = water_sun('sky_noon', cycle_sky)
        sky_night = water_sun('sky_night', cycle_sky + 'LightCycleStartStep=0\n')
        check('sky tint 1.00,1.00,1.00' in sky_noon and 'sky tint 0.25,0.25,0.25' in sky_night,
              "with the light cycle, the reflected sky is measured against the cycle's own midday: as it is at midday "
              "(1), a quarter at a night with half the midday ambient")
        glint = ('LightCycleOn=1\nLightChangeCycleTime=100000\nLightChangeStepCount=24\n'
                 'LightCycleSteps=12,100,100,100;0,10,20,30\nMoonGlint=255,255,255\n')
        glint_noon = water_sun('glint_noon', glint)
        glint_night = water_sun('glint_night', glint + 'LightCycleStartStep=0\n')
        glint_dusk = water_sun('glint_dusk', glint + 'LightCycleStartStep=18\n')
        check('water: sun 100,100,100' in glint_noon and 'water: sun 255,255,255' in glint_night and
              'water: sun 55,60,65' in glint_dusk,
              "the moon's glint: the water's sun is the cycle's by day (100), MoonGlint at midnight (255), and the "
              "cycle's still at sunset (18 h: halfway between its 100 and 10,20,30)")
        check("water: sun 255,235,188 (Colonization's)" in wlog and
              "water: sun 10,20,30 (the lighting's)" in water_sun('own', 'SunColour=1,2,3\nWaterSun=10,20,30\n') and
              "water: sun 1,2,3 (the lighting's)" in water_sun('shared', 'SunColour=1,2,3\n'),
              "water sun: Colonization's without the lighting, else WaterSun, else SunColour")

        # Under the water: a "submarine" 0.04 below the surface (z 0.5) shows in the refraction at band 1 of 8 over
        # a depth of 0.2: red 255 x 0.7 x (1 - 1.5/8); a ship above the surface doesn't; with UnderwaterDepth=0, nothing.
        def underwater(depth):
            udir = os.path.join(TEST, f'water_under_{depth}')
            if os.path.isdir(udir):
                shutil.rmtree(udir)
            os.makedirs(udir)
            shutil.copyfile(DLL, os.path.join(udir, 'd3d9.dll'))
            install_water(udir)
            with TestIni(os.path.join(udir, 'GraphicsUpgrade.ini')) as f:
                f.write(f'[general]\nOutputDir={os.path.join(udir, "out")}\nGames=*\n[debug]\nTraces=1\n[trace]\nAutoFrame=1\n'
                        f'[water]\nEnabled=1\nColonizationDir={col_dir}\nUnderwaterDepth={depth}\nUnderwaterVisibility=0.7\n'
                        f'UnderwaterBands=8\n[reflection]\nWaterHeight=0.5\n')
            res = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(udir, 'd3d9.dll'), 'water'])
            traces = os.path.join(udir, 'out', 'test_proxy', 'traces')
            bmp = open(os.path.join(traces, os.listdir(traces)[0], 'refraction.bmp'), 'rb').read()
            size = struct.unpack_from('<i', bmp, 18)[0]
            red = lambda x, y: bmp[54 + (int((1 - y) / 2 * size) * size + int((x + 1) / 2 * size)) * 4 + 2]
            return red(-0.5, 0.47), red(0.5, 0.47), 'final refs device 0, d3d 0' in res
        expect = round(255 * 0.7 * (1 - 1.5 / 8))
        sub_red, ship_red, released = underwater(0.2)
        none_red, _, _ = underwater(0)
        check(abs(sub_red - expect) <= 3 and ship_red == 0 and released,
              f'submarine below the surface shows through the water, faded by depth (red {sub_red}, expected {expect}); '
              f'the ship above the surface stays out of the refraction ({ship_red})')
        check(none_red == 0, f'UnderwaterDepth=0: nothing under the surface shown, as before ({none_red})')

        # Bundled files only (as in the release package: a GraphicsUpgrade folder next to the DLL), with ColonizationDir
        # pointing nowhere: the water must still be Colonization's. Then neither: BtS water left alone.
        for label, bundle in (('bundled', True), ('oldname', True), ('none', False)):
            bdir = os.path.join(TEST, 'water_' + label)
            bout = os.path.join(bdir, 'out')
            if os.path.isdir(bdir):
                shutil.rmtree(bdir)
            os.makedirs(os.path.join(bdir, 'GraphicsUpgrade'))
            shutil.copyfile(DLL, os.path.join(bdir, 'd3d9.dll'))
            if bundle:
                install_water(bdir, 'water_001.dds' if label == 'oldname' else 'water_normal.dds')
            with TestIni(os.path.join(bdir, 'GraphicsUpgrade.ini')) as f:
                f.write(f'[general]\nOutputDir={bout}\nGames=*\n[water]\nEnabled=1\nColonizationDir=C:\\nowhere\n')
            bres = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(bdir, 'd3d9.dll'), 'water'])
            blog = open(os.path.join(bout, 'test_proxy', 'session.log'), encoding='latin-1').read()
            mb = re.search(r'WATER pixel=([0-9a-f]{8})', bres)
            magenta = bool(mb) and (int(mb.group(1), 16) & 0xFFFFFF) == 0xFF00FF
            if label == 'oldname':
                check(not magenta and 'water: textures from ' + os.path.join(bdir, 'GraphicsUpgrade', 'water_001.dds') in blog,
                      'a 1.0.x GraphicsUpgrade folder (the normal map under its old name, water_001.dds) still works')
            elif bundle:
                check(not magenta and 'water: textures from ' + os.path.join(bdir, 'GraphicsUpgrade', 'water_normal.dds') in blog,
                      'bundled GraphicsUpgrade folder used when Colonization is absent (as in the release package)')
            else:
                check(magenta and 'water: textures not found' in blog,
                      'no bundled files and no Colonization: BtS water left alone, logged')
            check('final refs device 0, d3d 0' in bres, f'device and Direct3D fully released ({label})')

        # Water on models ([water] SurfaceTextures): a pool-like fixed-function quad (green, the texture's alpha)
        # gets the water's sky and glint on top; not listed, with the water off, or where the texture is clear, BtS's
        # picture stands.
        def surface_run(label, alpha, ini, mod_files=None):
            sdir = os.path.join(TEST, 'surface_' + label)
            if os.path.isdir(sdir):
                shutil.rmtree(sdir)
            os.makedirs(sdir)
            shutil.copyfile(DLL, os.path.join(sdir, 'd3d9.dll'))
            install_water(sdir)
            dds = os.path.join(sdir, 'pool.dds')
            with open(dds, 'wb') as f:
                f.write(surface_dds(alpha))
            mod_arg = []
            if mod_files is not None:
                mod = os.path.join(sdir, 'Mods', 'TestMod')
                for rel, content in mod_files.items():
                    os.makedirs(os.path.dirname(os.path.join(mod, rel)), exist_ok=True)
                    with open(os.path.join(mod, rel), 'wb') as f:
                        f.write(content)
                mod_arg = ['mod=' + mod]
            with TestIni(os.path.join(sdir, 'GraphicsUpgrade.ini')) as f:
                f.write(f'[general]\nOutputDir={os.path.join(sdir, "out")}\nGames=*\n[water]\nColonizationDir=C:\\nowhere\n'
                        + ini.replace('{dds}', dds))
            res = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(sdir, 'd3d9.dll'), 'surface', dds] + mod_arg)
            m = re.search(r'SURFACE pixel=([0-9a-f]{8}) restored=(\d)', res)
            rgb = tuple(int(m.group(1)[i:i + 2], 16) for i in (2, 4, 6)) if m else None
            slog = open(os.path.join(sdir, 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
            return rgb, bool(m) and m.group(2) == '1' and 'final refs device 0, d3d 0' in res, slog

        listed = 'Enabled=1\nSurfaceTextures={dds}\n'
        on, on_ok, on_log = surface_run('on', 15, listed)
        off, off_ok, _ = surface_run('off', 15, 'Enabled=0\nSurfaceTextures={dds}\n')
        unlisted, unlisted_ok, _ = surface_run('unlisted', 15, 'Enabled=1\nSurfaceTextures=\n')
        clear, clear_ok, _ = surface_run('clear', 0, listed)
        check(off == (0, 255, 0) and unlisted == (0, 255, 0) and off_ok and unlisted_ok,
              f'water on models: with the water off ({off}) or the texture not listed ({unlisted}), BtS draws it as is')
        check(on is not None and on_ok and on[1] < 250 and on[0] > 5 and on[2] > 5 and
              'water: water on a model (SurfaceTextures) drawn with the surface water (fvf 0x142, vertex alpha)' in on_log,
              f'water on models: a listed texture gets the sky reflected on top ({on}, from green); game state restored')
        check(clear == (0, 0, 0) and clear_ok, f'water on models: nothing where the texture is clear ({clear})')

        # A mod's own water textures, each on its own: water_env.dds packed in an FPK under Assets\Art\GraphicsUpgrade,
        # water_normal.dds from the bundled folder.
        env = open(os.path.join(water_assets, 'water_env.dds'), 'rb').read()
        _, ok, mlog = surface_run('modart', 15, listed, mod_files={
            'Assets/Mod0.FPK': fpk_bytes({'art\\graphicsupgrade\\water_env.dds': env})})
        check(ok and re.search(r'water: textures from .*surface_modart\\GraphicsUpgrade\\water_normal\.dds, '
                               r'.*surface_modart\\Mods\\TestMod\\Assets\\Mod0\.FPK', mlog) is not None,
              "a mod's water_env.dds from its Assets\\Art\\GraphicsUpgrade (in an FPK) with the bundled water_normal.dds")

        # The shipped GraphicsUpgrade.ini as is (only OutputDir redirected): effects run in BtS only, so in the harness the
        # water must stay as drawn (magenta) and no debug output may appear.
        print('release defaults (GraphicsUpgrade.ini):')
        rel_dir = os.path.join(TEST, 'release')
        rel_out = os.path.join(rel_dir, 'out')
        if os.path.isdir(rel_dir):
            shutil.rmtree(rel_dir)
        os.makedirs(rel_dir)
        shutil.copyfile(DLL, os.path.join(rel_dir, 'd3d9.dll'))
        install_water(rel_dir)
        ini = open(os.path.join(ROOT, 'GraphicsUpgrade.ini'), encoding='utf-8').read()
        ini = ini.replace('OutputDir=\n', f'OutputDir={rel_out}\n', 1)
        open(os.path.join(rel_dir, 'GraphicsUpgrade.ini'), 'w', encoding='utf-8').write(ini)
        relout = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(rel_dir, 'd3d9.dll'), 'water'])
        relog = open(os.path.join(rel_out, 'test_proxy', 'session.log'), encoding='latin-1').read()
        m2 = re.search(r'WATER pixel=([0-9a-f]{8})', relout)
        check(m2 and (int(m2.group(1), 16) & 0xFFFFFF) == 0xFF00FF and 'effects off in this game' in relog,
              'outside BtS the shipped settings change nothing (water stays as drawn, effects off logged)')
        ver = re.search(r'#define GRAPHICSUPGRADE_VERSION "([^"]+)"',
                        open(os.path.join(ROOT, 'src', 'common.h'), encoding='utf-8').read()).group(1)
        check(f'session start: GraphicsUpgrade {ver}' in relog, f'log names GraphicsUpgrade and its version ({ver})')
        check(not os.path.exists(os.path.join(rel_out, 'test_proxy', 'shaders')) and
              not os.path.exists(os.path.join(rel_out, 'test_proxy', 'traces')),
              'no shader dumps or traces with the shipped settings')
        check('final refs device 0, d3d 0' in relout, 'device and Direct3D fully released (shipped settings)')

    print('version resource:')
    check(file_version(os.path.join(TEST, 'd3d9.dll')) ==
          file_version(os.path.join(os.environ['SystemRoot'], 'SysWOW64', 'd3d9.dll')),
          'proxy file version = system d3d9.dll file version')
    check('first call: IDirect3D9::GetDeviceCaps' in log or 'first call: IDirect3DDevice9::SetStreamSource' in log,
          'first-call logging')

    print('shader dumps:')
    for kind, hdr in (('vs_2_0', 'test_vs.h'), ('ps_2_0', 'test_ps.h')):
        code = header_bytes(os.path.join(TEST, 'shaders', hdr))
        path = os.path.join(game, 'shaders', f'{kind}_{fnv1a(code):016x}.bin')
        check(os.path.exists(path) and open(path, 'rb').read() == code, f'{kind} dump identical to fxc output')

    traces = os.path.join(game, 'traces')
    tdir = os.path.join(traces, sorted(os.listdir(traces), key=lambda n: int(n.rsplit('_f', 1)[1]))[0])  # AutoFrame
    events = [json.loads(line) for line in open(os.path.join(tdir, 'trace.jsonl'))]
    meta = json.load(open(os.path.join(tdir, 'meta.json')))
    kinds = [e['ev'] for e in events]
    draws = [e for e in events if e['ev'] == 'draw']
    surfs = {e['ptr']: e for e in events if e['ev'] == 'surf'}
    texs = {e['ptr']: e for e in events if e['ev'] == 'tex'}
    print('trace:', tdir)
    check(meta['draws'] == 3 and len(draws) == 3, 'three draws')
    check(kinds[0] == 'begin_scene' and kinds[-1] == 'present', 'frame starts at BeginScene, ends at Present')
    check('clip_plane' in kinds and 'stretch_rect' in kinds and kinds.count('clear') == 2, 'clip plane, copy, clears')
    d0, d1, d2 = draws
    rt0 = surfs.get(d0['rt'][0], {})
    check(rt0.get('w') == 128 and rt0.get('tex') is not None, 'draw 0 renders into the 128x128 RT texture')
    check(d0['vs'].startswith('vs_2_0:') and d0['ps'].startswith('ps_2_0:'), 'draw 0 shader hashes')
    check(d0['rs']['clip'] == 1 and d1['rs']['clip'] == 0, 'clip-plane enable state read back')
    expected = bytearray()
    for y in range(64):
        for x in range(64):
            expected += struct.pack('<I', 0xFF000000 | (x * 4 << 16) | (y * 4 << 8))
    t0 = texs.get(d0['tex']['0']['t'], {})
    check(t0.get('hash') == f'{fnv1a(expected):016x}', 'managed texture content hash matches its pixels')
    t1 = texs.get(d1['tex']['0']['t'], {})
    check(t1.get('surf0') == d0['rt'][0], 'draw 1 samples the texture whose surface draw 0 rendered to')
    check(texs.get(d1['tex']['1']['t'], {}).get('type') == 'cube', 'cube texture on stage 1')
    check(d2['vs'] is None and d2['call'] == 'DPUP' and d2['view'][14] == 2.0, 'fixed-function draw with transforms')
    vsc = open(os.path.join(tdir, 'consts_vs.bin'), 'rb').read()
    psc = open(os.path.join(tdir, 'consts_ps.bin'), 'rb').read()
    vs_block = meta['vs_registers'] * 16
    c = struct.unpack_from('<16f', vsc, d0['vsc'] * vs_block)
    check(c == (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1), 'VS constants c0-c3 = identity')
    p = struct.unpack_from('<4f', psc, d0['psc'] * meta['ps_registers'] * 16)
    check(p == (1, 0.5, 0.25, 1), 'PS constant c0 = tint')

    print('\n' + ('ALL CHECKS PASSED' if not FAILS else f'{len(FAILS)} FAILED'))
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()

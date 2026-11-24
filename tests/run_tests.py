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
  - Civ4BeyondSword.exe from Steam and GOG (GU_BTS_GOG: GOG's "Civ4" folder): the exe patches' addresses checked
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
import zlib

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
GOG_BTS = os.environ.get('GU_BTS_GOG', r'C:\Program Files (x86)\GOG Galaxy\Games\Civilization IV Complete\Civ4')
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
                                              ['river0_vs', 'river0_ps', 'river1_vs', 'river1_ps']),
                                             ('Civ4Wave.fx', 'Wave_Shader', [0], ['wave_vs', 'wave_ps']),
                                             ('CultureBOrderShader.fx', 'TCultureBorder_Shader14', [0, 1],
                                              ['border0_vs', 'border0_ps', 'border1_vs', 'border1_ps'])):
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
BASELINE = {'water': {'Enabled': '0', 'GlintFollowsSun': '0', 'Caustics': '0', 'WaveShading': '0'},
            'reflection': {'Enabled': '0'}, 'shadows': {'Enabled': '0'},
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
        f.write(f'[general]\nOutputDir={OUT}\nGames=*\n[debug]\nTraces=1\nDumpShaders=1\nTerrainVertsPerPlot=33\n'
                f'[terrain tiles]\nTilePicker=1\nTileGroups=2\nTileGroup2=TERRAIN_DESERT\n'
                f'StandardBlendOnLand1=TERRAIN_GRASS: 8,0; 1,0; 2,0; 6,0; 13,0; 10,0; 3,0; 5,0; 9,0; 7,0; 4,0; 12,0; 11,0; 14,0\n'
                'PlotStamps1Terrains=TERRAIN_GRASS\nPlotStamps1HideModels=Art\\NoSuchFolder\\peak_single.dds\n'
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

    print('hotkeys kept from the game:')
    kdir = os.path.join(TEST, 'keys')
    if os.path.isdir(kdir):
        shutil.rmtree(kdir)
    os.makedirs(kdir)
    shutil.copyfile(DLL, os.path.join(kdir, 'd3d9.dll'))
    with TestIni(os.path.join(kdir, 'GraphicsUpgrade.ini')) as f:
        f.write(f'[general]\nOutputDir={os.path.join(kdir, "out")}\nGames=*\n[debug]\nTraces=1\n')
    kout = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(kdir, 'd3d9.dll'), 'keys'])
    klog = open(os.path.join(kdir, 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check('hotkeys kept from the game (message hook on thread' in klog, 'message hook installed with the device')
    check('ctrl+shift 1=0 9=0 /=0 Q=3;' in kout,
          'Ctrl+Shift+1 (water), +9 (trace) and +/ (screenshots) dropped before the window; Ctrl+Shift+Q gets through')
    check('ctrl 1=3 shift 1=3 1=3 ctrl+shift+alt 1=3;' in kout,
          '1 with any other modifiers (the game\'s unit groups) gets through')
    check('held 0+0, after 3' in kout, 'a dropped key stays dropped until released, even with Shift let go first')
    check('after release ctrl+shift 1=3' in kout, 'the hook goes with the device')

    print('terrain patch ([debug] TerrainVertsPerPlot, experimental):')
    check('terrain patch: TerrainVertsPerPlot 33 not applied: test_proxy.exe isn\'t Civ4BeyondSword.exe' in log,
          'not applied outside BtS (the harness ran with TerrainVertsPerPlot=33)')
    tout = run([os.path.join(TEST, 'test_proxy.exe'), 'terrainpatch'])
    m = re.search(r'^TERRAINPATCH (bytes33.*)$', tout, re.M)
    tp = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}
    check(tp.get('bytes33') == 1 and tp.get('changed') == 7,
          f'33: the seven sites rewritten (17 -> 33, shifts 4 -> 5, canyon window 16 -> 32), 7 bytes changed {tp}')
    check(tp.get('idempotent') == 1, 'applying it again writes nothing')
    check(tp.get('v65') == 1, '65: shift 6, canyon window 64')
    check(tp.get('refused') == 1 and 'unexpected bytes at +0x78f04' in tout,
          'one site with other bytes (another build): nothing written at all')
    check(tp.get('toosmall') == 1 and tp.get('bad') == 1 and tp.get('stock17') == 1,
          'image too small, 20 (not 2^n+1) refused; 17 does nothing')
    check(tp.get('cells') == 1, 'the landscape cell values given instead of the XML: samples = TerrainVertsPerPlot, plots '
          'per cell at most so many that a cell is 129 across (65: 2), samples a cell from those')
    check(tp.get('masksites') == 1, "hills' and peaks' coast scale masks: the two calls to the mask picker sent to the "
          'thunk (only if both still call it)')
    check(tp.get('hmfolder') == 1,
          "heightmap tiles from heightmap64 at 65 (heightmap32 at 33): the tile loader's nine path PUSHes pointed at the "
          "patch's strings; the scale masks' format prints the new prefix and skips the old one's argument")
    check(tp.get('maskflat') == 1 and tp.get('maskramp') == 1,
          'a 16x16 mask resampled to 64x64: flat stays flat; a ramp stays an evenly spaced ramp, the same either way round')

    print("the BtS builds the exe patches know (exe_layout.h), on the installed exes:")
    for label, exe, steam in (('GOG', os.path.join(GOG_BTS, 'Beyond the Sword', 'Civ4BeyondSword.exe'), False),
                              ('Steam', os.path.join(BTS, 'Beyond the Sword', 'Civ4BeyondSword.exe'), True)):
        if not os.path.exists(exe):
            print(f'  skip  {label}: no {exe} (set {"GU_BTS" if steam else "GU_BTS_GOG"})')
            continue
        xout = run([os.path.join(TEST, 'test_proxy.exe'), 'exelayout', exe] + (['steam'] if steam else []))
        m = re.search(r'^EXELAYOUT build=(\S+) timestamp=(\S+) (identified.*)$', xout, re.M)
        xl = {k: int(v) for k, v in (kv.split('=') for kv in m.group(3).split())} if m else {}
        if not steam:
            check(m and m.group(1) == 'GOG' and xl.get('identified') == 1,
                  f'GOG ({m.group(2) if m else "?"}): recognised by its three calls to the tile picker {xl}')
            check(all(xl.get(k) == 1 for k in ('verts', 'masks', 'folders', 'tilesets', 'landblend', 'jigsaw', 'peakcalls')),
                  'GOG: every patch site holds BtS\'s bytes (TerrainVertsPerPlot 33 and 65, coast scale masks, heightmap '
                  'folder, tile groups, land blends, jigsaw base stamp, peak stamp calls)' + ('' if xl else f'\n{xout}'))
            check(xl.get('lists') == 1, "GOG: the tile loader fills the layout's eight tile lists, in its order")
        else:
            check(xl.get('identified') == 1, f'Steam ({m.group(2) if m else "?"}): its code is encrypted on disk, so only '
                  f'its data is checked {xl}')
        check(xl.get('strings') == 1 and xl.get('cellslots') == 1 and xl.get('crt') == 1,
              f"{label}: the heightmap path strings, the landscape getters' delay-load slots and the CRT's srand / rand "
              'where the layout has them')

    print('terrain tile groups ([terrain tiles], experimental):')
    check("terrain tile groups: not applied: test_proxy.exe isn't Civ4BeyondSword.exe" in log,
          'not applied outside BtS (the harness ran with TilePicker=1, TileGroups=2)')
    rout = run([os.path.join(TEST, 'test_proxy.exe'), 'riverlayer'])
    m = re.search(r'^RIVERLAYER (arms.*)$', rout, re.M)
    rl = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}
    check(rl.get('arms') == 1 and rl.get('tile') == 1,
          "river layer: a square's arms from the plots round its middle; the tile from them (first tile + arms - 1)")
    check(rl.get('add') == 1, "river layer: a layer added after the square's own, as the builder writes one, and the "
          "list's end marker moved after it")
    check(rl.get('full') == 1, 'river layer: six layers fill a record, a seventh is refused and changes nothing')
    check(rl.get('sites') == 1 and rl.get('refused') == 1,
          "river layer: the builder's four CALLs on both builds sent to the thunk; other bytes refused, nothing written")
    check(rl.get('parse') == 1, 'river caps: a 64 x 64 8-bit TGA read either way up; other sizes, depths and cut files refused')
    check(rl.get('pick') == 1 and rl.get('order') == 1,
          "river caps: a square's tile by arms and the engine's pick, mouths on one arm only and where they exist; the "
          'order the blend sheet is laid out in')
    check(rl.get('cap') == 1, 'river caps: height = min(height, cap) through the grid\'s stride, nothing outside the square')
    check(rl.get('capsites') == 1, "river caps: the peak stamp's four CALLs on both builds")
    check(rl.get('cuts') == 1, "river cuts: what a square lost found again from a world position (wrapping east-west), "
          "forgotten when restamped with nothing cut, the version moving with each change; bridged whole in a shallow "
          "trench, not at all in a deep gorge (by the square's deepest cut), in proportion between")
    check(rl.get('depth') == 1, "river depths: the ground lowered by the tile, never below the sand's level; ground "
          "already lower left alone; nothing outside the square")
    sout = run([os.path.join(TEST, 'test_proxy.exe'), 'tilesets'])
    m = re.search(r'^TILESETS (ranges.*)$', sout, re.M)
    ts = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}
    check(ts.get('ranges') == 1, f'a list split into equal groups in file order; whole when too few for every set {ts}')
    check(ts.get('majority') == 1 and ts.get('corners') == 1,
          "a square's set: the majority of its corner plots (TL, TR, BR, BL as the engine finds them), ties lower")
    check(ts.get('seed') == 1, "the engine's seed (x * 0x1C81 + y * 0xB5D)")
    check(ts.get('patched') == 1, 'picker JMP, hill CALL, PUSH ESI + peak CALL to the thunks, NOPs after; the coast '
          "index's atlas and wave-model CALLs (to FUN_004788e0 before) to theirs")
    check(ts.get('cycle') == 1, 'coast variants folded into the art (stock 8, 8, 6, 7; never past the atlas 8, 8, 8, 7)')
    check(ts.get('refused') == 1 and 'unexpected bytes at +0x78e20' in sout,
          'one site with other bytes (another build): nothing written')
    check(ts.get('landparse') == 1 and ts.get('landbad') == 1,
          'StandardBlendOnLand1..8: "TERRAIN_X: 14 masks of tile,rotation entries" read (several entries a mask '
          'allowed); too few or too many masks, a bad rotation or no terrain refused')
    check(ts.get('landsites') == 1, "StandardBlendOnLand: the terrain layers' three calls to the blend pick sent to the thunk")
    check(ts.get('landshort') == 1,
          'StandardBlendOnLand=TERRAIN_A, TERRAIN_B (the short form): names in any case and separators, each given '
          'BtS\'s standard layout, the same entries as the full form spelled out')
    check(ts.get('mixed') == 1,
          'StandardBlendOnLand, layer by layer: all-land squares take land entries; a coast with one land terrain '
          'keeps its paired pick; a coast of two land terrains has its lowest land layer re-picked for the whole land '
          'mask and the higher ones take land entries; untouched when the lowest land terrain has no list or is drawn '
          'whole')
    mb = re.search(r'^TILESETS mixblend=(\d) (.*)$', sout, re.M)
    check(mb and mb.group(1) == '1',
          'MixedCoastBlend: on a coast of two land terrains every paired land layer takes the whole land mask (the '
          'square marked blended; the rest as before), and weighted by their share of the land corners, drawn one over '
          f"another, each terrain ends up at its share, the same either side of an edge; the peak layer too, its share "
          f"of all the land corners ({mb.group(2) if mb else ''})")
    check(ts.get('overlays') == 1,
          'StandardBlendOnLand, hill and peak layers (at no corner): land entries on all land; on a coast, covering '
          "part of the land, re-picked for the whole land mask (Mesa paints the cliff); covering all of it, or without "
          "a list, the engine's pick")
    check("terrain land blends: not applied: test_proxy.exe isn't Civ4BeyondSword.exe" in log,
          'StandardBlendOnLand: not applied outside BtS (the harness ran with StandardBlendOnLand1 set)')

    print('hill field ([terrain tiles] HillStamps, experimental, off by default):')
    shipped = open(os.path.join(ROOT, 'GraphicsUpgrade.ini'), encoding='utf-8', errors='replace').read()
    check(not re.search(r'^\s*HillStamps\s*=\s*[1-9]', shipped, re.M), 'the shipped INI leaves HillStamps off')
    hout = run([os.path.join(TEST, 'test_proxy.exe'), 'hillfield'])
    for line in hout.splitlines():
        if line.startswith('HILLFIELD placements'):
            print('  ' + line)
    m = re.search(r'^HILLFIELD (exact16.*)$', hout, re.M)
    hf = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}
    check(hf.get('exact16') == 1 and hf.get('exact64') == 1,
          "adjacent tiles continue each other: every square's tile, laid N apart as the engine lays them, is the one "
          'field sampled there (16 and 64 per plot)')
    check(0 < hf.get('seam16', 0) <= 130 and 0 < hf.get('seam64', 0) <= 130,
          'no creases at tile seams: curvature across seams / beside them %.2f (16), %.2f (64)'
          % (hf.get('seam16', 0) / 100, hf.get('seam64', 0) / 100))
    check(hf.get('stock') == 1 and hf.get('mask') == 1,
          "tiles only for squares with a hill corner (the engine asks only for those), the hill mask as the engine's; "
          'no stamps: every hill square keeps its stock tile')
    check(hf.get('multi') == 1 and hf.get('orient') == 1,
          'runs of hills get two- and three-plot stamps, every hill plot in one; a lone TL hill is high at pixel (0, 0)')
    check(hf.get('deterministic') == 1, 'the same map gives the same tiles (hills listed in any order); another map, others')
    check(hf.get('local') == 1, 'a plot that becomes or stops being a hill changes only the four squares round it (all '
          'the engine re-stamps); a new map is fitted again')
    check(hf.get('wrap') == 1, 'a map that wraps: a stamp across the seam, and squares 0 and W the same')
    check(hf.get('engine') == 1, 'the tile laid out as the hill stamp reads the art (texture +0x38, pixel data +0x50..+0x64)')
    check(hf.get('hugecut', 0) > 0 and hf.get('fitms', 9999) + hf.get('cutms', 9999) < 4000,
          f"a Huge map ({hf.get('hugehills')} hills): fitted in {hf.get('fitms')} ms, {hf.get('hugecut')} squares cut at "
          f"64x64 in {hf.get('cutms')} ms")

    print('jigsaw pieces ([terrain tiles] JigsawPieces1..8, experimental, off by default):')
    check(not re.search(r'^\s*JigsawPieces[1-8]Terrains\s*=\s*\S', shipped, re.M), 'the shipped INI sets no jigsaw pieces')
    jout = run([os.path.join(TEST, 'test_proxy.exe'), 'jigsaw'])
    for line in jout.splitlines():
        if line.startswith('JIGSAW placements'):
            print('  ' + line)
    m = re.search(r'^JIGSAW (dds=.*)$', jout, re.M)
    js = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}
    check(js.get('dds') == 1 and js.get('ddsrefused') == 1,
          'DDS heightmaps: L16 / R16 / A16B16G16R16 as height x 256, L8 / A8L8 / R8 / RGB red as the height; '
          'compressed or cut-off files refused')
    check(js.get('xml') == 1 and js.get('xmlrefused') == 1 and js.get('xmldefault') == 1,
          "pieces.xml: fHeightScale, each Layout's Plots and Pieces, comments skipped; mistakes refused with why; no "
          "fHeightScale, 1")
    check(js.get('geometry') == 1, "a piece's place from its plots and its pixels a plot from its size; a size that "
          "doesn't fit its layout refused")
    check(js.get('combine') == 1, 'sets combined: the highest raised and the lowest lowered')
    check(js.get('exact64') == 1 and js.get('exact16') == 1 and js.get('cut') == 1 and js.get('mask') == 1,
          "every square touching a set's plot gets a tile, and adjacent tiles continue each other (each is the one "
          "field sampled there, 64 and 16 per plot)")
    check(js.get('lattice') == 1 and js.get('once') == 1,
          'pieces go down by exact lattice moves (the 8 turns and mirrors, whole-plot shifts), every plot in one')
    check(js.get('massif') == 1, 'a band of plots is one massif: between neighbouring plots the field never drops to '
          'the base')
    check(js.get('base') == 1, "the base stamp is given a set's corners as water, as land, or as they are; a corner the "
          "engine has otherwise than the map as read leaves the square to the engine")
    check(js.get('grid') == 1 and js.get('gridview') == 1,
          "the tile added into the engine's height bytes at the square's place, clamped 0..255, never past the grid; "
          "the grid object read as the stamps read it")
    check(js.get('local') == 1 and js.get('refit') == 1,
          'a plot that joins or leaves a set changes only the four squares round it; a new map is placed again')
    check(js.get('deterministic') == 1, 'the same plots give the same tiles (in any order); other plots, others')
    check(js.get('wrap') == 1, 'a map that wraps: a piece across the seam, and squares 0 and W the same')
    check(js.get('hooked') == 1 and js.get('siterefused') == 1,
          'the base stamp hooked with a JMP over SUB ESP,0x10; CMP EDI,1 (only over those bytes)')
    check(js.get('hugecut', 0) > 0 and js.get('fitms', 9999) + js.get('cutms', 9999) < 4000,
          f"a Huge map ({js.get('hugeplots')} plots in a set): placed in {js.get('fitms')} ms, {js.get('hugecut')} "
          f"squares cut at 64x64 in {js.get('cutms')} ms")

    print('plot stamps ([terrain tiles] PlotStamps1..8, experimental, off by default):')
    check(not re.search(r'^\s*PlotStamps[1-8]Terrains\s*=\s*\S', shipped, re.M), 'the shipped INI sets no plot stamps')
    check(r'settings: [terrain tiles] PlotStamps<n>HideModels: Art\NoSuchFolder\peak_single.dds: no .dds textures '
          r'found' in log, 'PlotStamps1HideModels read as a texture list (the harness ran with one that finds nothing)')
    pout = run([os.path.join(TEST, 'test_proxy.exe'), 'plotstamps'])
    m = re.search(r'^PLOTSTAMPS (pick=.*)$', pout, re.M)
    ps = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}
    check(ps.get('pick') == 1, "a plot's stamp: the Nth listed feature's the Nth piece (wrapping), else picked by "
          'plot, the same every time')
    check(ps.get('orient') == 1, "a stamp stands unturned on its plot: its heightmap's north-west on the plot's "
          "north-west, its centre on the plot's centre, nothing past its plot and the one round it")
    check(ps.get('exact') == 1 and ps.get('reached') == 1 and ps.get('squares') == 1 and ps.get('peak') == 1,
          'the four squares round the plot each get their part of the stamp (sampled where the engine lays each '
          'height, 64 and 16 per plot), its full height in grid units')
    check(ps.get('combine') == 1, 'stamps on neighbouring plots combine by max')
    check('PLOTSTAMPS setfor=1' in pout, "a plot's stamp goes by its feature first, then by its terrain")
    mf = re.search(r'^PLOTFLATTEN (.*)$', pout, re.M)
    pf = {k: int(v) for k, v in (kv.split('=') for kv in mf.group(1).split())} if mf else {}
    check(pf.get('core') == 1, f'Flatten: the level is the mean height over the weights of 1, a grid level {pf}')
    check(pf.get('weight') == 1 and pf.get('seat') == 1 and pf.get('outside') == 1,
          'Flatten: each sample is drawn to the level by its weight: flat over the seat, nothing past the square')
    check(pf.get('repeat') == 1, 'Flatten: a square rebuilt by the engine comes out the same, with nothing accumulating')
    check(pf.get('two') == 1, "Flatten: two corners' seats in one square each keep their own level")
    check(pf.get('fallback') == 1 and pf.get('none') == 1,
          'Flatten: with no weight of 1, the mean of all it touches; with no weight at all, nothing changes')
    check(pf.get('edge') == 1, "Flatten: weight on a square's edge is found, on all four sides, and a seat inside isn't")
    check(pf.get('wrap') == 1, "Flatten: the peak stamp's four calls are wrapped round the stamp or another patch's "
          "wrapper, and not if they aren't one target's calls")
    mc = re.search(r'^PLOTSTAMPS colourfiles=(\d) colourrefused=(\d) atlas=(\d) cells=(\d) (.*)$', pout, re.M)
    check(mc and mc.group(1) == '1' and mc.group(2) == '1',
          f"a stamp's painting read: DXT5 or A8R8G8B8, square, a power of two, mips to 4 texels; others refused "
          f"{mc.group(5) if mc else ''}")
    check(mc and mc.group(3) == '1' and mc.group(4) == '1',
          'the paintings laid out in an atlas of whole cells, each level copied to its cell')
    check(ps.get('clip') == 1, "added onto the engine's heights (with a jigsaw piece's under it), clamped at 255; "
          'the samples clipped there counted (the headroom the stamps lack)')

    print('area borders, draping:')
    dout = run([os.path.join(TEST, 'test_proxy.exe'), 'drape'])
    m = re.search(r'^DRAPE (.*?)(?: \(.*)?$', dout, re.M)
    dr = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}
    dline = m.group(0) if m else dout.strip()[-300:]
    check(dr.get('layout') == 1 and dr.get('refuses') == 1,
          f"vertex layout from the FVF: normal, colour and uv found; blended or mis-sized vertices refused ({dline})")
    check(dr.get('heights') == 1, 'the height lookup: bilinear, unknown outside and next to unknown cells that count')
    check(dr.get('count') == 1, f"the guide's tiles cut into pieces no wider than a grid cell, the unused zero tile left "
                                f"out ({dline})")
    check(dr.get('ground') == 1 and dr.get('attrs') == 1,
          "every new vertex on the ground plus the lift; uv, colour and normal interpolated as BtS's")
    check(dr.get('pull') == 1 and dr.get('ortho') == 1,
          'pulled 24 units towards the camera without moving on the screen (perspective and orthographic)')
    check(dr.get('unknown') == 1 and dr.get('toomany') == 1,
          'refused over unknown ground, and when 16-bit indices would run out')
    check(dr.get('edge') == 1, "a guide whose corners are on grid cells, heights over exactly its cells: the vertices cut "
                               "onto its edges still find ground (Dune Wars trace 2026-11-19: drawn flat every frame)")

    print('terrain grid, partial updates:')
    cout = run([os.path.join(TEST, 'test_proxy.exe'), 'terraincells'])
    m = re.search(r'^TERRAINCELLS (.*)$', cout, re.M)
    tc = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())} if m else {}
    check(tc.get('pieces') == 1, f'changed rectangles, split at the grid\'s wrap: every texel once, in place {tc}')
    check(tc.get('shore') == 1 and tc.get('steps') == 16 and tc.get('seam', 0) > 0,
          'the shore worked out around each new chunk of terrain (16, some across the seam) = everything worked out again')
    check(tc.get('shoreall') == 1, 'the water height changing: all of the shore worked out again, the same')

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
    frame_png = os.path.join(rtraces, os.listdir(rtraces)[0], 'frame.png')
    check(os.path.exists(frame_png) and open(frame_png, 'rb').read(8) == b'\x89PNG\r\n\x1a\n',
          "frame.png saved with the trace (the trace's first frame as drawn)")
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
                   old=False, game_dir=None, flag_stage=0, mod_files=None, flag_dds=None, ramp=False, sun='-1,0,-1',
                   sheet='0', water_height=0):
        """Harness 'shadow' scene with shadows on or off; returns its SHADOW values and session log. sun: [shadows]
        SunDirection; water_height: [reflection] WaterHeight. old: the
        terrain and unit shaders named as in older BtS builds; game_dir: BtS's tree and river shaders from there;
        flag_stage: the texture stage the flag texture is on (flag_dds: another texture there); mod_files: {path in a
        mod folder: bytes}, run with mod= that folder; ramp: the detail texture brightens along u (256 texels); sheet '1':
        the base texture is a 16x16 blend sheet, tile rows 0-1 grey 0x20, 2-3 grey 0x60 (the boundary at world y = 0);
        'L', 'H': all 0x20, all 0x60."""
        sdir = os.path.join(TEST, f'shadow_{overlay}_{label}')
        # extra: more INI text (e.g. a [lighting] section)
        sout_dir = os.path.join(sdir, 'out')
        if os.path.isdir(sdir):
            shutil.rmtree(sdir)
        os.makedirs(sdir)
        shutil.copyfile(DLL, os.path.join(sdir, 'd3d9.dll'))
        # The water's textures, as the release package has them, for runs that turn the water on (the river glint).
        os.makedirs(os.path.join(sdir, 'GraphicsUpgrade'))
        for fn in ('water_normal.dds', 'water_env.dds'):
            shutil.copyfile(os.path.join(ROOT, 'textures', fn), os.path.join(sdir, 'GraphicsUpgrade', fn))
        with TestIni(os.path.join(sdir, 'GraphicsUpgrade.ini')) as f:
            f.write(f'[general]\nOutputDir={sout_dir}\nGames=*\n[reflection]\nWaterHeight={water_height}\n'
                    f'[shadows]\nEnabled={enabled}\nSunDirection={sun}\nDarkness=0.5\nOverlay=1\nBuildingShadows={buildings}\n'
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
        env = dict(os.environ, GU_TEST_OLD_SHADERS='1' if old else '0', GU_TEST_DETAIL_RAMP='1' if ramp else '0',
                   GU_TEST_SHEET=sheet)
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
    night = '[lighting groups]\nTerrainSun=60,70,90\nTerrainAmbient=30,35,50\n[lighting]\nEnabled=1\n'
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

    # Unlit effects (particles: a sandstorm's glow) a mod lists in [lighting] LitEffectTextures (or "*" for all) take
    # the same change of light: the additive effect's blue 64 over the night ground becomes 64 x 0.394. Multiplying
    # effects (a sandstorm's shade) scale the lit ground already; unlisted effects and the interface (an orthographic
    # camera) are left alone.
    def write_dds(path, bgra):
        """A 1x1 uncompressed 32-bit .dds of one pixel (B, G, R, A)."""
        hdr = bytearray(128)
        hdr[0:4] = b'DDS '
        struct.pack_into('<7I', hdr, 4, 124, 0x100F, 1, 1, 4, 0, 1)  # size, flags, height, width, pitch, depth, mips
        struct.pack_into('<8I', hdr, 76, 32, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)  # RGB + alpha, 32 bits
        struct.pack_into('<I', hdr, 108, 0x1000)
        with open(path, 'wb') as f:
            f.write(bytes(hdr) + bytes(bgra))

    effect_dds, other_dds = os.path.join(TEST, 'effect_blue64.dds'), os.path.join(TEST, 'effect_other.dds')
    write_dds(effect_dds, (0x40, 0, 0, 0xFF))  # the harness effect's texture
    write_dds(other_dds, (0x40, 0x40, 0, 0xFF))
    ground = shadow_run('night', 0, 'none', night)[0].get('lit', 0)
    lit_effect = ground + 64 * scale
    for overlay, label, ini, expect, name in (
            ('effect', 'all', 'LitEffectTextures=*\n', lit_effect, 'additive effect at night, every effect listed (*)'),
            ('effect', 'listed', f'LitEffectTextures={other_dds}; {effect_dds}\n', lit_effect,
             'additive effect at night, its texture listed'),
            ('effectlit', 'all', 'LitEffectTextures=*\n', lit_effect,
             'additive effect lit with an emissive-only material, at night'),
            ('effect', 'unlisted', f'LitEffectTextures={other_dds}\n', ground + 64,
             'additive effect at night, its texture not listed: unchanged'),
            ('effect', 'none', '', ground + 64, 'additive effect at night, nothing listed (the default): unchanged'),
            ('effectmul', 'all', 'LitEffectTextures=*\n', ground * 191 / 255,
             'multiplying effect (a sandstorm\'s shade) at night: unchanged'),
            ('effectui', 'all', 'LitEffectTextures=*\n', ground + 64,
             'additive draw through an orthographic camera (the interface) at night: unchanged')):
        v, elog = shadow_run(label, 0, overlay, night + ini)
        check(abs(v.get('lit', 0) - expect) <= 4 and v.get('restored') == 1 and v['released'],
              f"{name} ({v.get('lit')}, expected {expect:.0f} over the ground's {ground}); game state restored")
        if label == 'listed':
            check('LitEffectTextures: ' + effect_dds + ': 1 texture' in elog and
                  'first effect taking the light (blend 5/2, lighting 0' in elog,
                  'the listed effect texture found and logged; the first effect taking the light logged')

    # The settler's city-site guide: BtS draws it flat (here at z = -300) with the depth test off. With its texture in
    # [textures] AreaBorderTextures it's laid over the terrain's heights (the slope z = 0.2x, read from the terrain's
    # buffers) and depth tested: seen from 1000 up it's wider (64 pixels, not 49), and the occluder at z = 100 over
    # x < 0 hides that half. Unlisted, it's as BtS draws it, over the occluder.
    guide_dds = os.path.join(TEST, 'guide_white.dds')
    write_dds(guide_dds, (0xFF, 0xFF, 0xFF, 0xFF))
    g_off, glog_off = shadow_run('unlisted', 0, 'guide', '')
    g_on, glog_on = shadow_run('listed', 0, 'guide', f'[textures]\nAreaBorderTextures={guide_dds}\n')
    check(22 <= g_off.get('guidel', 0) <= 27 and 22 <= g_off.get('guider', 0) <= 27 and g_off.get('guidestate') == 1 and
          'area borders:' not in glog_off and g_off['released'],
          f"guide not listed: drawn as BtS draws it, flat and over everything ({g_off.get('guidel')} + "
          f"{g_off.get('guider')} pixels, expected about 24 + 25)")
    check(g_on.get('guidel') == 0 and 31 <= g_on.get('guider', 0) <= 35 and g_on.get('guidestate') == 1 and
          g_on['released'],
          f"guide listed: laid over the slope ({g_on.get('guider')} pixels right of centre, expected about 33) and depth "
          f"tested (the occluded half: {g_on.get('guidel')} pixels, expected 0); the game's buffers, depth test and "
          f"WORLD back after it")
    check('AreaBorderTextures: ' + guide_dds + ': 1 texture' in glog_on and
          'area borders: draped over the terrain (2 triangles cut into 8, ground -20.0 to 20.0), 24 units nearer the '
          'camera' in glog_on,
          "the guide's texture found and logged; its draping logged (the zero tile left out, the slope's heights)")

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

    print('waves:')
    # Our copy of BtS's wave shader (Civ4Wave.fx) against the game's own, and the opacity a wave model can carry in
    # its base layer's offset (10 + opacity).
    if not os.path.exists(os.path.join(GAME_SHADERS, 'wave_ps.bin')):
        print(f'  skip  wave shader comparison: BtS\'s Civ4Wave.fx not found under {BTS}')
    else:
        wdir2 = os.path.join(TEST, 'wavecmp')
        if os.path.isdir(wdir2):
            shutil.rmtree(wdir2)
        os.makedirs(wdir2)
        shutil.copyfile(DLL, os.path.join(wdir2, 'd3d9.dll'))
        with TestIni(os.path.join(wdir2, 'GraphicsUpgrade.ini')) as f:
            f.write(f'[general]\nOutputDir={os.path.join(wdir2, "out")}\nGames=none\n')
        wout2 = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(wdir2, 'd3d9.dll'), 'wavecmp', GAME_SHADERS])
        mw = re.search(r'WAVECMP same=(\d+) half=(\d+) zero=(\d+) lit=(\d+) tinted=(\d+)', wout2)
        check(mw and int(mw.group(1)) <= 1 and int(mw.group(4)) > 10000,
              f"our wave shader draws what BtS's does when a model carries no opacity (largest difference "
              f"{mw.group(1) if mw else '?'} of 255, {mw.group(4) if mw else '?'} channels lit)")
        check(mw and int(mw.group(5)) <= 2,
              f"the foam in the light now: scaled by the change of light (red 1, green 0.5, blue 0.25: worst "
              f"{mw.group(5) if mw else '?'} off)")
        check(mw and int(mw.group(2)) <= 3 and int(mw.group(3)) == 0,
              f"a wave model's opacity: base offset 10.5 draws the foam at half opacity (worst {mw.group(2) if mw else '?'}"
              f" off), 10 not at all (brightest {mw.group(3) if mw else '?'})")
        md = re.search(r'WAVEDRAPE flat=(\d+) moved=(\d+)', wout2)
        if 'WAVEDRAPE skipped' in wout2:
            print('  skip  draped waves: no vertex textures here')
        else:
            check(md and int(md.group(1)) <= 1 and int(md.group(2)) > 1000,
                  f"draped waves: where the ground is unknown or below them they're drawn as flat waves (worst "
                  f"{md.group(1) if md else '?'}); ground above them lifts them onto it ({md.group(2) if md else '?'} "
                  f"channels moved)")
        check('final refs device 0, d3d 0' in wout2, 'wave comparison: device released')

    print('culture borders:')
    # With [debug] TerrainVertsPerPlot 33/65 the borders are drawn with our copy of BtS's border shader (border.hlsl):
    # lifted onto the terrain's heights and nearer the camera in depth, so finer terrain doesn't rise through them.
    if not os.path.exists(os.path.join(GAME_SHADERS, 'border1_ps.bin')):
        print(f"  skip  culture borders: BtS's CultureBOrderShader.fx not found under {BTS}")
    else:
        def border_run(label, ini):
            bdir = os.path.join(TEST, label)
            if os.path.isdir(bdir):
                shutil.rmtree(bdir)
            os.makedirs(bdir)
            shutil.copyfile(DLL, os.path.join(bdir, 'd3d9.dll'))
            with TestIni(os.path.join(bdir, 'GraphicsUpgrade.ini')) as f:
                f.write(f'[general]\nOutputDir={os.path.join(bdir, "out")}\n{ini}')
            out = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(bdir, 'd3d9.dll'), 'border', GAME_SHADERS])
            plain = {int(p): (int(s), int(n)) for p, s, n in re.findall(r'BORDERPLAIN p=(\d) sum=(\d+) lit=(\d+)', out)}
            ridge = {int(p): int(n) for p, n in re.findall(r'BORDERRIDGE p=(\d) visible=(\d+)', out)}
            log_path = os.path.join(bdir, 'out', 'test_proxy', 'session.log')
            log = open(log_path, encoding='latin-1').read() if os.path.exists(log_path) else ''
            return out, plain, ridge, log

        bout, _, _, _ = border_run('bordercmp', 'Games=none\n')
        if 'BORDERCMP skipped' in bout:
            print('  skip  our border shader: no vertex textures here')
        else:
            mc = re.search(r'BORDERCMP same0=(\d+) same1=(\d+)', bout)
            check(mc and int(mc.group(1)) <= 1 and int(mc.group(2)) <= 1,
                  f"our border shader draws what BtS's does, both passes, nothing lifted or moved (worst "
                  f"{mc.group(1) if mc else '?'} and {mc.group(2) if mc else '?'} of 255)")
            ml = re.search(r'BORDERLIFT flat=(\d+) moved=(\d+)', bout)
            check(ml and int(ml.group(1)) <= 1 and int(ml.group(2)) > 1000,
                  f"our border shader: where the ground is unknown it's drawn as BtS draws it (worst "
                  f"{ml.group(1) if ml else '?'}); ground 100 above it lifts it onto it "
                  f"({ml.group(2) if ml else '?'} pixels changed)")
            mw = re.search(r'BORDERWRAP west=(\d+) east=(\d+)', bout)
            check(mw and int(mw.group(1)) <= 1 and int(mw.group(2)) <= 1,
                  f"our border shader wraps the height grid itself: ground known only at negative cells lifts the "
                  f"border's west half and not its east, even on a card that clamps vertex texture reads (worst "
                  f"{mw.group(1) if mw else '?'} and {mw.group(2) if mw else '?'} of 255)")
        check('final refs device 0, d3d 0' in bout, 'border comparison: device released')
        textures = '[textures]\nAnisotropy=1\n'
        sout, splain, sridge, slog = border_run('borderstock', f'Games=*\n{textures}')
        fout, fplain, fridge, flog = border_run('borderfine', f'Games=*\n{textures}[debug]\nTerrainVertsPerPlot=65\n')
        check(len(sridge) == 2 and all(v == 0 for v in sridge.values()) and 'borders:' not in slog,
              f"stock terrain: the borders drawn as BtS draws them, a ridge 10 units in front hides them "
              f"({sridge.get(0)}, {sridge.get(1)} channels lit)")
        check(len(fridge) == 2 and all(v > 10000 for v in fridge.values()) and
              'borders: culture borders drawn with our border shader, 24 units nearer the camera; not lifted' in flog,
              f"TerrainVertsPerPlot=65: the borders drawn with ours, nearer the camera in depth, so a ridge 10 units "
              f"in front no longer hides them ({fridge.get(0)}, {fridge.get(1)} channels lit)")
        check(len(fplain) == 2 and all(fplain[p][1] > 10000 and abs(fplain[p][0] - splain[p][0]) <= splain[p][0] * 0.0005
                                       for p in (0, 1)),
              f"TerrainVertsPerPlot=65: with nothing in front, the borders look as BtS draws them (sums "
              f"{[fplain.get(p, (0,))[0] for p in (0, 1)]} vs {[splain.get(p, (0,))[0] for p in (0, 1)]})")
        check('final refs device 0, d3d 0' in sout and 'final refs device 0, d3d 0' in fout,
              'culture borders: device released, stock and finer terrain')

    print('lighting:')

    def light_run(label, ini, ff=False, mod_ini=None, rt=False, files=None, blob=False, parts=None):
        """mod_ini: a mod's GraphicsUpgrade.ini (a GraphicsUpgrade folder in Mods\\TestMod, run with mod=...); rt: drawn
        into a render-target texture, like the interface's 3D views ('screen': one the size of the screen, like the map
        behind the Civilopedia); files: {path under the run's folder (the game folder): bytes}; blob: a unit's blob
        shadow (grey 128) just before the model; parts: 'weapon', 'land' or 'same' (LightScene, test_proxy.cpp)."""
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
        for rel, content in (files or {}).items():
            os.makedirs(os.path.dirname(os.path.join(ldir, rel)), exist_ok=True)
            with open(os.path.join(ldir, rel), 'wb') as f:
                f.write(content)
        res = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(ldir, 'd3d9.dll'), 'light'] +
                  (['ff'] + (['tex'] if ff == 'tex' else []) if ff else []) + (['blob'] if blob else []) +
                  ([parts] if parts else []) + ([rt if isinstance(rt, str) else 'rt'] if rt else []) + extra)
        m = re.search(r'LIGHT pixel=([0-9a-f]{8}) restored=(\d)', res)
        rgb = tuple(int(m.group(1)[i:i + 2], 16) for i in (2, 4, 6)) if m else None
        return rgb, bool(m) and m.group(2) == '1' and 'final refs device 0, d3d 0' in res

    def near(a, b, tol=2):
        return a is not None and all(abs(x - y) <= tol for x, y in zip(a, b))

    lit = '[lighting groups]\nUnitSun=200,100,50\nUnitAmbient=20,40,60\n[lighting]\nEnabled=1\n'
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
    # With the cycle off, a sun path (SunMiddayFrom) fixes the sun where the path has it at LightCycleStartStep, and
    # [shadows] SunDirection (straight down here) goes unused: midday from the south, 30 degrees up, travels
    # (0, 0.866, -0.5), so the upward-facing unit gets sin 30 = 0.5 of the sun 200,100,50, plus ambient 20,40,60.
    rgb, ok = light_run('fixed_sun', lit + 'LightCycleOn=0\nSunMiddayFrom=S\nSunMiddayHeight=30\nSunriseHeight=10\n'
                                            'LightCycleStartStep=12\n[shadows]\nSunDirection=0,0,-1\n')
    flog = open(os.path.join(TEST, 'light_fixed_sun', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    m = re.search(r'sun fixed on the sun path at step 12 of 24 .*direction ([-\d.]+),([-\d.]+),([-\d.]+)', flog)
    fixed = [float(g) for g in m.groups()] if m else []
    check(near(rgb, (120, 90, 85)) and ok and len(fixed) == 3 and
          all(abs(a - b) < 0.002 for a, b in zip(fixed, (0, 0.866, -0.5))),
          f'cycle off: the sun stands on the sun path at LightCycleStartStep (south, 30 degrees up), not at '
          f'SunDirection -> {rgb}, expected (120, 90, 85)')
    # LightCycleImage: a column per step, the top row the sun, the second the ambient. A 24x2 PNG in the game folder's
    # GraphicsUpgrade folder: at midday (column 12) sun 200,0,0 and ambient 0,0,60, so the unit is 200,0,60; the other
    # columns are green with no ambient.
    def png(width, rows):
        raw = b''.join(b'\0' + bytes(c for px in row for c in px) for row in rows)
        chunk = lambda kind, data: (struct.pack('>I', len(data)) + kind + data +
                                    struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF))
        return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, len(rows), 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

    def bmp(width, rows):
        """24-bit, bottom row first, rows padded to 4 bytes."""
        pad = (4 - width * 3 % 4) % 4
        data = b''.join(bytes(c for px in row for c in reversed(px)) + b'\0' * pad for row in reversed(rows))
        head = struct.pack('<2sIHHI', b'BM', 54 + len(data), 0, 0, 54)
        return head + struct.pack('<IiiHHIIiiII', 40, width, len(rows), 1, 24, 0, len(data), 2835, 2835, 0, 0) + data

    sun_row = [(200, 0, 0) if x == 12 else (0, 255, 0) for x in range(24)]
    amb_row = [(0, 0, 60) if x == 12 else (0, 0, 0) for x in range(24)]
    img_ini = ('[lighting]\nEnabled=1\nAmbientColour=90,90,90\nMatchShadowDirection=0\nLightCycleOn=1\n'
               'LightChangeCycleTime=100000\nLightChangeStepCount=24\nLightCycleImage=cycle.png\n'
               'LightCycleSteps=12,255,255,255\n')
    rgb, ok = light_run('cycle_image', img_ini, files={'GraphicsUpgrade/cycle.png': png(24, [sun_row, amb_row])})
    ilog = open(os.path.join(TEST, 'light_cycle_image', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(near(rgb, (200, 0, 60)) and ok and '24x2: 24 steps, sun from the top row, ambient from the second' in ilog and
          'light cycle, 24 sun, 24 ambient' in ilog,
          f'LightCycleImage (a 24x2 PNG): at midday column 12, sun 200,0,0 + ambient 0,0,60 -> {rgb}; it replaces '
          f'LightCycleSteps and AmbientColour')
    # A mod's own, in its GraphicsUpgrade folder: a 12x1 BMP makes 12 steps (whatever LightChangeStepCount says), and
    # with no second row the ambient keeps AmbientColour. Step 6 is blue.
    row12 = [(0, 0, 255) if x == 6 else (255, 0, 0) for x in range(12)]
    rgb, ok = light_run('cycle_image_mod', img_ini.replace('LightCycleImage=cycle.png\n', '') + 'LightCycleStartStep=6\n',
                        mod_ini='[lighting]\nLightCycleImage=cycle.bmp\n',
                        files={'Mods/TestMod/GraphicsUpgrade/cycle.bmp': bmp(12, [row12])})
    mlog = open(os.path.join(TEST, 'light_cycle_image_mod', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(near(rgb, (90, 90, 255)) and ok and '12x1: 12 steps' in mlog and
          "no second row: the ambient doesn't cycle" in mlog and 'over 12 steps' in mlog,
          f"a mod's LightCycleImage (a 12x1 BMP in its GraphicsUpgrade folder): 12 steps, step 6 blue, ambient "
          f"AmbientColour 90,90,90 -> {rgb}, expected (90, 90, 255)")
    # Not found: a warning, and the lists as before.
    rgb, ok = light_run('cycle_image_missing', img_ini.replace('cycle.png', 'nothere.png'))
    xlog = open(os.path.join(TEST, 'light_cycle_image_missing', 'out', 'test_proxy', 'session.log'),
                encoding='latin-1').read()
    check(near(rgb, (255, 255, 255)) and ok and 'LightCycleImage=nothere.png not found' in xlog,
          f'LightCycleImage not found: warned, and LightCycleSteps used instead -> {rgb}')
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
    rgb, ok = light_run('cycle_own', cycle + '[lighting groups]\nUnitSun=10,20,30\n')
    check(near(rgb, (10, 20, 30)) and ok, f'a group with its own *Sun key keeps it through the cycle -> {rgb}')
    # The ambient cycles the same way (no sun: the pixel is the ambient), unless the group has its own *Ambient key.
    amb_cycle = ('[lighting]\nEnabled=1\nSunColour=0,0,0\nMatchShadowDirection=0\nLightCycleOn=1\n'
                 'LightChangeCycleTime=100000\nLightChangeStepCount=24\nLightCycleStartStep=0\nLightCycleAmbientSteps=6,0,100,0;18,0,0,100\n')
    rgb, ok = light_run('cycle_amb', amb_cycle)
    check(near(rgb, (0, 50, 50)) and ok, f'ambient colour cycle: halfway from step 18 round to step 6 -> {rgb}, '
          'expected (0, 50, 50)')
    rgb, ok = light_run('cycle_amb_own', amb_cycle + '[lighting groups]\nUnitAmbient=10,20,30\n')
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
    rgb_mod, ok_mod = light_run('mod', base, mod_ini='[lighting groups]\nUnitSun=100,100,100\n')
    mlog = open(os.path.join(TEST, 'light_mod', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    rgb_nomod, _ = light_run('mod_none', base, mod_ini='')
    nlog = open(os.path.join(TEST, 'light_mod_none', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(near(rgb_mod, (120, 140, 160)) and ok_mod and 'overrides the game folder' in mlog and
          near(rgb_nomod, (220, 140, 110)) and 'no GraphicsUpgrade.ini' in nlog,
          f"a mod's GraphicsUpgrade.ini overrides setting by setting: its sun 100,100,100 with the game folder's ambient "
          f"-> {rgb_mod} (expected (120, 140, 160)); a mod with only a GraphicsUpgrade folder keeps the game folder's "
          f"-> {rgb_nomod}")

    # 1.0's INIs had the group colours in [lighting]: read there when [lighting groups] leaves them empty, with a
    # warning; a mod's old key still overrides the game folder's new one.
    old_lit = '[lighting]\nEnabled=1\nMatchShadowDirection=0\nUnitSun=200,100,50\nUnitAmbient=20,40,60\n'
    rgb_old, ok_old = light_run('old_groups', '[lighting groups]\nUnitSun=\n' + old_lit)
    olog = open(os.path.join(TEST, 'light_old_groups', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    rgb_new, _ = light_run('old_groups_new', '[lighting groups]\nUnitSun=100,100,100\n' + old_lit)
    onlog = open(os.path.join(TEST, 'light_old_groups_new', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    rgb_oldmod, _ = light_run('old_groups_mod', base, mod_ini='[lighting]\nUnitSun=100,100,100\n')
    omlog = open(os.path.join(TEST, 'light_old_groups_mod', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    moved = 'settings: [lighting] %s has moved; set it in [lighting groups]'
    check(near(rgb_old, (220, 140, 110)) and ok_old and moved % 'UnitSun' in olog and moved % 'UnitAmbient' in olog and
          near(rgb_new, (120, 140, 160)) and moved % 'UnitSun' not in onlog and moved % 'UnitAmbient' in onlog and
          near(rgb_oldmod, (120, 140, 160)) and moved % 'UnitSun' in omlog,
          f"group colours in [lighting] (1.0's place) still read, with a warning: {rgb_old}, expected (220, 140, 110); "
          f"[lighting groups] wins when set: {rgb_new}; a mod's old key overrides the game folder's new one: "
          f"{rgb_oldmod}, expected (120, 140, 160)")

    # Hotkeys written as keys: letters (any case), F-keys, numpad, names, hex codes, empty = none, junk = none + warning.
    keys_ini = ('[water]\nToggleKey=Numpad5\nNormalMapToggleKey=F9\n[shadows]\nToggleKey=F11\n[lighting]\nToggleKey=\nLightCycleToggleKey=f12\n'
                '[reflection]\nToggleKey=0x4C\nOverlayToggleKey=PrintScreen\n[ghost]\nToggleKey=bogus\n'
                '[textures]\nToggleKey=Home\nDetailNormalsToggleKey=F7\n[debug]\nTraces=1\n[trace]\nKey=t\n')
    light_run('keys', keys_ini)
    klog = open(os.path.join(TEST, 'light_keys', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    expect_keys = ('water Numpad5, water normal map F9, shadows F11, lighting none, light cycle F12, filtering Home, '
                   'detail normals F7, reflection L, reflection overlay PrintScreen, shadow overlay none, ghost none, '
                   'trace T')
    check(expect_keys in klog and "[ghost] ToggleKey=bogus isn't a key" in klog,
          f'hotkeys written as keys (t, F11, f12, F7, F9, Numpad5, Home, PrintScreen, 0x4C, empty, a bogus name) read as '
          f'{expect_keys}')
    light_run('defkeys', '')
    dlog = open(os.path.join(TEST, 'light_defkeys', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check('water 1, water normal map 7, ' in dlog and 'filtering 5, detail normals 6, ' in dlog,
          'by default Ctrl+Shift+7 cycles the water normal maps and Ctrl+Shift+6 switches the detail normals')
    # Anisotropic filtering: on by default at the INI's 8 (or the card's most, if less); Anisotropy=1 turns it off.
    alog = open(os.path.join(TEST, 'light_keys', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    light_run('noaniso', '[textures]\nAnisotropy=1\n')
    olog = open(os.path.join(TEST, 'light_noaniso', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check(re.search(r'anisotropic filtering: x[2-8] \(Anisotropy=8', alog) is not None and
          'anisotropic filtering: off (Anisotropy=1' in olog,
          'anisotropic filtering: on at Anisotropy=8 (capped by the card), off at Anisotropy=1')

    # Buildings: fixed-function lights (a directional light and D3DRS_AMBIENT) take the Building colours.
    for label, ini, expect, what in (
            ('ff_off', '', (144, 160, 176), "fixed-function (building) lit by the game's light and ambient"),
            ('ff_on', '[lighting groups]\nBuildingSun=200,100,50\nBuildingAmbient=20,40,60\n[lighting]\nEnabled=1\nMatchShadowDirection=0\n',
             (236, 172, 158), 'fixed-function lights take the INI Building colours, the way BtS uses them: sun as the '
             "light's diffuse (200,100,50), ambient as the light's own ambient (20,40,60) on top of the global "
             'ambient (16,32,48)'),
            ('ff_unit', '[lighting groups]\nUnitSun=200,100,50\nUnitAmbient=20,40,60\n[lighting]\nEnabled=1\n', (144, 160, 176),
             'a building is left alone when only the Unit colours are set (unset groups keep the game\'s light)')):
        rgb, ok = light_run(label, ini, ff=True)
        check(near(rgb, expect) and ok, f'{what} -> {rgb}, expected {expect}; light and ambient restored')

    # BuildingLightRange: a textured building (grey 64, emissive 0.1) under a light adding up past 1 (emissive 0.1 +
    # global ambient 16,32,48 + ambient 150,100,50 + sun 200: 1.54, 1.40, 1.27) is lit to its texture's colour at
    # range 1, as by BtS; at 2 to 64 x the light, as the ground is; at 1.5 up to 1.5 x 64 (red).
    hot = ('[lighting groups]\nBuildingSun=200,200,200\nBuildingAmbient=150,100,50\n[lighting]\nEnabled=1\n'
           'MatchShadowDirection=0\n')
    for label, ini, expect, what in (
            ('ff_range1', hot, (64, 64, 64), "BuildingLightRange 1 (the default): Direct3D's lighting stops at 1, "
             "the texture's own colour"),
            ('ff_range2', hot + 'BuildingLightRange=2\n', (98, 90, 81), 'BuildingLightRange 2: lights, emissive and '
             'global ambient at half strength, the draw doubled after its own texture stages'),
            ('ff_range15', hot + 'BuildingLightRange=1.5\n', (96, 90, 81), 'BuildingLightRange 1.5: up to 1.5 times '
             'the texture (red capped)'),
            ('ff_range_off', hot.replace('Enabled=1', 'Enabled=0') + 'BuildingLightRange=2\n', (42, 46, 50),
             "BuildingLightRange with the lighting off: as BtS, under the game's own light (sun 0.5, ambient "
             '16,32,48, emissive 0.1)')):
        rgb, ok = light_run(label, ini, ff='tex')
        check(near(rgb, expect) and ok, f'{what} -> {rgb}, expected {expect}; light, ambient, material, texture '
              f'factor and stages restored')
    # GroundLitTextures: everything at half strength as at range 2 (0.77, 0.70, 0.635), added to the grey texture (64) less
    # 0.5 rather than doubled and multiplied by it.
    grey_dds = (struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x100F, 1, 1, 4, 0, 1) +
                struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000) +
                struct.pack('<IIIII', 0x1000, 0, 0, 0, 0) + struct.pack('<I', 0xFF404040))
    listed = {'Assets/Art/rock.dds': grey_dds}
    rgb, ok = light_run('ff_ground', hot + 'GroundLitTextures=Art\\rock.dds\n', ff='tex', files=listed)
    check(near(rgb, (133, 115, 98)) and ok, f'GroundLitTextures: the light added to the texture, as on the ground -> {rgb}, '
          f'expected (133, 115, 98); light, ambient, material and stages restored')
    rgb, ok = light_run('ff_ground_other', hot + 'BuildingLightRange=2\nGroundLitTextures=Art\\other.dds\n', ff='tex',
                        files={'Assets/Art/other.dds': grey_dds[:-4] + struct.pack('<I', 0xFF505050)})
    check(near(rgb, (98, 90, 81)) and ok, f'a model with another texture is lit as before -> {rgb}, expected (98, 90, 81)')
    # Just after a unit's blob, a model counts as that unit's part; listed, it keeps the Building light and the sum.
    unit_blob = grey_dds[:-4] + struct.pack('<I', 0xFF808080)
    rgb, ok = light_run('ff_ground_unit', hot + 'GroundLitTextures=Art\\rock.dds\n[shadows]\nEnabled=1\n'
                        'BlobShadowTexture=Art\\blob.dds\n', ff='tex', blob=True,
                        files={**listed, 'Assets/Art/blob.dds': unit_blob})
    check(near(rgb, (133, 115, 98)) and ok, f'GroundLitTextures: a model beside a unit is lit the same -> {rgb}, '
          f'expected (133, 115, 98); light, ambient, material and stages restored')
    # At a blob: a weapon under a light of its own keeps the Unit light (the game's: sun 0.4, emissive 0.1, ambient
    # 16,32,48; no Unit colours set); a mesh under the land's light keeps the Building group's range (98,90,81); with
    # units under the land's own light the colours can't tell them apart, so a mesh there stays the unit's (sun 0.5).
    at_blob = hot + 'BuildingLightRange=2\n[shadows]\nEnabled=1\nBlobShadowTexture=Art\\blob.dds\n'
    for label, parts, expect, what in (
            ('ff_part_weapon', 'weapon', (36, 40, 44), "a weapon under the unit's light keeps the Unit light"),
            ('ff_part_land', 'land', (98, 90, 81), 'a land model next to a blob keeps the Building light'),
            ('ff_part_same', 'same', (42, 46, 50), "units sharing the land's light: a mesh at a blob stays the unit's")):
        rgb, ok = light_run(label, at_blob, ff='tex', blob=True, parts=parts, files={'Assets/Art/blob.dds': unit_blob})
        check(near(rgb, expect) and ok, f'{what} -> {rgb}, expected {expect}; light, ambient, material and stages '
              f'restored')
    light_run('ff_range_bad', hot + 'BuildingLightRange=3\n', ff='tex')
    rlog = open(os.path.join(TEST, 'light_ff_range_bad', 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
    check("BuildingLightRange=3 isn't 1..2; using 2" in rlog, 'BuildingLightRange outside 1..2 warned about and capped')

    # The interface's 3D views (the unit in the lower-left corner, the Civilopedia's models) are drawn into textures of
    # their own: they keep the game's light, whatever the INI and the time of day on the map.
    building = '[lighting groups]\nBuildingSun=200,100,50\nBuildingAmbient=20,40,60\n[lighting]\nEnabled=1\nMatchShadowDirection=0\n'
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
                         ('lighting-warm.ini', 'unit sun 255,235,188, ambient 89,103,133')):
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
    terrain_light = '[lighting groups]\nTerrainSun=200,100,50\nTerrainAmbient=20,40,60\n[lighting]\nEnabled=1\n'
    river_light = '[lighting groups]\nRiverSun=200,100,50\nRiverAmbient=20,40,60\n[lighting]\nEnabled=1\n'
    tl, _ = shadow_run('lit', 0, 'none', terrain_light)
    check(abs(tl.get('lit', 0) - expect_b) <= 3 and tl.get('restored') == 1 and tl['released'],
          f'terrain lightmap re-lit with the INI colours (blue {tl.get("lit")}, expected {expect_b}, '
          f'BtS {shadow["off"].get("lit")})')
    # Terrain colours equal to the baked ones (the BtS preset): the lightmap exactly as BtS baked it.
    tb, _ = shadow_run('baked', 0, 'none', '[lighting groups]\nTerrainSun=246,239,222\nTerrainAmbient=89,103,133\n[lighting]\nEnabled=1\n')
    check(tb.get('lit') == shadow['off'].get('lit') and tb.get('restored') == 1 and tb['released'],
          f'terrain colours equal to the baked ones leave the lightmap exactly as BtS baked it '
          f'({tb.get("lit")} = {shadow["off"].get("lit")})')
    # Trees: BtS's own (unlit) tree shaders; with Tree colours our lit copy of the vertex shader takes over, lit from
    # the shadows' sun (-1,0,-1 here: 0.707 of full sun on the upward-facing patch).
    have_trees = os.path.exists(os.path.join(river_dir, 'tree_vs.bin'))
    if have_trees:
        t_off, _ = shadow_run('lit', 0, 'tree')
        t_on, _ = shadow_run('lit', 0, 'tree', '[lighting groups]\nTreeSun=200,100,50\nTreeAmbient=20,40,60\n[lighting]\nEnabled=1\n')
        unlit = round(255 * (0x80 / 255) * (0xE6 / 255))
        lit_b = round(255 * (0x80 / 255) * (0xE6 / 255) * (50 / 255 * 0.70711 + 60 / 255))
        check(abs(t_off.get('lit', 0) - unlit) <= 2, f'trees unlit as in BtS ({t_off.get("lit")}, expected {unlit})')
        check(abs(t_on.get('lit', 0) - lit_b) <= 3 and t_on.get('restored') == 1 and t_on['released'],
              f'trees lit with the INI Tree colours by our copy of their vertex shader ({t_on.get("lit")}, '
              f'expected {lit_b}); the game\'s shader restored')
        t_bts, _ = shadow_run('lit', 0, 'tree', '[lighting groups]\nTreeSun=0,0,0\nTreeAmbient=255,255,255\n[lighting]\nEnabled=1\n')
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
    _, nlog = shadow_run('normals', 0, 'slope', '[lighting groups]\nTerrainSun=255,255,255\n[lighting]\nEnabled=1\n[debug]\nTraces=1\n'
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
          and 'grid spacing 250.000' in nlog and '1024 cells cover' in nlog,
          f'terrain normals from the vertex buffer: spacing 250 found (1024 cells), normal of the z = 0.2x slope {got} '
          f'(expected 102,128,252)')

    # The height grid covers the whole map at any spacing: three flat terrain chunks (5x5 vertices, heights 10, 20,
    # 30) 1024 cells apart, which a 1024-cell grid would lay over each other. finemap: spacing 11.25 ("High Detail
    # Terrain"), so a Huge map needs 2048 cells; widemap: 22.5, but mtxLightmap says the map is 256 plots across;
    # ultramap: 9x9 chunks of vertices 2.8125 apart (the 65-sample terrain patch with High Detail Terrain), of which the
    # grid keeps 1 in 4 (11.25 apart), so a Huge map still fits: 3x3 known cells a chunk. sharedmap: finemap's three
    # places drawn from one vertex buffer (height 10), as BtS draws some: read in each place.
    import array
    import math
    for fine_overlay, fine_spacing, fine_half, fine_log in (
            ('finemap', '11.250', 2, ''), ('widemap', '22.500', 2, ''), ('sharedmap', '11.250', 2, ''),
            ('ultramap', '11.250', 1, 'grid spacing 11.250 world units (1 in 4 of the vertices, 2.812 apart)')):
        fine_known = 3 * (2 * fine_half + 1) ** 2
        _, flog = shadow_run('heights', 0, fine_overlay, '[lighting]\nEnabled=1\n[debug]\nTraces=1\n[trace]\nAutoFrame=1\n')
        fdir = os.path.join(TEST, f'shadow_{fine_overlay}_heights', 'out', 'test_proxy', 'traces')
        fpath = os.path.join(fdir, os.listdir(fdir)[0], 'terrain_heights.bin') if os.path.isdir(fdir) else ''
        cells, known, wrong = 0, 0, ['no terrain_heights.bin']
        if os.path.exists(fpath):
            h = array.array('f')
            h.frombytes(open(fpath, 'rb').read())
            cells = int(round(math.sqrt(len(h))))
            known = sum(1 for v in h if v == v)
            wrong = []
            for (cx, cy), z in (((0, 0), 10), ((1024, 0), 20), ((0, 1024), 30)):
                z = 10 if fine_overlay == 'sharedmap' else z
                for dy in range(-fine_half, fine_half + 1):
                    for dx in range(-fine_half, fine_half + 1):
                        x, y = (cx + dx) % cells, (cy + dy) % cells
                        if h[y * cells + x] != z:
                            wrong.append(f'({cx + dx},{cy + dy})={h[y * cells + x]} not {z}')
        check(cells == 2048 and known == fine_known and not wrong and f'grid spacing {fine_spacing}' in flog and
              fine_log in flog,
              f'{fine_overlay}: height grid {cells} cells at spacing {fine_spacing} (expected 2048), three chunks 1024 '
              f'cells apart kept apart ({known} known cells, expected {fine_known}{"; " + ", ".join(wrong[:3]) if wrong else ""})')
        # The normal texture, uploaded a chunk at a time (only the cells each changed; the one at (0,0) spans the
        # grid's seam): known exactly at those cells, and flat there.
        npath = os.path.join(os.path.dirname(fpath), 'terrain_normals.bmp') if fpath else ''
        tex_known, tex_wrong = 0, ['no terrain_normals.bmp']
        if os.path.exists(npath):
            nb = open(npath, 'rb').read()
            tex_wrong = []
            alphas = nb[54 + 3::4]  # BGRA, top-down: cell (x, y) at 54 + (y * cells + x) * 4
            tex_known = alphas.count(255)
            for (cx, cy) in ((0, 0), (1024, 0), (0, 1024)):
                for dy in range(-fine_half, fine_half + 1):
                    for dx in range(-fine_half, fine_half + 1):
                        x, y = (cx + dx) % cells, (cy + dy) % cells
                        px = nb[54 + (y * cells + x) * 4:58 + (y * cells + x) * 4]
                        if px[3] != 255 or abs(px[2] - 128) > 1 or abs(px[1] - 128) > 1 or px[0] < 254:
                            tex_wrong.append(f'({cx + dx},{cy + dy})={tuple(px)}')
        check(tex_known == fine_known and not tex_wrong,
              f'{fine_overlay}: the normal texture, updated a chunk at a time, known at exactly the {fine_known} cells read and '
              f'flat there ({tex_known} known{"; " + ", ".join(tex_wrong[:3]) if tex_wrong else ""})')

    # Per-pixel terrain lighting: BtS's colours, the sun moved overhead (LightDirection 0,0,-1). The slope's sun
    # amount changes by N.L(new) - N.L(BtS's sun), N from its normal map texel (102,128,253); BtS's sun direction
    # leaves it exactly as baked.
    import math
    baked = '[lighting groups]\nTerrainSun=246,239,222\nTerrainAmbient=89,103,133\n[lighting]\nEnabled=1\n'
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

    # Detail normals ([textures] DetailNormals): the detail texture's directions on the ground are read from the
    # terrain's TEXCOORD1 (here u = (x + 500) / 1000, v = 1 - (y + 500) / 1000). A flat detail texture adds no bumps
    # (the checks above ran with detail normals on, at their defaults). With the sun overhead, a detail texture that
    # brightens along u by 1/255 a texel tilts the slope's normal by DetailBumpiness x -1/255 along +x:
    # amount += N_detail . L - N . L, N the slope's normal (102,128,253), L straight up.
    _, dlog = shadow_run('detail_axes', 0, 'slope', baked)
    m = re.search(r'detail texture uv per world unit: u = (\S+) x \+ (\S+) y, v = (\S+) x \+ (\S+) y', dlog)
    axes = [float(g) for g in m.groups()] if m else []
    check(len(axes) == 4 and all(abs(a - b) < 1e-5 for a, b in zip(axes, (0.001, 0, 0, -0.001))),
          f"detail normals: the detail texture's directions on the ground read from the terrain's TEXCOORD1 {axes}")
    turned = baked + 'LightDirection=0,0,-1\n'
    bump = 100
    d_on, _ = shadow_run('detail_on', 0, 'slope', turned + f'[textures]\nDetailBumpiness={bump}\n', ramp=True)
    d_off, _ = shadow_run('detail_off', 0, 'slope', turned + '[textures]\nDetailNormals=0\n', ramp=True)
    d_zero, _ = shadow_run('detail_zero', 0, 'slope', turned + '[textures]\nDetailBumpiness=0\n', ramp=True)

    def bumped_change(slope_x, slope_y):
        """Blue change from detail normals on the slope, sun overhead (terrain_shadow.hlsl, axes u +x, v -y)."""
        flat = n
        b = [flat[0] + slope_x, flat[1] + slope_y, flat[2]]  # slope.x * u_axis - slope.y * v_axis, v_axis = (0, -1)
        bl = math.sqrt(sum(x * x for x in b))
        return 255 * (0xE6 / 255) * 0.5 * sun_b * (b[2] / bl - flat[2])

    want = bumped_change(bump * -1 / 255, 0)
    got = d_on.get('lit', 0) - d_off.get('lit', 0)
    check(abs(got - want) <= 2 and abs(want) > 5 and d_zero.get('lit') == d_off.get('lit') and
          d_on.get('restored') == 1 and d_on['released'],
          f"detail normals from the detail texture's brightness: blue {d_on.get('lit')} vs {d_off.get('lit')} without "
          f'(change {got}, expected {want:.1f}); DetailBumpiness=0 changes nothing ({d_zero.get("lit")})')
    # A mod's own normal map for a detail texture: named in its CIV4ArtDefines_Terrain.xml's <Detail>, with _normal
    # before .dds. The harness's detail texture (1x1 grey 128) is recognised by its content; the map's texel
    # (64,128,255) tilts the normal 0.5 against u (-x), at DetailNormalStrength 1.
    def dds_1x1(argb):
        head = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x100F, 1, 1, 4, 0, 1)
        head += struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
        return head + struct.pack('<IIIII', 0x1000, 0, 0, 0, 0) + struct.pack('<I', argb)
    art = (b'<?xml version="1.0"?>\n<Civ4ArtDefines><TerrainArtInfos><TerrainArtInfo><Type>ART_DEF_TERRAIN_TEST</Type>'
           b'<Detail>Art/Terrain/Textures/Detail/TestDetail.dds</Detail></TerrainArtInfo></TerrainArtInfos>'
           b'</Civ4ArtDefines>\n')
    shipped_files = {'Assets/XML/Art/CIV4ArtDefines_Terrain.xml': art,
                     'Assets/Art/Terrain/Textures/Detail/TestDetail.dds': dds_1x1(0xFF808080),
                     'Assets/Art/Terrain/Textures/Detail/TestDetail_normal.dds': dds_1x1(0xFF4080FF)}
    s_on, slog2 = shadow_run('detail_map', 0, 'slope', turned, mod_files=shipped_files)
    s_off, _ = shadow_run('detail_map_off', 0, 'slope', turned + '[textures]\nDetailNormals=0\n',
                          mod_files=shipped_files)
    tilt = 0x40 / 255 * 2 - 1
    want = bumped_change(tilt, 0x80 / 255 * 2 - 1)
    got = s_on.get('lit', 0) - s_off.get('lit', 0)
    check('1 with their own normal map' in slog2 and 'testdetail_normal.dds in use for' in slog2.lower() and
          abs(got - want) <= 2 and abs(want) > 10 and s_on.get('restored') == 1 and s_on['released'],
          f"a mod's own normal map for a detail texture found and used (blue {s_on.get('lit')} vs {s_off.get('lit')} "
          f"without: change {got}, expected {want:.1f})")

    # Blend sheets read inside each square's tile ([textures] BlendTileClamp, Shader Model 3): the lit point (200,0)
    # reads the sheet at v = 0.5, the boundary between two rows of tiles (grey 0x20 above, 0x60 below). Read as BtS
    # does, filtering mixes the two; clamped, it's one tile's grey, as lit as a sheet all of that grey.
    clamp_off = turned + '[textures]\nBlendTileClamp=0\n'
    c_on, clog = shadow_run('tile_clamp', 0, 'slope', turned, sheet='1')
    c_off, _ = shadow_run('tile_clamp_off', 0, 'slope', clamp_off, sheet='1')
    c_lo, _ = shadow_run('tile_clamp_lo', 0, 'slope', clamp_off, sheet='L')
    c_hi, _ = shadow_run('tile_clamp_hi', 0, 'slope', clamp_off, sheet='H')
    lo, hi, on, off = c_lo.get('lit', 0), c_hi.get('lit', 0), c_on.get('lit', 0), c_off.get('lit', 0)
    check('terrain: blend sheets read inside each square\'s tile at every mip (first 16x16, tiles 4 texels)' in clog and
          hi - lo > 40 and lo + 10 < off < hi - 10 and min(abs(on - lo), abs(on - hi)) <= 2 and
          c_on.get('restored') == 1 and c_on['released'],
          f"blend sheets read inside the square's tile: blue {on} at a boundary between tiles ({off} read as BtS does, "
          f"mixing them; one tile alone {lo} or {hi})")

    # [debug] TerrainDebug: 2, each terrain layer a flat colour (the first sheet seen 0x2060E0: blue 224), the rest of the
    # scene left out (no caster, so no shadow); 1, the layers opaque, drawn as they are; 3, the flat colours with the
    # sheet's alpha (the harness's opaque: the same 224).
    d1, _ = shadow_run('terrain_debug1', 1, extra='[debug]\nTerrainDebug=1\n')
    d2, dlog = shadow_run('terrain_debug2', 1, extra='[debug]\nTerrainDebug=2\n')
    d3, _ = shadow_run('terrain_debug3', 1, extra='[debug]\nTerrainDebug=3\n')
    check('terrain debug 2 from start' in dlog and 'drawn as colour 0 (2060e0)' in dlog and d2.get('lit') == 224 and
          d2.get('shadowed') == 224 and d3.get('lit') == 224 and d2['released'] and d3['released'],
          f"[debug] TerrainDebug 2 and 3: each terrain layer a flat colour, nothing else drawn (blue {d2.get('lit')} lit, "
          f"{d2.get('shadowed')} where the shadow was; 3: {d3.get('lit')})")
    d4, d4log = shadow_run('terrain_debug4', 1, extra='[debug]\nTerrainDebug=4\n', sheet='1')
    check('terrain: blend sheets read inside each square' in d4log and d4.get('lit') == 224 and d4['released'],
          f"[debug] TerrainDebug 4: the flat colours through our terrain shader, the sheet read inside its tile (blue "
          f"{d4.get('lit')})")
    check(d1.get('lit') == shadow['on'].get('lit') and d1['released'],
          f"[debug] TerrainDebug 1: the layers drawn opaque, otherwise as they are (blue {d1.get('lit')}, "
          f"{shadow['on'].get('lit')} without)")

    # [terrain tiles] PlotStamps<n>Textures, through the real shader ([debug] PlotStampsTest: a 16 x 16 map, plots 180 across,
    # plot centres at 180 x (i - 7.5), one stamp, every terrain draw its layer). The painting: only its south-east
    # quarter shows, grey 0x60, over the 0x20 sheet. The lit point (200, 0) is in the square of plots (8..9, 7..8).
    def dds_a8r8g8b8(size, texel):
        """Full mips down to 4 texels; texel(u, v) in 0..1, v down, gives (b, g, r, a)."""
        levels = []
        s = size
        while s >= 4:
            levels.append(bytes(c for y in range(s) for x in range(s) for c in texel((x + 0.5) / s, (y + 0.5) / s)))
            s //= 2
        header = bytearray(128)
        header[:4] = b'DDS '
        struct.pack_into('<IIII', header, 4, 124, 0x1007, size, size)
        struct.pack_into('<I', header, 28, len(levels))
        struct.pack_into('<IIIIIIII', header, 76, 32, 0x41, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000)
        return bytes(header) + b''.join(levels)

    stamp_dir = os.path.join(TEST, 'stamp_painting')
    os.makedirs(stamp_dir, exist_ok=True)
    with open(os.path.join(stamp_dir, 'a.dds'), 'wb') as f:
        f.write(dds_a8r8g8b8(64, lambda u, v: (0x60, 0x60, 0x60, 255) if u > 0.5 and v > 0.5 else (0x20, 0x20, 0x20, 0)))

    def stamp_at(x, y, detail=1, ramp=False, over=False, layer='', multiply=0):
        v, slog = shadow_run(f'stamp_{x}_{y}', 0, 'slope', turned + f'[debug]\nPlotStampsTest={stamp_dir};{x};{y}{layer}\n'
                             f'[terrain tiles]\nPlotStamps1TextureDetail={detail}\n' +
                             ('PlotStamps1OnFeatures=FEATURE_TEST\n' if over else '') +
                             f'PlotStamps1TextureMultiply={multiply}\n', sheet='L', ramp=ramp)
        return v, slog

    s_se, s_log = stamp_at(8, 8)   # the point is south-east of this plot: in the painting's opaque quarter
    s_n, _ = stamp_at(8, 7)        # north-east of this one
    s_w, _ = stamp_at(9, 8)        # south-west of this one
    s_far, _ = stamp_at(1, 1)      # not at the square's corners
    # PlotStamps<n>OnFeatures: over the layer. Where the painting's alpha is 0 the layer is exactly as it was.
    o_se, _ = stamp_at(8, 8, over=True)
    o_n, _ = stamp_at(8, 7, over=True)
    o_ramp_n, _ = stamp_at(8, 7, detail=0, ramp=True, over=True)
    o_ramp_far, _ = stamp_at(1, 1, detail=0, ramp=True, over=True)
    # A plot stamped for another layer isn't painted on this one, opaque as it is at the point.
    o_other, _ = stamp_at(8, 8, over=True, layer=';3')
    s_other, _ = stamp_at(8, 8, layer=';3')
    check(abs(o_se.get('lit', 0) - hi) <= 2 and abs(o_n.get('lit', 0) - lo) <= 1 and
          o_ramp_n.get('lit') == o_ramp_far.get('lit') and o_se['released'],
          f"a painting over the layer (a set by feature): shows where its alpha does (blue {o_se.get('lit')}; {hi}), and "
          f"where its alpha is 0 the layer's as it was, its detail too (blue {o_n.get('lit')}; {lo}. "
          f"{o_ramp_n.get('lit')}; {o_ramp_far.get('lit')})")
    check(abs(o_other.get('lit', 0) - lo) <= 1 and abs(s_other.get('lit', 0) - lo) <= 1,
          f"a layer paints only its own stamps: another layer's on the same square leaves it as it was (blue "
          f"{o_other.get('lit')}, {s_other.get('lit')}; {lo})")
    check('plot stamps\' paintings: 1 in a 64x64 atlas, 5 levels' in s_log and s_se.get('restored') == 1 and
          s_se['released'], 'the paintings made into an atlas, the game\'s state restored after, nothing left behind')
    check(abs(s_se.get('lit', 0) - hi) <= 2,
          f"a painted stamp replaces the layer's colour where it shows, lit as the layer (blue {s_se.get('lit')}; a sheet "
          f"of that grey {hi})")
    check(abs(s_n.get('lit', 0) - hi) > 10 and abs(s_w.get('lit', 0) - hi) > 10,
          f"its north is north and its east east: the quarters that don't show don't (blue {s_n.get('lit')}, "
          f"{s_w.get('lit')}; shown {hi})")
    check(abs(s_far.get('lit', 0) - lo) <= 2,
          f"a stamp elsewhere leaves the square's blend alone (blue {s_far.get('lit')}; {lo} without)")
    # The same as DXT5, what the art uses: blocks of one opaque colour, 0x63 blue.
    block = struct.pack('<BB6xHHI', 255, 255, 0x630C, 0x630C, 0)
    dxt5 = bytearray(128)
    dxt5[:4] = b'DDS '
    struct.pack_into('<IIII', dxt5, 4, 124, 0x1007, 64, 64)
    struct.pack_into('<I', dxt5, 28, 5)
    struct.pack_into('<II4s', dxt5, 76, 32, 4, b'DXT5')
    for s in (64, 32, 16, 8, 4):
        dxt5 += block * ((s // 4) ** 2)
    with open(os.path.join(stamp_dir, 'a.dds'), 'wb') as f:
        f.write(bytes(dxt5))
    s_dxt, _ = stamp_at(8, 8)
    check(abs(s_dxt.get('lit', 0) - hi) <= 5 and s_dxt['released'],
          f"a DXT5 painting the same (blue {s_dxt.get('lit')}; a sheet of nearly that grey {hi})")
    # Six paintings: an atlas of 4 x 2 cells. The plot has the sixth (column 1, row 1), the only one of that grey.
    stamp_dir = os.path.join(TEST, 'stamp_cells')
    os.makedirs(stamp_dir, exist_ok=True)
    for name, grey in zip('abcdef', (0x20, 0x20, 0x20, 0x20, 0x20, 0x60)):
        with open(os.path.join(stamp_dir, name + '.dds'), 'wb') as f:
            f.write(dds_a8r8g8b8(64, lambda u, v, g=grey: (g, g, g, 255)))
    s_six, s6log = stamp_at(8, 8)
    # half alpha over the layer: halfway, the layer showing through
    half_dir = os.path.join(TEST, 'stamp_half')
    os.makedirs(half_dir, exist_ok=True)
    with open(os.path.join(half_dir, 'a.dds'), 'wb') as f:
        f.write(dds_a8r8g8b8(64, lambda u, v: (0x60, 0x60, 0x60, 128)))
    stamp_dir, keep_dir = half_dir, stamp_dir
    o_half, _ = stamp_at(8, 8, over=True)
    # PlotStamps<n>TextureMultiply: a black painting added to the light comes out grey; multiplied by it, black
    with open(os.path.join(half_dir, 'a.dds'), 'wb') as f:
        f.write(dds_a8r8g8b8(64, lambda u, v: (0, 0, 0, 255)))
    k_added, _ = stamp_at(8, 8, detail=0, over=True)
    k_mult, _ = stamp_at(8, 8, detail=0, over=True, multiply=1)
    with open(os.path.join(half_dir, 'a.dds'), 'wb') as f:
        f.write(dds_a8r8g8b8(64, lambda u, v: (0x60, 0x60, 0x60, 255)))
    g_mult, _ = stamp_at(8, 8, detail=0, over=True, multiply=1)
    stamp_dir = keep_dir
    check(abs(o_half.get('lit', 0) - (lo + hi) / 2) <= 4,
          f"a part-see-through painting mixes with the layer under it (blue {o_half.get('lit')}; {lo} to {hi})")
    check(k_mult.get('lit', 99) <= 2 and k_added.get('lit', 0) > 30 and g_mult.get('lit', 0) > 30 and k_mult['released'],
          f"PlotStamps<n>TextureMultiply: a black painting stays black (blue {k_mult.get('lit')}; added to the light "
          f"{k_added.get('lit')}), and a colour still takes the light ({g_mult.get('lit')})")
    check('paintings: 6 in a 256x128 atlas' in s6log and abs(s_six.get('lit', 0) - hi) <= 2,
          f"each plot its own painting: the sixth of six read from its place in the atlas (blue {s_six.get('lit')}; "
          f"its grey {hi}, the others {lo})")
    # The detail texture on a painting: all of it at 1, none at 0 (the harness's detail ramp brightens along u, so at the
    # lit point it changes the colour unless it's left out, when the painting's own grey shows).
    d_all, _ = stamp_at(8, 8, 1, True)
    d_none, _ = stamp_at(8, 8, 0, True)
    d_half, _ = stamp_at(8, 8, 0.5, True)
    d_flat, _ = stamp_at(8, 8, 0, False)
    check(abs(d_none.get('lit', 0) - d_flat.get('lit', 1)) <= 1 and abs(d_all.get('lit', 0) - d_none.get('lit', 0)) > 8 and
          min(d_all.get('lit', 0), d_none.get('lit', 0)) < d_half.get('lit', 0) < max(d_all.get('lit', 0), d_none.get('lit', 0)),
          f"PlotStamps<n>TextureDetail: the terrain's detail texture left off a painting at 0 (blue {d_none.get('lit')}, "
          f"whatever the detail: {d_flat.get('lit')}), all of it at 1 ({d_all.get('lit')}), between ({d_half.get('lit')})")

    # A unit under the ground (a sandworm under the sand) casts nothing on its sunward side; over it, it casts as ever.
    s_on, _ = shadow_run('sunk', 1, 'sunk')
    g_on, _ = shadow_run('ground50', 1, 'ground50')
    t_on, _ = shadow_run('shallow', 1, 'shallow')
    print(f'  sunk {s_on}\n  ground50 {g_on}')
    check(abs(s_on.get('sunk', 0) - s_on.get('lit', 0)) <= 2,
          f"a unit under the ground doesn't shade it ({s_on.get('sunk')}, lit {s_on.get('lit')})")
    check(abs(g_on.get('above', 0) - g_on.get('lit', 0) // 2) <= 3,
          f"a unit above raised ground still casts ({g_on.get('above')}, lit {g_on.get('lit')})")
    check(abs(t_on.get('shallow', 0) - t_on.get('lit', 0)) <= 2,
          f"nor does a part just under the ground ({t_on.get('shallow')}, lit {t_on.get('lit')})")

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
    # Wave rings at the water's height don't cast; that height is the water draw's, not [reflection] WaterHeight's
    # start value (a mod's water can lie far below it, its land too: Dune Wars' desert floor). The building stands at
    # z 10: under WaterHeight=49.5, 10 above the water drawn at 0.
    b_low, _ = shadow_run('lowwater', 1, 'buildingwater', '', buildings=1, water_height=49.5)
    b_high, _ = shadow_run('highwater', 1, 'building', '', buildings=1, water_height=49.5)
    check(abs(b_low.get('bld', 0) - round(b_low.get('lit', 0) * 0.32)) <= 3 and
          abs(b_high.get('bld', 0) - b_high.get('lit', 0)) <= 2,
          f'buildings cast down to the height of the water drawn: {b_low.get("bld")} with the water at 0 (expected '
          f'{round(b_low.get("lit", 0) * 0.32)}); with no water drawn, the WaterHeight=49.5 start value keeps the '
          f'building at z 10 from casting ({b_high.get("bld")}, lit {b_high.get("lit")})')
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

    # River (P1, BtS's own shader): f3SunLightDiffuse/AmbientColor in its pixel shader take the River colours, and its
    # ripples are lit from the shadows' sun (MatchShadowDirection=1; -1,0,-1: 45 degrees up, so 0.707 of the sun on
    # the flat river) instead of the game's (straight down here).
    if have_rivers:
        rl, _ = shadow_run('lit', 0, 'river1', river_light)
        up = 0.5 ** 0.5 + 0.5 ** 0.5 * (2 * 0x80 / 255 - 1)  # the normal map's 0x80 is a hair off flat
        expect_r = round(255 * (0xE6 / 255) * min(1.0, (0x80 / 255) * min(1.0, sun[2] * up + amb[2])))
        check(abs(rl.get('lit', 0) - expect_r) <= 3 and rl.get('restored') == 1 and rl['released'],
              f'river lit with the INI colours, from the shadows\' sun (blue {rl.get("lit")}, expected {expect_r}, BtS 92)')
        # The sea's glint off the river: with the sun straight up and the view straight down, the river mirrors the
        # middle of the sky texture's painted sun (alpha 1) into the camera: the river's sun (0.6) at RiverGlint=128 added
        # as the sea adds it, in squared colours (and times the fog of war, 0xE6).
        # RiverGlint=0 (or the view not lined up, as in the checks above): as BtS draws it.
        rg, rglog = shadow_run('glint', 0, 'river1', '[water]\nEnabled=1\nGlintFollowsSun=1\n', sun='0,0,-1')
        rg0, _ = shadow_run('glint0', 0, 'river1', '[water]\nEnabled=1\nGlintFollowsSun=1\nRiverGlint=0\n', sun='0,0,-1')
        expect_g = round(255 * ((0xE6 / 255 * 0x80 / 255 * 0.8) ** 2 + 0.6 ** 2 * 0xE6 / 255) ** 0.5)
        check(abs(rg.get('lit', 0) - expect_g) <= 4 and abs(rg0.get('lit', 0) - 92) <= 3 and rg.get('restored') == 1 and
              rg['released'],
              f'the glint off the river where it mirrors the sun into the view (blue {rg.get("lit")}, expected '
              f'{expect_g}); RiverGlint=0: none ({rg0.get("lit")}, BtS 92)')
        # The ripples sway along the channel (RiverShimmer, 3 texels by default): the flow map made and used. Here the
        # river's texture coordinates sit at the atlas' corner, off any channel, so the colour above isn't moved.
        check('their ripples sway along the channels, 3 texels each way (flow map 128x256)' in rglog,
              "the rivers' ripples sway along the channels (flow map made)")

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
        tl_old, _ = shadow_run('old_lit', 0, 'tree', '[lighting groups]\nTreeSun=200,100,50\nTreeAmbient=20,40,60\n[lighting]\nEnabled=1\n',
                               game_dir=OLD_GAME_SHADERS)
        tl_new, _ = shadow_run('lit', 0, 'tree', '[lighting groups]\nTreeSun=200,100,50\nTreeAmbient=20,40,60\n[lighting]\nEnabled=1\n')
        check(tl_old.get('lit') == tl_new.get('lit') and tl_old['released'],
              f"older build's tree shader lit as Steam's ({tl_old.get('lit')} = {tl_new.get('lit')})")
    else:
        print('  skip  older build\'s tree and river shaders: set GU_BTS_OLD_SHADERS to its Shaders folder')

    print('water:')
    # The water textures as packaged (textures/, as a GraphicsUpgrade folder next to each test DLL).
    water_assets = os.path.join(ROOT, 'textures')

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
            f.write(f'[general]\nOutputDir={wout_dir}\nGames=*\n[water]\nEnabled=1\n')
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
            mn = re.search(r'WATERNM mean=(\d+) flat=(\d+)', cout)
            nm_mean, flat_mean = (int(mn.group(1)) / 1000, int(mn.group(2)) / 1000) if mn else (99, 0)
            check(mn and nm_mean < 0.1 * flat_mean and flat_mean > 1,
                  f"DXT5nm normal maps (x in alpha, y in green, f4NormalFormat c8 = 1) draw the same water as the same "
                  f"normals in rgb: on average {nm_mean:.2f} of 255 apart, against {flat_mean:.2f} for a flat map "
                  f"(the test sky is random noise, so single pixels can differ)")
            ms = re.search(r'WATERSHORE neutral=(\d+) turned=(\d+) caustics=(\d+)', cout)
            if 'WATERSHORE skipped' in cout:
                print('  skip  shoreline water shader: no Shader Model 3 here')
            else:
                check(ms and int(ms.group(1)) <= 1 and int(ms.group(2)) > 1000 and int(ms.group(3)) > 1000,
                      f"the shoreline water shader: with its additions off it draws the plain water (worst "
                      f"{ms.group(1) if ms else '?'}); the glint turned changes {ms.group(2) if ms else '?'} channels; "
                      f"caustics in shallow water change {ms.group(3) if ms else '?'}")
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

        # The shoreline water (Shader Model 3) through the proxy. The glint follows the sun: with the sun exactly where
        # the sky texture painted it, the picture is as before. (This scene's camera has no eye to reflect a sun to;
        # the water comparison scene above checks that a turned glint moves.)
        def shore_run(label, water_ini, sun='', extra_files=None):
            sdir = os.path.join(TEST, 'water_shore_' + label)
            if os.path.isdir(sdir):
                shutil.rmtree(sdir)
            os.makedirs(sdir)
            shutil.copyfile(DLL, os.path.join(sdir, 'd3d9.dll'))
            install_water(sdir)
            for src, name in (extra_files or []):
                shutil.copyfile(src, os.path.join(sdir, 'GraphicsUpgrade', name))
            with TestIni(os.path.join(sdir, 'GraphicsUpgrade.ini')) as f:
                f.write(f'[general]\nOutputDir={os.path.join(sdir, "out")}\nGames=*\n[water]\nEnabled=1\n'
                        f'{water_ini}[shadows]\nSunDirection={sun or "-1,0,-1"}\n'
                        f'[debug]\nWaterTime=7.5\n')  # the water's clock stopped: every run draws the same frame
            out = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(sdir, 'd3d9.dll'), 'water'])
            log = open(os.path.join(sdir, 'out', 'test_proxy', 'session.log'), encoding='latin-1').read()
            m = re.search(r'sum=(\d+) brightest=(\d+),(\d+)', out)
            return (int(m.group(1)), (int(m.group(2)), int(m.group(3)))) if m else (0, (0, 0)), log, out

        painted = '-0.4531,-0.5757,-0.6807'  # the sun painted in water_env.dds, as a travel direction
        (base_sum, base_bright), _, _ = shore_run('off', 'GlintFollowsSun=0\n')
        (same_sum, same_bright), slog, sout = shore_run('painted', 'GlintFollowsSun=1\nCaustics=1\n', painted)
        if 'Shader Model 3; left out' in slog:
            print('  skip  shoreline water: no Shader Model 3 here')
        else:
            check('drawn with the shoreline water (glint follows the sun on' in slog and
                  'no caustics; the glint still follows the sun' in slog and
                  'final refs device 0, d3d 0' in sout,
                  'glint follows the sun: the Shader Model 3 water drawn; without caustic textures no caustics; '
                  'device released')
            check(abs(same_sum - base_sum) <= base_sum * 0.001,
                  f'glint follows the sun: with the sun where the sky texture painted it, the picture is as before '
                  f'(frame {same_sum} vs {base_sum})')
            # Caustics: the bundled textures from the GraphicsUpgrade folder load (no terrain here, so no
            # shore is known and the picture is the glint's alone).
            files = [(os.path.join(ROOT, 'textures', fn), fn) for fn in ('water_caustics1.dds', 'water_caustics2.dds')]
            (tex_sum, _), tlog, tout = shore_run('textures', 'GlintFollowsSun=1\nCaustics=1\n', painted, files)
            check('shoreline and shallow water ready' in tlog and 'shore not read yet or off' in tlog and
                  abs(tex_sum - base_sum) <= base_sum * 0.001 and 'final refs device 0, d3d 0' in tout,
                  f'caustics: the bundled textures load from the GraphicsUpgrade folder; with no shore '
                  f'known the water is unchanged (frame {tex_sum} vs {base_sum}); device released')
            # Wave shading: the waves lit by the glint's light change the picture; off, it's as before.
            (wave_sum, _), wlog, wout = shore_run('wave_shading', 'GlintFollowsSun=0\nWaveShading=255\n')
            check('wave shading 255/255' in wlog and wave_sum != base_sum and 'final refs device 0, d3d 0' in wout,
                  f'wave shading: the Shader Model 3 water drawn with the waves lit, so the picture changes '
                  f'(frame {wave_sum} vs {base_sum}); device released')


        # The water's sun colour comes from the lighting (WaterSun, else SunColour, else the cycle); without the
        # lighting, Colonization's own.
        def water_sun(label, light, water='', numbered=0, nm=(), mod_ini=None):
            """water: more [water] keys (a second [water] section would be ignored); numbered: that many
            water_normal_1.dds, _2... (copies of water_normal.dds) in the GraphicsUpgrade folder too, those numbered
            in nm marked DXT5nm (DDPF_NORMAL); mod_ini: a mod's GraphicsUpgrade.ini (run with mod=...)."""
            sdir = os.path.join(TEST, 'water_sun_' + label)
            if os.path.isdir(sdir):
                shutil.rmtree(sdir)
            os.makedirs(sdir)
            shutil.copyfile(DLL, os.path.join(sdir, 'd3d9.dll'))
            install_water(sdir)
            for i in range(1, numbered + 1):
                data = bytearray(open(os.path.join(water_assets, 'water_normal.dds'), 'rb').read())
                if i in nm:
                    data[83] |= 0x80  # the pixel format's flags (offset 80): DDPF_NORMAL
                with open(os.path.join(sdir, 'GraphicsUpgrade', f'water_normal_{i}.dds'), 'wb') as f:
                    f.write(data)
            with TestIni(os.path.join(sdir, 'GraphicsUpgrade.ini')) as f:
                f.write(f'[general]\nOutputDir={os.path.join(sdir, "out")}\nGames=*\n[water]\nEnabled=1\n'
                        f'{water}[lighting]\nEnabled=1\n{light}')
            extra = []
            if mod_ini is not None:
                mod_gu = os.path.join(sdir, 'Mods', 'TestMod', 'GraphicsUpgrade')
                os.makedirs(mod_gu)
                with open(os.path.join(mod_gu, 'GraphicsUpgrade.ini'), 'w') as f:
                    f.write(mod_ini)
                extra = ['mod=' + os.path.join(sdir, 'Mods', 'TestMod')]
            run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(sdir, 'd3d9.dll'), 'water'] + extra)
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
              "water: sun 10,20,30 (the lighting's)" in water_sun('own', 'SunColour=1,2,3\n[lighting groups]\nWaterSun=10,20,30\n') and
              "water: sun 1,2,3 (the lighting's)" in water_sun('shared', 'SunColour=1,2,3\n'),
              "water sun: Colonization's without the lighting, else WaterSun, else SunColour")
        # The water's look, one key each (Colonization's by default).
        check('reflection evenness 0.22995, reflection ripple 0.238738, refraction ripple 0.0954951' in wlog and
              'reflection evenness 1, reflection ripple 0.5, refraction ripple 0' in
              water_sun('constants', '', water='ReflectionEvenness=1\nReflectionRipple=0.5\nRefractionRipple=0\n'),
              "water: ReflectionEvenness, ReflectionRipple and RefractionRipple read (Colonization's by default)")
        # 1.0's INIs: WaterConstants=x,y,z,w, read only when none of the three new keys is set (in that file: a mod's old
        # line still overrides the game folder's new keys).
        old_warning = ('settings: [water] WaterConstants is the old name; use ReflectionEvenness, ReflectionRipple and '
                       'RefractionRipple')
        wc_old = water_sun('constants_old', '', water='WaterConstants=1,0.5,0,0.7\n')
        wc_both = water_sun('constants_both', '', water='WaterConstants=1,0.5,0,0.7\nReflectionRipple=0.3\n')
        wc_mod = water_sun('constants_mod', '', water='ReflectionEvenness=0.1\nReflectionRipple=0.2\nRefractionRipple=0.3\n',
                           mod_ini='[water]\nWaterConstants=1,0.5,0,0.7\n')
        check('reflection evenness 1, reflection ripple 0.5, refraction ripple 0' in wc_old and old_warning in wc_old and
              'reflection evenness 0.22995, reflection ripple 0.3, refraction ripple 0.0954951' in wc_both and
              old_warning not in wc_both and
              'reflection evenness 1, reflection ripple 0.5, refraction ripple 0' in wc_mod and old_warning in wc_mod,
              "water: the old WaterConstants still read, with a warning, unless a new key is set; a mod's old line "
              "overrides the game folder's new keys")
        # GlintFollowsSun=1 with the light cycle: the glint crosses the view right to left by day (sunrise 6 h to sunset
        # 18 h) and again by night (the moon's, 18 h to 6 h), so it goes off the left edge and comes back on the right.
        arc = ('LightCycleOn=1\nLightChangeCycleTime=100000\nLightChangeStepCount=24\n'
               'LightCycleSteps=12,100,100,100;0,10,20,30\n')
        # It crosses the view at a steady pace, in full from about 8:20 to 15:40, and goes round the back quickly, behind
        # the camera at sunrise and sunset, out of view (brightness 0 at 6:15, 0.02 of the way round).
        arc_logs = {h: water_sun(f'glint_arc_{h}', arc + f'LightCycleStartStep={h}\n', water='GlintFollowsSun=1\n')
                    for h in (9, 12, 21, 0, 6.25)}
        expect_arc = {9: "the sun's glint, 0.25 of the way round (right to left), brightness 1.00",
                      12: "the sun's glint, 0.50 of the way round (right to left), brightness 1.00",
                      21: "the moon's glint, 0.25 of the way round (right to left), brightness 1.00",
                      0: "the moon's glint, 0.50 of the way round (right to left), brightness 1.00",
                      6.25: "the sun's glint, 0.02 of the way round (right to left), brightness 0.00"}
        got_arc = {h: (re.search(r"the (?:sun|moon)'s glint, .*brightness [\d.]+", log) or [None])[0]
                   for h, log in arc_logs.items()}
        check(all(expect_arc[h] in (got_arc[h] or '') for h in expect_arc) and 'glint, ' not in wlog,
              f"the glint crosses right to left by day and again by night, fading in and out at the ends: {got_arc} "
              f"(expected 9 h the sun's 0.25 of the way, 12 h 0.50, 21 h the moon's 0.25, 0 h 0.50, all at full "
              f"brightness; 6:15 0.02 of the way round, behind the camera, at 0); none without the cycle")
        # More normal maps beside water_normal.dds: water_normal_1.dds, _2... loaded (at 1440 world units per repeat);
        # NormalMap picks the one at the start, falling back to water_normal.dds when there's no such file.
        two = water_sun('normals', '', water='NormalMap=2\n', numbered=2, nm=(2,))
        bad = water_sun('normals_bad', '', water='NormalMap=3\n', numbered=2)
        check(re.search(r'water: normal map 1: .*water_normal_1\.dds \(1440 world units per repeat\)', two) is not None
              and 'water_normal_2.dds' in two and 'water_normal_3.dds' not in two.replace('NormalMap=3', '') and
              'water: starting with normal map 2' in two and
              re.search(r'water_normal_2\.dds \(1440 world units per repeat, DXT5nm\)', two) is not None and
              re.search(r'water_normal_1\.dds \(1440 world units per repeat\)', two) is not None and
              "NormalMap=3, but there's no water_normal_3.dds; water_normal.dds instead" in bad and
              'normal map 1:' not in wlog,
              'water normal maps: water_normal_1.dds and _2 found beside water_normal.dds (none in the plain run); '
              'NormalMap=2 starts with the second, NormalMap=3 falls back to water_normal.dds; a DXT5nm one '
              '(DDPF_NORMAL) is recognised')

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
                        f'[water]\nEnabled=1\nUnderwaterDepth={depth}\nUnderwaterVisibility=0.7\n'
                        f'UnderwaterBands=8\n[reflection]\nWaterHeight=0.5\n')
            res = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(udir, 'd3d9.dll'), 'water'])
            traces = os.path.join(udir, 'out', 'test_proxy', 'traces')
            bmp = open(os.path.join(traces, os.listdir(traces)[0], 'refraction.bmp'), 'rb').read()
            size = struct.unpack_from('<i', bmp, 18)[0]
            red = lambda x, y: bmp[54 + (int((1 - y) / 2 * size) * size + int((x + 1) / 2 * size)) * 4 + 2]
            return red(-0.5, 0.47), red(0.5, 0.47), red(0.5, -0.47), 'final refs device 0, d3d 0' in res
        expect = round(255 * 0.7 * (1 - 1.5 / 8))
        sub_red, ship_red, rigid_red, released = underwater(0.2)
        none_red, _, none_rigid, _ = underwater(0)
        check(abs(sub_red - expect) <= 3 and ship_red == 0 and released,
              f'submarine below the surface shows through the water, faded by depth (red {sub_red}, expected {expect}); '
              f'the ship above the surface stays out of the refraction ({ship_red})')
        check(abs(rigid_red - expect) <= 3,
              f'a submarine drawn as a rigid fixed-function mesh (no unit shader, no vertex blending) shows the same way, '
              f'and the screen copy comes before it (red {rigid_red}, expected {expect})')
        check(none_red == 0 and none_rigid == 0,
              f'UnderwaterDepth=0: nothing under the surface shown, as before ({none_red}, {none_rigid})')

        # Sea resources (whales, fish...), which BtS draws after the water: kept and drawn into the next frame's
        # reflection and refraction, before its water, as ships are. Frame 2 (traced) replays frame 1's models through
        # its own camera, which has moved 0.1 right (clip x) since: the two above the water reflected there (mirrored in
        # x), the one under it in the refraction there; the model over land not kept, so not reflected; one lying flat at
        # the water's height (like clams) in neither, so it can't flicker in and out of them at the surface.
        sdir = os.path.join(TEST, 'water_sea')
        if os.path.isdir(sdir):
            shutil.rmtree(sdir)
        os.makedirs(sdir)
        shutil.copyfile(DLL, os.path.join(sdir, 'd3d9.dll'))
        install_water(sdir)
        with TestIni(os.path.join(sdir, 'GraphicsUpgrade.ini')) as f:
            f.write(f'[general]\nOutputDir={os.path.join(sdir, "out")}\nGames=*\n[debug]\nTraces=1\n[trace]\nAutoFrame=2\n'
                    f'[water]\nEnabled=1\nUnderwaterDepth=50\nUnderwaterVisibility=0.7\n'
                    f'UnderwaterBands=8\n[reflection]\nEnabled=1\nSize=512\n')
        sres = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(sdir, 'd3d9.dll'), 'sea'])
        straces = os.path.join(sdir, 'out', 'test_proxy', 'traces')
        stdir = os.path.join(straces, os.listdir(straces)[0]) if os.path.isdir(straces) and os.listdir(straces) else ''

        def bmp_at(name, x, y):
            """(blue, green, red, alpha) at clip (x, y) of one of the trace's 32-bit BMPs."""
            path = os.path.join(stdir, name)
            if not os.path.exists(path):
                return (0, 0, 0, 0)
            bmp = open(path, 'rb').read()
            size = struct.unpack_from('<i', bmp, 18)[0]
            o = 54 + (int((1 - y) / 2 * size) * size + int((x + 1) / 2 * size)) * 4
            return tuple(bmp[o:o + 4])

        refl = {k: int(bmp_at('reflection.bmp', *p)[3] > 128) for k, p in
                {'S1': (0.1, -0.2), 'S1_old_camera': (0.2, -0.2), 'F': (0.1, 0.2), 'F_old_camera': (0.2, 0.2),
                 'L': (-0.6, -0.5), 'C': (0.5, 0.6)}.items()}
        under = {k: (lambda c: c[2] - c[1])(bmp_at('refraction.bmp', *p)) for k, p in
                 {'S2': (0.3, 0.2), 'S2_old_camera': (0.2, 0.2), 'C': (-0.5, 0.6)}.items()}
        sevents = [json.loads(line) for line in open(os.path.join(stdir, 'trace.jsonl'))] if stdir else []
        held = sum(1 for e in sevents if e.get('ev') == 'sea_model_held')
        replayed = [e.get('draws') for e in sevents if e.get('ev') == 'sea_models_replayed']
        ms = re.search(r'SEA restored=(\d) s1=([0-9a-f]{8})', sres)
        check(refl == {'S1': 1, 'S1_old_camera': 0, 'F': 1, 'F_old_camera': 0, 'L': 0, 'C': 0} and under['S2'] > 40 and
              under['S2_old_camera'] < 10 and abs(under['C']) < 10 and held == 4 and replayed == [4],
              f"sea resources drawn after the water (unit shader and fixed function) are kept and drawn into the next "
              f"frame's reflection and refraction through its camera: reflected {refl}; under the water, in the "
              f"refraction (red over green {under}); {held} kept (not the one over land), {replayed} replayed")
        check(ms and ms.group(1) == '1' and 'final refs device 0, d3d 0' in sres,
              "the game's water draw state is back after the replays before it; device fully released")
        check(ms and int(ms.group(2), 16) & 0xFFFFFF == 0xFF0000,
              f"the map shows the sea resource from the game's own draw after the water, untouched (pixel "
              f"{ms.group(2) if ms else '?'})")

        # Bundled files only (as in the release package: a GraphicsUpgrade folder next to the DLL): the water must be
        # ours. Then none: BtS water left alone.
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
                f.write(f'[general]\nOutputDir={bout}\nGames=*\n[water]\nEnabled=1\n')
            bres = run([os.path.join(TEST, 'test_proxy.exe'), os.path.join(bdir, 'd3d9.dll'), 'water'])
            blog = open(os.path.join(bout, 'test_proxy', 'session.log'), encoding='latin-1').read()
            mb = re.search(r'WATER pixel=([0-9a-f]{8})', bres)
            magenta = bool(mb) and (int(mb.group(1), 16) & 0xFFFFFF) == 0xFF00FF
            if label == 'oldname':
                check(not magenta and 'water: textures from ' + os.path.join(bdir, 'GraphicsUpgrade', 'water_001.dds') in blog,
                      'a 1.0.x GraphicsUpgrade folder (the normal map under its old name, water_001.dds) still works')
            elif bundle:
                check(not magenta and 'water: textures from ' + os.path.join(bdir, 'GraphicsUpgrade', 'water_normal.dds') in blog,
                      'bundled GraphicsUpgrade folder used (as in the release package)')
            else:
                check(magenta and 'water: textures not found' in blog,
                      'no bundled files: BtS water left alone, logged')
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
                f.write(f'[general]\nOutputDir={os.path.join(sdir, "out")}\nGames=*\n[water]\n'
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

        # SurfaceTextures written on two lines (a mod's lake texture added under the oasis'): both lines count, and
        # the log says so. An ordinary setting written twice: only its first line counts, and the log says that.
        twice = ('Enabled=1\nSurfaceTextures=Art\\Terrain\\Features\\BodyOfWater\\lake_water.dds\nSurfaceTextures={dds}\n'
                 'UnderwaterBands=8\nUnderwaterBands=4\n')
        two, two_ok, two_log = surface_run('twolines', 15, twice)
        check(two is not None and two_ok and two[1] < 250 and two[0] > 5 and
              'settings: [water] SurfaceTextures is set on 2 lines' in two_log and
              'the entries of all of them are used' in two_log and
              'BodyOfWater\\lake_water.dds: no .dds textures found' in two_log,
              f'SurfaceTextures on two lines: the second line still counts ({two}), and the log warns')
        check('settings: [water] UnderwaterBands is set on 2 lines' in two_log and
              'only the first counts, so remove the others' in two_log,
              'an ordinary setting written twice: the log says only the first line counts')

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
        shutil.copyfile(os.path.join(ROOT, 'textures', 'day-night.png'), os.path.join(rel_dir, 'GraphicsUpgrade', 'day-night.png'))
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
        check(re.search(r'LightCycleImage: .*day-night\.png, 24x2: 24 steps, sun from the top row, ambient from the '
                        r'second', relog) is not None,
              "the shipped day-night colours read from GraphicsUpgrade\\day-night.png (24 x 2: 24 steps)")

    print('version resource:')
    check(file_version(os.path.join(TEST, 'd3d9.dll')) ==
          file_version(os.path.join(os.environ['SystemRoot'], 'SysWOW64', 'd3d9.dll')),
          'proxy file version = system d3d9.dll file version')
    check('first call: IDirect3D9::GetDeviceCaps' in log or 'first call: IDirect3DDevice9::SetStreamSource' in log,
          'first-call logging')
    # The game checks formats thousands of times at startup: IDirect3D9's calls too are logged once each.
    repeated = re.findall(r'\] call: \S+', log)
    check(not repeated and 'first call: IDirect3D9::' in log,
          f"IDirect3D9 calls logged only the first time, so the log stays short ({len(repeated)} repeated call lines)")

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
    verts = [e for e in events if e['ev'] == 'verts']
    check(d1['call'] == 'DIP' and len(verts) == 1 and verts[0]['i'] == d1['i'] and verts[0].get('idx') == [0, 1, 2] and
          [round(v, 3) for v in verts[0]['pos']] == [-0.8, -0.8, 0.5, 0, 0.8, 0.5, 0.8, -0.8, 0.5] and
          'stages' not in verts[0],
          'small indexed draw: its vertex positions and indices recorded (shader draw: no texture stages)')
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

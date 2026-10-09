"""Build GraphicsUpgrade and assemble the release package.

  uv run tools/package.py

Builds the proxy (build.bat), then writes dist/GraphicsUpgrade-<version>/ and dist/GraphicsUpgrade-<version>.zip:

  d3d9.dll               the proxy
  GraphicsUpgrade.ini    settings
  presets/               [lighting] sections for BtS's, Colonization's and Civilization VI's lighting
  GraphicsUpgrade/       Colonization's water textures: water_001.dds (normal map), water_env.dds (environment cube)

To install, copy the folder's contents into the Beyond the Sword folder.
"""
import hashlib
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WATER_TEXTURES = ('water_001.dds', 'water_env.dds')


def version():
    src = (ROOT / 'src' / 'common.h').read_text(encoding='utf-8')
    return re.search(r'#define GRAPHICSUPGRADE_VERSION "([^"]+)"', src).group(1)


def build():
    r = subprocess.run(['cmd', '/c', str(ROOT / 'build.bat')], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-4000:], r.stderr[-4000:])
        sys.exit('build failed')


def main():
    build()
    name = f'GraphicsUpgrade-{version()}'
    dist = ROOT / 'dist'
    pkg = dist / name
    if pkg.exists():
        shutil.rmtree(pkg)
    (pkg / 'presets').mkdir(parents=True)
    (pkg / 'GraphicsUpgrade').mkdir()

    shutil.copyfile(ROOT / 'build' / 'd3d9.dll', pkg / 'd3d9.dll')
    shutil.copyfile(ROOT / 'GraphicsUpgrade.ini', pkg / 'GraphicsUpgrade.ini')
    for p in sorted((ROOT / 'presets').glob('*.ini')):
        shutil.copyfile(p, pkg / 'presets' / p.name)
    for fn in WATER_TEXTURES:
        shutil.copyfile(ROOT / 'textures' / fn, pkg / 'GraphicsUpgrade' / fn)

    files = sorted(p for p in pkg.rglob('*') if p.is_file())
    zip_path = dist / f'{name}.zip'
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in files:
            z.write(p, Path(name) / p.relative_to(pkg))

    print(f'{name}:')
    for p in files:
        digest = hashlib.sha256(p.read_bytes()).hexdigest()[:16]
        print(f'  {p.relative_to(pkg).as_posix():36s} {p.stat().st_size:>9,d}  sha256 {digest}')
    print(f'zip: {zip_path} ({zip_path.stat().st_size:,d} bytes)')


if __name__ == '__main__':
    main()

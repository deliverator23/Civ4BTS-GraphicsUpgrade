"""Shaders from a game's compiled effects (Shaders/FXO/*.fx), for the tests: loads the effect with the 64-bit D3DX
(D3DX9_43.dll in System32) on a hidden device and writes each pass's vertex and pixel shader as <kind>_<hash>.bin,
named the way GraphicsUpgrade's shader dumps are (hash = 64-bit FNV-1a of the shader's bytes).
"""
import ctypes
import ctypes.wintypes as wt
import os

FNV_OFFSET = 1469598103934665603


def fnv1a(data, h=FNV_OFFSET):
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


class _PresentParameters(ctypes.Structure):
    _fields_ = [('BackBufferWidth', ctypes.c_uint), ('BackBufferHeight', ctypes.c_uint),
                ('BackBufferFormat', ctypes.c_uint), ('BackBufferCount', ctypes.c_uint),
                ('MultiSampleType', ctypes.c_uint), ('MultiSampleQuality', ctypes.c_uint),
                ('SwapEffect', ctypes.c_uint), ('hDeviceWindow', wt.HWND), ('Windowed', wt.BOOL),
                ('EnableAutoDepthStencil', wt.BOOL), ('AutoDepthStencilFormat', ctypes.c_uint),
                ('Flags', ctypes.c_uint), ('FullScreen_RefreshRateInHz', ctypes.c_uint),
                ('PresentationInterval', ctypes.c_uint)]


class _PassDesc(ctypes.Structure):
    _fields_ = [('Name', ctypes.c_char_p), ('Annotations', ctypes.c_uint),
                ('pVertexShaderFunction', ctypes.c_void_p), ('pPixelShaderFunction', ctypes.c_void_p)]


def _vcall(obj, index, restype, *args):
    """COM vtable slot `index` on interface pointer obj, with (argtype, value) pairs."""
    vtbl = ctypes.cast(ctypes.c_void_p.from_address(obj).value, ctypes.POINTER(ctypes.c_void_p))
    fn = ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *[a[0] for a in args])(vtbl[index])
    return fn(obj, *[a[1] for a in args])


# ID3DXEffect (ID3DXBaseEffect) vtable slots.
_GETPASSDESC, _GETTECHBYNAME, _GETPASS = 6, 13, 14


class EffectReader:
    def __init__(self):
        self.d3d9 = ctypes.WinDLL('d3d9.dll')
        self.d3dx = ctypes.WinDLL(os.path.join(os.environ['SystemRoot'], 'System32', 'D3DX9_43.dll'))
        self.d3dx.D3DXGetShaderSize.argtypes = [ctypes.c_void_p]
        self.d3dx.D3DXGetShaderSize.restype = ctypes.c_uint
        user32 = ctypes.WinDLL('user32.dll')
        user32.CreateWindowExW.restype = wt.HWND
        user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_int, ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
        self.d3d9.Direct3DCreate9.restype = ctypes.c_void_p
        self.d3d9.Direct3DCreate9.argtypes = [ctypes.c_uint]
        hwnd = user32.CreateWindowExW(0, 'STATIC', 'game_shaders', 0, 0, 0, 64, 64, None, None, None, None)
        d3d = self.d3d9.Direct3DCreate9(32)
        pp = _PresentParameters(BackBufferWidth=64, BackBufferHeight=64, SwapEffect=1, hDeviceWindow=hwnd, Windowed=1)
        dev = ctypes.c_void_p()
        # IDirect3D9::CreateDevice (slot 16), HAL, software vertex processing
        hr = _vcall(d3d, 16, ctypes.c_long, (ctypes.c_uint, 0), (ctypes.c_uint, 1), (wt.HWND, hwnd),
                    (ctypes.c_uint, 0x20), (ctypes.POINTER(_PresentParameters), ctypes.byref(pp)),
                    (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(dev)))
        if hr < 0:
            raise OSError(f'CreateDevice failed {hr & 0xffffffff:#x}')
        self.dev = dev.value

    def _shader(self, ptr):
        if not ptr:
            return None
        code = ctypes.string_at(ptr, self.d3dx.D3DXGetShaderSize(ptr))
        tok = int.from_bytes(code[:4], 'little')
        kind = ('vs' if tok >> 16 == 0xFFFE else 'ps') + f'_{(tok >> 8) & 0xFF}_{tok & 0xFF}'
        return kind, code

    def passes(self, fxo_path, technique, pass_indices):
        """[(vs (kind, bytes) or None, ps (kind, bytes) or None)] for those passes of the technique."""
        data = open(fxo_path, 'rb').read()
        create = self.d3dx.D3DXCreateEffect
        create.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p,
                           ctypes.c_uint, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p),
                           ctypes.POINTER(ctypes.c_void_p)]
        create.restype = ctypes.c_long
        eff = ctypes.c_void_p()
        # D3DXSHADER_SKIPVALIDATION: keep techniques this card would reject
        if create(self.dev, data, len(data), None, None, 0x2, None, ctypes.byref(eff), None) < 0:
            raise OSError(f'D3DXCreateEffect failed on {fxo_path}')
        tech = _vcall(eff.value, _GETTECHBYNAME, ctypes.c_void_p, (ctypes.c_char_p, technique.encode()))
        out = []
        for i in pass_indices:
            p = _vcall(eff.value, _GETPASS, ctypes.c_void_p, (ctypes.c_void_p, tech), (ctypes.c_uint, i))
            desc = _PassDesc()
            _vcall(eff.value, _GETPASSDESC, ctypes.c_long, (ctypes.c_void_p, p),
                   (ctypes.POINTER(_PassDesc), ctypes.byref(desc)))
            out.append((self._shader(desc.pVertexShaderFunction), self._shader(desc.pPixelShaderFunction)))
        _vcall(eff.value, 2, ctypes.c_ulong)  # Release
        return out


def find_fxo(shader_dir, name):
    """The effect file in a Shaders/FXO folder, matched without regard to case (the games' names vary)."""
    folder = os.path.join(shader_dir, 'FXO')
    if os.path.isdir(folder):
        for f in os.listdir(folder):
            if f.lower() == name.lower():
                return os.path.join(folder, f)
    return None


def extract(reader, shader_dir, effect, technique, pass_indices, out_dir):
    """Writes the passes' shaders to out_dir as <kind>_<hash>.bin; returns their file names, or None if the effect
    isn't there."""
    path = find_fxo(shader_dir, effect)
    if not path:
        return None
    os.makedirs(out_dir, exist_ok=True)
    names = []
    for vs, ps in reader.passes(path, technique, pass_indices):
        for shader in (vs, ps):
            if shader:
                kind, code = shader
                name = f'{kind}_{fnv1a(code):016x}.bin'
                with open(os.path.join(out_dir, name), 'wb') as f:
                    f.write(code)
                names.append(name)
    return names

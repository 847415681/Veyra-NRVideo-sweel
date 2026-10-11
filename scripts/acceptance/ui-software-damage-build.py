"""Scoped UI build: main.cpp only; all engine objects are accepted immutable inputs.

Also configures the real CMake project: no QuickPrivate or full-redraw dependency.
--probe compiles a PRIVATE copy of main.cpp with the test-only renderer probe.
"""
from pathlib import Path
import ctypes, hashlib, json, os, re, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[2]
BASE = Path('E:/项目/Veyra'); TASK = 'ui-smear-handoff-20261011'
OUT = BASE / 'build' / TASK; TMP = BASE / 'tmp' / TASK / 'build'
LOG = BASE / 'logs' / TASK / ('build-' + time.strftime('%Y%m%d-%H%M%S'))
PRIOR = BASE / 'logs/sr-visual-quality-20261010/build-20261010-153535/receipt.json'
CACHE = BASE / 'build/vfg-next-20261007'
for p in (OUT, TMP, LOG): p.mkdir(parents=True, exist_ok=True)
began = time.monotonic(); commands = []; inputs = {}

def sha(p):
    with Path(p).open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()

def protect(p):
    p = Path(p); assert p.is_file(), p
    inputs[str(p)] = sha(p); return p

shell = ctypes.WinDLL('shell32')
shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]

def args(s):
    n = ctypes.c_int(); p = shell.CommandLineToArgvW(s, ctypes.byref(n))
    result = [p[i] for i in range(n.value)]; ctypes.windll.kernel32.LocalFree(p); return result

env = dict(os.environ); env.update(TEMP=str(TMP), TMP=str(TMP), VSLANG='1033')
setup = subprocess.run(['cmd.exe', '/u', '/d', '/c', r'call C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && set'],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding='utf-16-le', errors='replace', env=env, check=True)
for line in setup.stdout.splitlines():
    if '=' in line:
        k, v = line.split('=', 1)
        if k and not k.startswith('='): env[k] = v
env.update(TEMP=str(TMP), TMP=str(TMP))

def run(command, label, cwd=TMP):
    commands.append(command)
    with (LOG / (label + '.log')).open('wb') as stream:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                timeout=min(300, 890 - (time.monotonic() - began)))
    print(label, result.returncode, flush=True)
    if result.returncode:
        print((LOG / (label + '.log')).read_text(encoding='utf8', errors='replace')[-7000:]); raise SystemExit(result.returncode)

prior = json.loads(protect(PRIOR).read_text(encoding='utf8'))
link = next(c for c in prior['commands'] if 'vs_link_exe' in c and any(v.endswith('veyra_qml_ui.exe') for v in c))
cmake = str(protect(link[0]))
ninja = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.*)$', (CACHE / 'CMakeCache.txt').read_text(encoding='utf8'), re.M).group(1)
protect(ninja)
conf = OUT / 'cmake'
if sys.argv[1:] != ['--probe']:
    configure = [cmake, '-S', str(ROOT), '-B', str(conf), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MAKE_PROGRAM=' + ninja,
        '-DCMAKE_PREFIX_PATH=C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64;C:/veyra-deps/xbox-installed/x64-windows-static',
        '-DVEYRA_BUILD_QML_UI=ON', '-DVEYRA_BUILD_QT_PROBE=OFF', '-DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON',
        # FFmpeg's installed FindFFMPEG.cmake cannot resolve its non-ASCII prefix.
        # Use the same 9.0.1 import/header ABI as the accepted build. The private
        # runtime comes from the already verified zlib/PS5-slices256 package.
        '-DVEYRA_FFMPEG_ROOT=C:/veyra-deps/ffmpeg-ps5-dav1d-installed',
        '-DVEYRA_LIBASS_ROOT=E:/veyra-ascii-20261005/installed/x64-windows-static',
        '-DVEYRA_DECKLINK_SDK_DIR=E:/项目/Veyra/deps/decklink-sdk-obs31-20261009',
        '-DVEYRA_MAGEWELL_SDK_DIR=E:/项目/Veyra/deps/magewell/3.3.1.1596']
    for option in ('REMOTEPLAY', 'MOONLIGHT', 'XBOX', 'DLSS_FG', 'DLSS_SR', 'NVOF'):
        configure.append('-DVEYRA_ENABLE_' + option + '=OFF')
    sdk = Path('C:/Users/123/Desktop/Veyra DLSS Video Player/third_party_local')
    for key, path in {'DLSS_SDK_ROOT':'nvidia/DLSS_SDK_310.7.0', 'NVOF_SDK_ROOT':'nvidia/Optical_Flow_SDK_5.0.7',
                      'NVENC_HEADERS_ROOT':'nvidia/nv-codec-headers-13.0', 'XESS_ROOT':'intel/xess-3.0.2'}.items():
        configure.append('-DVEYRA_' + key + '=' + (sdk / path).as_posix())
    run(configure, 'configure-real-project')
old = args(subprocess.check_output([ninja, '-t', 'commands', 'CMakeFiles/veyra_qml_ui.dir/apps/veyra-qml/main.cpp.obj'], cwd=CACHE, text=True, encoding='utf8').splitlines()[-1])
new = args(subprocess.check_output([ninja, '-t', 'commands', 'CMakeFiles/veyra_qml_ui.dir/apps/veyra-qml/main.cpp.obj'], cwd=conf, text=True, encoding='utf8').splitlines()[-1])
privateIncludes = [v for v in new if v.startswith(('-I', '-external:I')) and '/6.8.3' in v.replace('\\', '/')]
assert not any('QtQuick/6.8.3' in v.replace('\\', '/') for v in privateIncludes), 'withdrawn QuickPrivate dependency remains'
source = protect(ROOT / 'apps/veyra-qml/main.cpp')
name = 'veyra_qml_ui'
if sys.argv[1:] == ['--probe']:
    probe = protect(ROOT / 'tests/qml/SoftwareUiDamageProbe.h')
    text = source.read_text(encoding='utf8')
    text = text.replace('#include "../veyra/resource.h"', '#include "' + (ROOT / 'apps/veyra/resource.h').as_posix() + '"')
    text = '#include "' + probe.as_posix() + '"\n' + text
    needle = '    if (testSize.isValid()) window->resize(testSize);'
    assert text.count(needle) == 1
    text = text.replace(needle, needle + '\n    installSoftwareUiDamageProbe(window, &bridge);')
    source = OUT / 'probe-main.cpp'; source.write_text(text, encoding='utf8'); protect(source)
    name = 'veyra_ui_damage_probe'
obj = OUT / (name + '.obj')
compile = old[:1] + ['/utf-8', '/I' + str(ROOT / 'include'), '/I' + str(ROOT / 'src')] + privateIncludes
compile += [v for v in old[1:-1] if not v.startswith(('/Fo', '/Fd')) and v != '/showIncludes']
compile += ['/Fo' + str(obj), '/Fd' + str(OUT / (name + '.compile.pdb')), str(source)]
run(compile, 'compile-main')
oldRsp = protect(next(v[1:] for v in link if v.startswith('@')))
text = oldRsp.read_text(encoding='utf8')
oldMain = prior['overrides']['CMakeFiles/veyra_qml_ui.dir/apps/veyra-qml/main.cpp.obj']['path']
assert text.count(oldMain) == 1
for item in args('link ' + text)[1:]:
    if Path(item).suffix.lower() in ('.obj', '.res', '.lib') and Path(item).is_file(): protect(item)
text = text.replace(oldMain, str(obj))
rsp = OUT / (name + '.rsp'); rsp.write_text(text, encoding='utf8')
manifest = OUT / ('manifest-' + name); manifest.mkdir(exist_ok=True)
command = []
for value in link:
    if value.startswith('--intdir='): value = '--intdir=' + str(manifest)
    elif value.startswith('@'): value = '@' + str(rsp)
    elif value.startswith(('/out:', '/implib:', '/pdb:')):
        prefix, path = value.split(':', 1); value = prefix + ':' + str(OUT / (name + Path(path).suffix))
    command.append(value)
run(command, 'link-main', CACHE)
for path, digest in inputs.items(): assert sha(path) == digest, 'input changed: ' + path
qt = Path('C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64/bin/Qt6Quick.dll')
assert b'QSG_SOFTWARE_RENDERER_FORCE_PARTIAL_UPDATES' not in qt.read_bytes(), 'recheck Qt variable support'
receipt = {'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
    'source': str(source), 'sourceSha256': sha(source), 'exe': str(OUT / (name + '.exe')), 'exeSha256': sha(OUT / (name + '.exe')),
    'privateIncludesFromCMake': privateIncludes, 'commands': commands, 'inputsUnchanged': inputs,
    'qmlTelemetrySha256': sha(ROOT / 'qml/Veyra/VTelemetryValue.qml'), 'qtQuickSha256': sha(qt),
    'seconds': time.monotonic() - began, 'probeOnly': name != 'veyra_qml_ui'}
(LOG / 'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf8')
(BASE / 'reports' / TASK / ('probe-build.json' if receipt['probeOnly'] else 'build.json')).write_text(json.dumps({'receipt': str(LOG / 'receipt.json')}, indent=2), encoding='utf8')
print(json.dumps({k: receipt[k] for k in ('exe', 'exeSha256', 'seconds', 'probeOnly')}, ensure_ascii=False))

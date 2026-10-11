"""Check the full merged CMake graph and build a private UI damage probe.

Reuses the just-tested product link inputs, never the pre-merge GUI objects.
No runtime/SDK is written and no user application or profile is touched.
"""
from pathlib import Path
import ctypes, hashlib, json, os, re, subprocess, time

ROOT = Path(__file__).resolve().parents[2]
BASE = Path('E:/项目/Veyra'); TASK = 'merge-claude-fixes-20261011'
OUT = BASE / 'build' / TASK; CACHE = BASE / 'build/vfg-next-20261007'
TMP = BASE / 'tmp' / TASK / 'configure'; TMP.mkdir(parents=True, exist_ok=True)
LOG = BASE / 'logs' / TASK / ('configure-' + time.strftime('%Y%m%d-%H%M%S'))
LOG.mkdir(parents=True, exist_ok=False)
pointer = json.loads((BASE / 'reports' / TASK / 'build.json').read_text('utf8'))
prior = json.loads(Path(pointer['receipt']).read_text('utf8'))
commands = []; inputs = {}; began = time.monotonic()

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
    a = [p[i] for i in range(n.value)]; ctypes.windll.kernel32.LocalFree(p); return a

env = dict(os.environ); env.update(TEMP=str(TMP), TMP=str(TMP), VSLANG='1033', PYTHONDONTWRITEBYTECODE='1')
setup = subprocess.run(['cmd.exe', '/u', '/d', '/c', r'call C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && set'],
    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding='utf-16-le', errors='replace', env=env, check=True)
for line in setup.stdout.splitlines():
    if '=' in line:
        k, v = line.split('=', 1)
        if k and not k.startswith('='): env[k] = v
env.update(TEMP=str(TMP), TMP=str(TMP), PYTHONDONTWRITEBYTECODE='1')

def run(command, label, cwd=TMP):
    commands.append(command)
    with (LOG / (label + '.log')).open('wb') as stream:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=290)
    print(label, result.returncode, flush=True)
    if result.returncode:
        print((LOG / (label + '.log')).read_text('utf8', errors='replace')[-7000:]); raise SystemExit(result.returncode)

link = next(c for c in prior['commands'] if 'vs_link_exe' in c and any(v.endswith('veyra_qml_ui.exe') for v in c))
cmake = str(protect(link[0]))
cache = (CACHE / 'CMakeCache.txt').read_text('utf8')
options = dict((key, value) for key, typ, value in re.findall(r'^(VEYRA_[^:]+|CMAKE_MAKE_PROGRAM|CMAKE_PREFIX_PATH):([^=]+)=(.*)$', cache, re.M) if typ != 'INTERNAL')
# These pinned tools are explicit in the accepted cache, outside the VEYRA prefix.
for key in ('PROTOC', 'PKG_CONFIG_EXECUTABLE', 'SDL3_DIR', 'LibDataChannel_DIR', 'Opus_DIR', 'nlohmann_json_DIR'):
    options[key] = re.search(r'^' + key + r':[^=]+=(.*)$', cache, re.M).group(1)
options.update(VEYRA_DECKLINK_SDK_DIR=str(BASE / 'deps/decklink-sdk-obs31-20261009'),
    VEYRA_AMD_NR_TEST_GRAPH_SOURCE=str(ROOT / 'src/pipeline/EnhanceGraph.cpp'), VEYRA_DISPLAY_VERSION=prior['displayVersion'])
for key in ('REMOTEPLAY', 'MOONLIGHT', 'XBOX'): assert options['VEYRA_ENABLE_' + key] == 'ON'
conf = OUT / 'cmake'
run([cmake, '-S', str(ROOT), '-B', str(conf), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release'] +
    ['-D' + key + '=' + value for key, value in options.items()], 'configure-full-product')
ninja = str(protect(options['CMAKE_MAKE_PROGRAM']))
graph = (conf / 'build.ninja').read_text('utf8')
assert 'src/engine/EngineWatchdog.cpp.obj' in graph.replace('\\', '/')
live = subprocess.check_output([ninja, '-t', 'commands', 'veyra_live_presentation_tests.exe'], cwd=conf, text=True, encoding='utf8')
assert '/STACK:8388608' in live
mainTarget = re.search(r'^build (CMakeFiles[\\/]veyra_qml_ui\.dir[\\/]apps[\\/]veyra-qml[\\/]main.cpp.obj):', graph, re.M).group(1)
main = subprocess.check_output([ninja, '-t', 'commands', mainTarget], cwd=conf, text=True, encoding='utf8').splitlines()[-1]
assert 'QtQuick/6.8.3' not in main.replace('\\', '/')  # No withdrawn QuickPrivate/full-redraw hook.
for feature in ('REMOTEPLAY', 'MOONLIGHT', 'XBOX'): assert '-DVEYRA_ENABLE_' + feature + '=1' in main
assert str(ROOT / 'apps/veyra-qml/main.cpp').replace('\\', '/') in main.replace('\\', '/')
(LOG / 'main-command.txt').write_text(main, 'utf8')
(LOG / 'live-commands.txt').write_text(live, 'utf8')

source = protect(ROOT / 'apps/veyra-qml/main.cpp'); probe = protect(ROOT / 'tests/qml/SoftwareUiDamageProbe.h')
text = source.read_text('utf8').replace('#include "../veyra/resource.h"', '#include "' + (ROOT / 'apps/veyra/resource.h').as_posix() + '"')
needle = '    if (testSize.isValid()) window->resize(testSize);'
assert text.count(needle) == 1
text = '#include "' + probe.as_posix() + '"\n' + text.replace(needle, needle + '\n    installSoftwareUiDamageProbe(window, &bridge);')
source = OUT / 'damage-probe-main.cpp'; source.write_text(text, 'utf8')
name = 'veyra_ui_damage_probe'; obj = OUT / (name + '.obj')
oldCompile = next(c for c in prior['commands'] if c[-1].replace('\\', '/').endswith('/apps/veyra-qml/main.cpp'))
compile = [v for v in oldCompile[:-1] if not v.startswith(('/Fo', '/Fd'))] + ['/Fo' + str(obj), '/Fd' + str(OUT / (name + '.compile.pdb')), str(source)]
run(compile, 'compile-private-main')
oldRsp = protect(next(v[1:] for v in link if v.startswith('@')))
text = oldRsp.read_text('utf8'); oldMain = prior['overrides']['CMakeFiles/veyra_qml_ui.dir/apps/veyra-qml/main.cpp.obj']['path']
assert text.count(oldMain) == 1
for p in args('link ' + text)[1:]:
    if Path(p).suffix.lower() in ('.obj', '.res', '.lib') and Path(p).is_file(): protect(p)
protect(OUT / 'veyra_qml_ui.exe')
rsp = OUT / (name + '.rsp'); rsp.write_text(text.replace(oldMain, str(obj)), 'utf8')
manifest = OUT / ('manifest-' + name); manifest.mkdir(exist_ok=True)
command = []
for v in link:
    if v.startswith('--intdir='): v = '--intdir=' + str(manifest)
    elif v.startswith('@'): v = '@' + str(rsp)
    elif v.startswith(('/out:', '/implib:', '/pdb:')):
        prefix, path = v.split(':', 1); v = prefix + ':' + str(OUT / (name + Path(path).suffix))
    command.append(v)
run(command, 'link-private-probe', CACHE)
for p, digest in inputs.items(): assert sha(p) == digest, p
receipt = {'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(), 'commands':commands,
    'configuredFeatures':options, 'quickPrivate':False, 'testStackBytes':8388608, 'inputsUnchanged':inputs,
    'productExeSha256':sha(OUT / 'veyra_qml_ui.exe'), 'probeExeSha256':sha(OUT / (name + '.exe')), 'seconds':time.monotonic()-began}
(LOG / 'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), 'utf8')
(BASE / 'reports' / TASK / 'configure.json').write_text(json.dumps({'receipt':str(LOG / 'receipt.json')}), 'utf8')
print('PASS full CMake + private probe', flush=True)

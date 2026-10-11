"""Build the merged Claude fixes against immutable accepted main objects.
All changed-header includers are recompiled; new watchdog is linked explicitly.
Actual full CMake configuration is independently checked before promotion.
"""
from pathlib import Path
import ctypes, hashlib, json, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

BASE = Path('E:/项目/Veyra')
NAME = 'merged'
TASK = 'merge-claude-fixes-20261011'
ROOT = BASE / 'worktrees' / TASK
BASE_COMMIT = '5ba186a91e868c28b4b5ec46c9166aa321d90998'
CACHE = BASE / 'build/vfg-next-20261007'; OUT = BASE / 'build' / TASK
SDK = BASE / 'deps/decklink-sdk-obs31-20261009'
LABEL = '2.0.6-fixes-20261010'
PREVIOUS = BASE / 'logs/sr-visual-quality-20261010/build-20261010-153535/receipt.json'
LOG = BASE / 'logs' / TASK / ('build-' + time.strftime('%Y%m%d-%H%M%S')); TMP = BASE / 'tmp' / TASK / 'build'
for p in (OUT, LOG, TMP): p.mkdir(parents=True, exist_ok=True)
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
shell = ctypes.WinDLL('shell32'); shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]; shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p); ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]
def args(s):
    n = ctypes.c_int(); p = shell.CommandLineToArgvW(s, ctypes.byref(n)); a = [p[i] for i in range(n.value)]; ctypes.windll.kernel32.LocalFree(p); return a
def unescape(s): return s.replace('$ ', ' ').replace('$:', ':').replace('$$', '$')
tool = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.*)$', (CACHE / 'CMakeCache.txt').read_text(encoding='utf8'), re.M).group(1)
ninja = (CACHE / 'build.ninja').read_text(encoding='utf8'); inputs = {}; sources = {}; commands = []; start = time.monotonic()
def protect(p):
    p = Path(p); p = p if p.is_absolute() else CACHE / p; assert p.is_file(), p
    if str(p) not in inputs: inputs[str(p)] = sha(p)
    return p
def rule(name):
    lines = re.search(r'^build ' + re.escape(name) + r': .*?(?=\n\n)', ninja, re.M | re.S).group().splitlines()
    var = dict(re.match(r'  ([A-Z_]+) = (.*)', l).groups() for l in lines[1:] if re.match(r'  ([A-Z_]+) = (.*)', l))
    objects = [unescape(n).replace('\\', '/') for n in lines[0].split('Release ', 1)[1].split(' | ', 1)[0].split(' || ', 1)[0].split(' ')]
    for value in args(unescape(var.get('LINK_LIBRARIES', ''))):
        if value.lower().endswith('.lib') and (Path(value).is_file() or (CACHE / value).is_file()): protect(value)
    return objects, var
def compiler(name):
    command = args(subprocess.check_output([tool, '-t', 'commands', name], cwd=CACHE, text=True, encoding='utf8').splitlines()[-1])
    return ['-DVEYRA_DISPLAY_VERSION="' + LABEL + '"' if v.startswith(('-DVEYRA_DISPLAY_VERSION=', '/DVEYRA_DISPLAY_VERSION=')) else v for v in command]
env = dict(os.environ); env.update(TEMP=str(TMP), TMP=str(TMP), VSLANG='1033')
setup = subprocess.run(['cmd.exe', '/u', '/d', '/c', r'call C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && set'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding='utf-16-le', errors='replace', env=env, check=True)
for line in setup.stdout.splitlines():
    if '=' in line:
        k, v = line.split('=', 1)
        if k and not k.startswith('='): env[k] = v
env.update(TEMP=str(TMP), TMP=str(TMP))
def run(command, name, cwd=TMP):
    commands.append(command); remaining = 890 - (time.monotonic() - start); assert remaining > 0
    with (LOG / (name + '.log')).open('wb') as stream: r = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=min(300, remaining))
    print(name, 'exit', r.returncode, flush=True)
    if r.returncode: print((LOG / (name + '.log')).read_text(encoding='utf8', errors='replace')[-6000:]); raise RuntimeError(name)

# Files changed on this branch (committed or not) relative to main.
def git(*a): return subprocess.check_output(['git', *a], cwd=ROOT, text=True, encoding='utf8')
changed = set(git('diff', '--name-only', BASE_COMMIT).split()) | set(git('ls-files', '--others', '--exclude-standard').split())
changed = {p.replace('\\', '/') for p in changed}
changed_headers = sorted(p for p in changed if p.endswith(('.h', '.hpp', '.inl')))
# Transitive includers of changed headers (by include-path suffix), independent of the old ninja deps.
include_re = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
files = [p for folder in ('include', 'src', 'apps', 'tests') for p in (ROOT / folder).rglob('*') if p.suffix in ('.h', '.hpp', '.inl', '.cpp', '.cc')]
includes = {p.relative_to(ROOT).as_posix(): include_re.findall(p.read_text(encoding='utf8', errors='replace')) for p in files}
affected = set(changed_headers); grew = True
while grew:
    grew = False
    for path, incs in includes.items():
        if path in affected: continue
        if any(any(h.endswith('/' + i.lstrip('./')) or h == i for h in affected) for i in incs):
            affected.add(path); grew = True
print('changed', sorted(changed), flush=True)
headerKey = hashlib.sha256(''.join(str(p) + sha(p) for folder in ('include', 'tests') for p in sorted((ROOT / folder).rglob('*.h'))).encode()).hexdigest()
def compile(source, name, a, extra=(), dependencyKey=''):
    sources[str(source)] = sha(source); target = OUT / (name + '.obj')
    command = a[:1] + ['/utf-8', '/I' + str(OUT), '/I' + str(ROOT / 'include'), '/I' + str(ROOT / 'src')] + [v.replace(str(CACHE).replace('\\', '/'), str(OUT).replace('\\', '/')) if v.startswith(('/DVEYRA_SHADER_DIR', '-DVEYRA_SHADER_DIR')) else v for v in a[1:-1] if not v.startswith(('/Fo', '/Fd')) and v != '/showIncludes']
    command += list(extra) + ['/Fo' + str(target), '/Fd' + str(OUT / (name + '.pdb')), str(source)]
    key = hashlib.sha256((json.dumps(command) + sources[str(source)] + headerKey + dependencyKey).encode()).hexdigest(); stamp = target.with_suffix('.build.json')
    if target.is_file() and stamp.is_file():
        old = json.loads(stamp.read_text())
        if old['key'] == key and old['object'] == sha(target): print('reuse', name, flush=True); return target
    run(command, 'compile-' + name); stamp.write_text(json.dumps({'key': key, 'object': sha(target)}), encoding='utf8'); return target
previous = json.loads(PREVIOUS.read_text(encoding='utf8'))
overrides = {name: protect(row['path']) for name, row in previous['overrides'].items()}
for name, row in previous['overrides'].items(): assert sha(row['path']) == row['sha256'], name
# The accepted build compiled these per run from the source tree; they are rebuilt below.
for generated in ('CMakeFiles/veyra_qml_ui.dir/.qt/rcc/qrc_veyra_i18n.cpp.obj', 'CMakeFiles/veyra_qml_ui.dir/veyra-qml-version.rc.res'):
    overrides.pop(generated, None)
targets = {n: rule(n + '.exe') for n in ('veyra_qml_ui', 'veyra_capture_tests', 'veyra_capture_color_tests', 'veyra_capture_format_selection_tests', 'veyra_hdr_color_tests', 'veyra_capture_audio_tests', 'veyra_qml_quick_tests', 'veyra_present_sink_lifecycle_tests', 'veyra_live_presentation_tests', 'veyra_nr_video_quality_probe', 'veyra_effect_chain_tests', 'veyra_repair_contract_tests', 'veyra_live_timing_tests', 'veyra_audio_timeline_tests', 'veyra_audio_track_output_tests', 'veyra_audio_playback_rate_tests') + ('veyra_vfg_gpu_tests',)}
objects = set(p for obj, var in targets.values() for p in obj)
def libraries(var): return {Path(v).stem for v in args(unescape(var.get('LINK_LIBRARIES', ''))) if v.endswith('.lib') and Path(v).stem.startswith('veyra_')}
groups = set().union(*(libraries(var) for objs, var in targets.values()))
for group in groups:
    if re.search(r'^build ' + re.escape(group + '.lib') + r': (?:CXX|C)_STATIC_', ninja, re.M): objects.update(rule(group + '.lib')[0])
dependencies = {}; current = None
for line in subprocess.check_output([tool, '-t', 'deps'], cwd=CACHE, text=True, encoding='utf8').splitlines():
    if line and not line[0].isspace(): current = line.split(': #deps ', 1)[0].replace('\\', '/'); dependencies[current] = []
    elif current and line.strip(): dependencies[current].append(line.strip().replace('\\', '/'))
rebuild = []
for obj in objects:
    relative = obj.split('.dir/', 1)[-1][:-4]
    if not relative.startswith(('src/', 'apps/', 'tests/')) or not (ROOT / relative).is_file(): continue
    dep_changed = any(any(d.endswith('/' + h) for d in dependencies.get(obj, ())) for h in changed_headers)
    # Test objects not linked by the accepted build may predate main: always rebuild them.
    forced = obj.startswith('CMakeFiles/veyra_vfg_gpu_tests.dir/tests/')
    if forced or relative in changed or relative in affected or dep_changed or (obj not in overrides and not (CACHE / obj).is_file()):
        rebuild.append((obj, ROOT / relative, compiler(obj)))
print('rebuild', sorted(o for o, _, _ in rebuild), flush=True)
moc = protect(Path('C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64/bin/moc.exe'))
run([str(moc), str(ROOT / 'tests/qml/QuickSmokeTests.cpp'), '-o', str(OUT / 'QuickSmokeTests.moc')], 'generate-quick-test-moc')
def job(row):
    obj, source, a = row; return obj, compile(source, obj.replace('/', '-').replace('.', '-'), a)
# Sources added on this branch (not in the cached CMake graph): same flags as a
# neighbour in the same static library, linked wherever that library is.
added = {'CMakeFiles/veyra_engine.dir/src/engine/EngineWatchdog.cpp.obj': ('src/engine/EngineWatchdog.cpp', 'CMakeFiles/veyra_engine.dir/src/engine/EngineController.cpp.obj')}
rebuild += [(obj, ROOT / source, compiler(neighbour)) for obj, (source, neighbour) in added.items()]
with ThreadPoolExecutor(max_workers=8) as pool:
    for obj, p in pool.map(job, rebuild): overrides[obj] = p
quickAutogen = 'CMakeFiles/veyra_qml_quick_tests.dir/veyra_qml_quick_tests_autogen/mocs_compilation.cpp.obj'
emptyMoc = OUT / 'quick-mocs.cpp'; emptyMoc.write_text('// Setup moc is included explicitly by QuickSmokeTests.cpp.\n', encoding='utf8')
overrides[quickAutogen] = compile(emptyMoc, 'quick-mocs', compiler(quickAutogen))
# Bridge meta-object group (QmlPlayerBridge.h is a Q_OBJECT header).
if 'include/veyra/ui/QmlPlayerBridge.h' in changed:
    autogen = json.loads(protect(CACHE / 'CMakeFiles/veyra_qml_bridge_autogen.dir/AutogenInfo.json').read_text(encoding='utf8'))
    mocArgs = [autogen['QT_MOC_EXECUTABLE']] + ['-D' + d for d in autogen['MOC_DEFINITIONS']] + ['-I' + str(ROOT / 'include')] + ['-I' + d for d in autogen['MOC_INCLUDES']]
    run(mocArgs + ['-o', str(OUT / 'moc_QmlPlayerBridge.cpp'), str(ROOT / 'include/veyra/ui/QmlPlayerBridge.h')], 'moc-QmlPlayerBridge')
    mocs = OUT / 'bridge_mocs_compilation.cpp'
    retained = [protect(CACHE / f'veyra_qml_bridge_autogen/EC3QK4C7QJ/moc_{n}.cpp') for n in ('MoonlightInputCapture', 'QmlExportQueueModel')]
    mocs.write_text(''.join('#include "' + p.as_posix() + '"\n' for p in retained) + '#include "moc_QmlPlayerBridge.cpp"\n', encoding='utf8')
    overrides['CMakeFiles/veyra_qml_bridge.dir/veyra_qml_bridge_autogen/mocs_compilation.cpp.obj'] = compile(mocs, 'bridge-mocs', compiler('CMakeFiles/veyra_qml_bridge.dir/src/ui/QmlPlayerBridge.cpp.obj'), dependencyKey=sha(OUT / 'moc_QmlPlayerBridge.cpp'))
for obj in objects:
    if obj not in overrides: protect(obj)
# Unchanged objects added by PR31, not present in the older cached CMake graph.
assert not ({'include/veyra/ui/PlaylistModel.h', 'src/ui/PlaylistModel.cpp'} & changed)
playlist = [protect(BASE / 'build/pr31-playlist-20261009' / n) for n in ('PlaylistModel.obj', 'playlist-moc.obj')]
sourceCompiler = compiler('CMakeFiles/veyra_sources.dir/src/source/CaptureCardSource.cpp.obj')
generatedSdk = OUT / 'decklink-generated'; generatedSdk.mkdir(exist_ok=True)
for file in SDK.rglob('*'):
    if file.is_file(): protect(file)
midl = protect(Path(r'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/midl.exe'))
run([str(midl), '/nologo', '/env', 'x64', '/I', str(SDK), '/out', str(generatedSdk), '/h', 'DeckLinkAPI_h.h',
     '/iid', 'DeckLinkAPI_i.c', '/tlb', 'DeckLinkAPI.tlb', '/client', 'none', '/server', 'none', str(SDK / 'DeckLinkAPI.idl')], 'generate-decklink-abi')
nativeCore = compile(ROOT / 'src/source/DeckLinkCapture.cpp', 'DeckLinkCapture', sourceCompiler)
nativeDriver = compile(ROOT / 'src/source/DeckLinkDriver.cpp', 'DeckLinkDriver', sourceCompiler, ['/DVEYRA_HAS_DECKLINK=1', '/I' + str(generatedSdk)], sha(generatedSdk / 'DeckLinkAPI_h.h'))
stubDriver = compile(ROOT / 'src/source/DeckLinkDriver.cpp', 'DeckLinkDriver-stub', sourceCompiler, ['/DVEYRA_HAS_DECKLINK=0'])
native = [nativeCore, nativeDriver]
uiOnly = sys.argv[1:] == ['ui-only']
shaderOut = OUT / 'shaders'; shaderOut.mkdir(exist_ok=True)
compiledShaders = {'PackedCaptureToLinear'} | {Path(p).stem for p in changed if p.startswith('shaders/') and p.endswith('.hlsl')}
for source in (CACHE / 'shaders').rglob('*'):
    if not source.is_file() or (source.suffix == '.dxil' and source.stem in compiledShaders): continue
    protect(source); target = shaderOut / source.relative_to(CACHE / 'shaders'); target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists(): os.link(source, target)
assert not [p for p in changed if p.startswith('shaders/') and not p.endswith('.hlsl')], 'shared shader include changes need every includer rebuilt'
dxc = protect(Path(r'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe'))
for stem in sorted(compiledShaders):
    target = shaderOut / (stem + '.dxil')
    if target.exists(): target.unlink()
    run([str(dxc), '-T', 'cs_6_0', '-E', 'main', '-O3', '-Qstrip_debug', '-Qstrip_reflect', '-Fo', str(target), str(ROOT / 'shaders' / (stem + '.hlsl'))], 'shader-' + stem)
resource = 'CMakeFiles/veyra_qml_ui.dir/.qt/rcc/qrc_veyra_i18n.cpp.obj'
rcc = protect(Path('C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64/bin/rcc.exe'))
qrc = OUT / 'i18n.qrc'; qrc.write_text('<RCC><qresource prefix="/i18n"><file alias="catalog.json">' + (ROOT / 'i18n/catalog.json').as_posix() + '</file></qresource></RCC>', encoding='utf8')
generated = OUT / 'qrc_veyra_i18n.cpp'; run([str(rcc), '--name', 'veyra_i18n', '--no-zstd', '-o', str(generated), str(qrc)], 'generate-i18n')
overrides[resource] = compile(generated, 'qrc_veyra_i18n', compiler(resource), dependencyKey=sha(ROOT / 'i18n/catalog.json')); sources[str(ROOT / 'i18n/catalog.json')] = sha(ROOT / 'i18n/catalog.json')
versionSource = ROOT / 'apps/veyra/version.rc.in'
version = versionSource.read_text(encoding='utf8')
for key, value in {'PROJECT_VERSION_MAJOR': '2', 'PROJECT_VERSION_MINOR': '0', 'PROJECT_VERSION_PATCH': '6',
                   'VEYRA_DISPLAY_VERSION': LABEL, 'VEYRA_RESOURCE_FILENAME': 'veyra_qml_ui.exe'}.items():
    version = version.replace('@' + key + '@', value)
assert '@' not in version
versionRc = OUT / 'veyra-qml-version.rc'; versionRc.write_text(version, encoding='utf8')
(OUT / 'veyra-resource.h').write_bytes((ROOT / 'apps/veyra/resource.h').read_bytes())
if not (OUT / 'veyra.ico').exists(): os.link(protect(ROOT / 'assets/veyra.ico'), OUT / 'veyra.ico')
rc = protect(Path(r'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe'))
versionRes = OUT / 'veyra-qml-version.rc.res'
run([str(rc), '/nologo', '/c65001', '/fo', str(versionRes), str(versionRc)], 'compile-version-resource', OUT)
overrides['CMakeFiles/veyra_qml_ui.dir/veyra-qml-version.rc.res'] = versionRes
cmake = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
link = r'C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Tools\MSVC\1444~1.352\bin\Hostx64\x64\link.exe'; executables = []
def linkTarget(name, objs, var):
    rsp = OUT / (name + '.rsp'); rsp.write_text('\n'.join(subprocess.list2cmdline([str(p)]) for p in objs) + '\n' + unescape(var.get('LINK_PATH', '')) + '\n' + unescape(var['LINK_LIBRARIES']), encoding='utf8')
    directory = OUT / ('manifest-' + name); directory.mkdir(exist_ok=True)
    flags = [('/MANIFESTINPUT:' + str(CACHE / 'veyra-qml.manifest')) if a.startswith('/MANIFESTINPUT:') else a for a in args(unescape(var.get('LINK_FLAGS', '')))]
    # Test builds only: public-symbol PDB for the app so [engine-hang] frames resolve
    # (symbolize.py). Release defaults /OPT:REF,ICF are kept explicitly.
    if name == 'veyra_qml_ui': flags += ['/DEBUG:FULL', '/OPT:REF', '/OPT:ICF']
    # wmain of the replay test holds many PlayerSnapshot locals (CMakeLists: same option).
    if name == 'veyra_live_presentation_tests': flags += ['/STACK:8388608']
    command = [cmake, '-E', 'vs_link_exe', '--msvc-ver=1944', '--intdir=' + str(directory), r'--rc=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\rc.exe', r'--mt=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\mt.exe', '--manifests', '--', link, '/nologo', '@' + str(rsp), '/out:' + str(OUT / (name + '.exe')), '/implib:' + str(OUT / (name + '.lib')), '/pdb:' + str(OUT / (name + '.pdb')), '/version:0.0'] + flags
    run(command, 'link-' + name, CACHE); executables.append(OUT / (name + '.exe'))
# Test targets linked from the static sink library only need the changed sink object;
# the other recompiled sink objects (encoders) reference the GPU graph.
selective = {'veyra_capture_audio_tests': '/src/sink/CaptureAudioSession.cpp.obj'}
selective.update({n: '/src/sink/WasapiAudioSink.cpp.obj' for n in ('veyra_audio_timeline_tests', 'veyra_audio_track_output_tests', 'veyra_audio_playback_rate_tests')})
for name, (objs, var) in targets.items():
    if uiOnly and name != 'veyra_qml_ui': continue
    selected = [overrides.get(obj, CACHE / obj) for obj in objs]
    linked = libraries(var)
    selected += [p for obj, p in overrides.items() if obj not in objs and any(obj.startswith('CMakeFiles/' + group + '.dir/') for group in linked) and
                 (name not in selective or obj.endswith(selective[name]))]
    if name == 'veyra_qml_ui': selected += playlist
    if 'veyra_sources' in linked: selected += native
    linkTarget(name, selected, var)
if not uiOnly:
    objects, var = targets['veyra_live_presentation_tests']
    linked = libraries(var)
    modules = [p for obj, p in overrides.items() if any(obj.startswith('CMakeFiles/' + group + '.dir/') for group in linked)]
    for name, source in (('veyra_settings_transaction_tests', 'tests/integration/SettingsTransactionTests.cpp'), ('veyra_sr_quality_tests', 'tests/integration/SrQualityTests.cpp')):
        test = compile(ROOT / source, name, compiler(objects[0]))
        linkTarget(name, [test] + modules + native, var)
for p, d in inputs.items(): assert sha(p) == d, p
for folder in ('include', 'src', 'shaders', 'qml', 'tests'):
    for file in (ROOT / folder).rglob('*'):
        if file.is_file() and file.suffix in ('.h', '.hlsl', '.hlsli', '.qml', '.cpp'): sources[str(file)] = sha(file)
head = git('rev-parse', 'HEAD').strip(); dirty = git('status', '--porcelain').strip()
receipt = {'task': TASK, 'head': head, 'dirty': dirty.splitlines(), 'changed': sorted(changed), 'commands': commands, 'displayVersion': LABEL, 'sources': sources,
           'overrides': {n: {'path': str(p), 'sha256': sha(p)} for n, p in overrides.items()}, 'inputsUnchanged': inputs, 'seconds': time.monotonic() - start,
           'executables': {p.name: sha(p) for p in executables}, 'recompiledDependencyClosure': len(rebuild)}
(LOG / 'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf8')
print('built', TASK, 'head', head[:8], 'dirty', len(receipt['dirty']), 'rebuilt', len(rebuild), 'seconds', round(receipt['seconds'], 1), LOG, flush=True)

(BASE / 'reports' / TASK / 'build.json').write_text(json.dumps({'receipt':str(LOG / 'receipt.json')}), encoding='utf8')

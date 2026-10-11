"""Serial integration regressions for the merged Claude fixes; private runtime and profile.
Usage: py -3.11 -I merge-claude-fixes-test.py case ...
"""
from pathlib import Path
import hashlib, json, os, re, shutil, subprocess, sys, time

BASE = Path('E:/项目/Veyra'); TASK = 'merge-claude-fixes-20261011'
NAME = 'merged'
ROOT = BASE / 'worktrees' / TASK
DONOR = BASE / 'test-packages/fixes-20261010/Veyra-2.0.6-fixes-20261010-NVIDIA-win64-portable'
BUILD = BASE / 'build' / TASK; APP = BASE / 'tests' / TASK / ('regress-' + NAME)
QT = Path('C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64')
FFMPEG = BASE / 'deps/ffmpeg-subtitle-formats-20261009/installed/bin'
GTA4K = 'E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4'
cases = sys.argv[1:]; assert cases
manifest = json.loads((DONOR / 'package-manifest.json').read_text(encoding='utf8'))


def sha(p):
    with open(p, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def verify():
    for row in manifest['files']:
        assert sha(DONOR / row['path']) == row['sha256'], row['path']


def replace(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    swap = target.with_name(target.name + '.swap'); shutil.copy2(source, swap); os.replace(swap, target)


verify()
running = [l for l in subprocess.check_output(['tasklist', '/fo', 'csv', '/nh'], text=True, errors='replace').splitlines() if 'veyra' in l.lower()]
assert not running, running
if not APP.exists():
    APP.mkdir(parents=True)
    for row in manifest['files']:
        rel = Path(row['path'])
        if rel.parts[0] in ('runtime_local', 'logs') or rel.name == 'veyra_qml_ui.exe':
            continue
        target = APP / rel; target.parent.mkdir(parents=True, exist_ok=True); os.link(DONOR / rel, target)
    (APP / 'runtime_local').mkdir(exist_ok=True)
for source in BUILD.glob('*.exe'):
    replace(source, APP / source.name)
for source in (ROOT / 'qml').rglob('*'):
    if source.is_file() and (source.suffix in ('.qml', '.js') or source.name == 'qmldir'):
        replace(source, APP / 'qml' / source.relative_to(ROOT / 'qml'))
for source in (BUILD / 'shaders').glob('*.dxil'):
    replace(source, APP / 'shaders' / source.name)
for source in FFMPEG.glob('*.dll'):
    target = APP / source.name; swap = target.with_name(target.name + '.swap'); os.link(source, swap); os.replace(swap, target)
for source in (DONOR / 'runtime_local/amd/fidelityfx').glob('*.dll'):
    target = APP / 'runtime_local/amd/fidelityfx' / source.name
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True); os.link(source, target)
for source in (ROOT / 'tests/qml/quick').glob('*.qml'):
    replace(source, APP / 'qml-tests' / source.name)
log = BASE / 'logs' / TASK / NAME / ('tests-' + time.strftime('%Y%m%d-%H%M%S')); log.mkdir(parents=True, exist_ok=False)
results = []
for case in cases:
    output = log / case; output.mkdir(); temporary = BASE / 'tmp' / TASK / NAME / log.name / case; temporary.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('VEYRA_', 'QT_', 'QSG_'))}
    env.update(TEMP=str(temporary), TMP=str(temporary), CUDA_CACHE_PATH=str(temporary / 'cuda'), QML_DISK_CACHE_PATH=str(temporary / 'qml'),
               PATH=str(APP) + os.pathsep + str(QT / 'bin') + os.pathsep + env['PATH'], QML_IMPORT_PATH=str(QT / 'qml'),
               QT_FORCE_STDERR_LOGGING='1', VEYRA_EXPORT_TEST_ARTIFACTS=str(output))
    if case in ('slider', 'menu', 'components'):
        file = {'slider': 'tst_slider_value.qml', 'menu': 'tst_menu_layout.qml', 'components': 'tst_components.qml'}[case]
        env['QSG_RHI_BACKEND'] = 'd3d12'
        command = [str(APP / 'veyra_qml_quick_tests.exe'), '-input', str(APP / 'qml-tests' / file), '-o', '-,txt']
    elif case == 'sr':
        command = [str(APP / 'veyra_sr_quality_tests.exe'), str(output)]
    elif case == 'inactive':
        command = [str(APP / 'veyra_live_presentation_tests.exe'), GTA4K, str(output), '--inactive-effects']
    elif case == 'inactive-node':
        command = [str(APP / 'veyra_live_presentation_tests.exe'), GTA4K, str(output), '--inactive-effects-node']
    elif case == 'chain':
        command = [str(APP / 'veyra_effect_chain_tests.exe')]
    elif case == 'contracts':
        command = [str(APP / 'veyra_repair_contract_tests.exe')]
    elif case == 'video':
        command = [str(APP / 'veyra_live_presentation_tests.exe'), GTA4K, str(output), '--settings-rebuild']
    elif case == 'rollback':
        command = [str(APP / 'veyra_live_presentation_tests.exe'), GTA4K, str(output), '--reset-rollback']
    elif case == 'presenter':
        command = [str(APP / 'veyra_settings_transaction_tests.exe'), str(output)]
    elif case == 'lifecycle':
        command = [str(APP / 'veyra_present_sink_lifecycle_tests.exe'), str(output)]
    elif case == 'hdr':
        command = [str(APP / 'veyra_hdr_color_tests.exe')]
    elif case == 'timing':
        command = [str(APP / 'veyra_live_timing_tests.exe')]
    elif case == 'half-rate':
        command = [str(APP / 'veyra_live_presentation_tests.exe'), str(BASE / 'tests/claude-dedup-20261011/media/gta6-true60.mp4'), str(output), '--half-rate']
    elif case == 'vfg-gpu':
        command = [str(APP / 'veyra_vfg_gpu_tests.exe'), str(APP / 'runtime/nvidia-vfg'), '1280', '720', 'async']
    elif case == 'capture-color':
        command = [str(APP / 'veyra_capture_color_tests.exe')]
    elif case.startswith('audio') and case != 'audio-rate':
        flag = {'audio':None,'audio-jitter':'--jitter','audio-underrate':'--underrate',
                'audio-startup-seek':'--startup-seek','audio-stall':'--read-stall','audio-stall-direct':'--read-stall-direct'}[case]
        command = [str(APP / 'veyra_audio_timeline_tests.exe'), str(output)] + ([flag] if flag else [])
    elif case == 'audio-rate':
        import wave, math, struct
        fixture = output / '440hz.wav'
        with wave.open(str(fixture), 'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
            w.writeframes(b''.join(struct.pack('<h',int(12000*math.sin(2*math.pi*440*i/48000))) for i in range(240000)))
        command = [str(APP / 'veyra_audio_playback_rate_tests.exe'), str(fixture)]
    elif case in ('seek-stress','file-endpoint'):
        command = [str(APP / 'veyra_live_presentation_tests.exe'), GTA4K, str(output), '--'+case]
    else:
        raise AssertionError(case)
    start = time.monotonic()
    with (output / 'console.log').open('wb') as stream:
        try:
            run = subprocess.run(command, cwd=APP, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=290); code = run.returncode
        except subprocess.TimeoutExpired:
            code = -1
    text = (output / 'console.log').read_text(encoding='utf8', errors='replace')
    errors = [line for line in text.splitlines() if re.search(r'^FAIL |FAIL!|passed=0|ReferenceError:|TypeError:|Binding loop|Cannot assign|D3D12 ERROR|D3D12 CORRUPTION', line)]
    passed = code == 0 and not errors
    row = {'case': case, 'exit': code, 'seconds': round(time.monotonic() - start, 1), 'passed': passed, 'errors': errors[:10]}
    results.append(row); print(json.dumps(row, ensure_ascii=False), flush=True)
verify()
receipt = {'task': NAME, 'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(), 'results': results,
           'passed': all(r['passed'] for r in results), 'donorUnchanged': len(manifest['files']),
           'executables': {p.name: sha(p) for p in APP.glob('*.exe')}}
(log / 'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf8')
(BASE / 'reports' / TASK / ('tests-' + log.name + '.json')).write_text(json.dumps({'receipt':str(log / 'receipt.json')}), encoding='utf8')
print('passed' if receipt['passed'] else 'FAILED', log)
raise SystemExit(not receipt['passed'])

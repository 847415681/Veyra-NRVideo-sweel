"""Serial private GUI tests. Never opens or edits the user's installed app/profile."""
from pathlib import Path
import ctypes, hashlib, json, os, re, shutil, statistics, subprocess, sys, time
from ctypes import wintypes
import psutil
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
BASE = Path('E:/项目/Veyra'); TASK = 'ui-smear-handoff-20261011'
TEST = BASE / 'tests' / TASK; APP = TEST / 'app'
BUILD = BASE / 'build' / TASK
DONOR = BASE / 'test-packages/fixes-20261010/Veyra-2.0.6-fixes-20261010-NVIDIA-win64-portable'
LOG = BASE / 'logs' / TASK
MEDIA = Path('E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4')

def sha(p):
    with Path(p).open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()

def replace(source, target):
    swap = target.with_name(target.name + '.swap'); shutil.copy2(source, swap); os.replace(swap, target)

def write(target, data):
    swap = target.with_name(target.name + '.swap'); swap.write_bytes(data); os.replace(swap, target)

def verify_donor():
    rows = json.loads((DONOR / 'package-manifest.json').read_text(encoding='utf8'))['files']
    for row in rows: assert sha(DONOR / row['path']) == row['sha256'], row['path']
    return len(rows)

def no_player():
    for proc in psutil.process_iter(['name', 'exe']):
        assert not (proc.info['name'] or '').lower().startswith(('veyra', 'fg-presentation', 'flash-pixel')), proc.info

def stage():
    if not APP.exists():
        shutil.copytree(DONOR, APP, copy_function=os.link)
    for n in ('veyra_qml_ui.exe', 'veyra_ui_damage_probe.exe'):
        replace(BUILD / n, APP / n)

def qml_variant(old):
    for file in ('VIcon.qml', 'ProPage.qml'):
        replace((DONOR if old else ROOT) / 'qml/Veyra' / file, APP / 'qml/Veyra' / file)

def environment(name):
    tmp = BASE / 'tmp' / TASK / name; tmp.mkdir(parents=True, exist_ok=True)
    env = {k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_', 'QT_', 'QSG_'))}
    env.update(PATH=str(APP) + os.pathsep + env['PATH'], TEMP=str(tmp), TMP=str(tmp),
        CUDA_CACHE_PATH=str(tmp / 'cuda'), QML_DISK_CACHE_PATH=str(tmp / 'qml'), QML_DISABLE_DISK_CACHE='1',
        QT_FORCE_STDERR_LOGGING='1', QT_SCALE_FACTOR='1', VEYRA_TEST_IGNORE_RTSS='1')
    return env

def profile(name, mode, reduced):
    folder = TEST / 'profiles' / name; folder.mkdir(parents=True, exist_ok=False)
    prefs = {'language':'en', 'autoResume':False, 'rememberPosition':False, 'prewarmEnhancement':False,
        'gpuPriority':'normal', 'overlayCompat':'auto' if mode == 'rtss' else 'off',
        'obsGameCapture':mode == 'obs', 'reducedMotion':reduced, 'subtitleEnabled':False, 'sentinel':name}
    (folder / 'qml-preferences.v1.json').write_text(json.dumps(prefs), encoding='utf8')
    return folder

def errors(text):
    return [line for line in text.splitlines() if re.search(r'REGRESSION_FAIL|ReferenceError:|TypeError:|\[ERROR\s*\]|\[FATAL\s*\]|D3D12 ERROR|D3D12 CORRUPTION|Binding loop|Error decoding|Cannot open:|Failed to get image', line)]

def pixels(detailed=False):
    rows = []
    for name, mode, partial in [('pixels-old-obs', 'obs', True), ('pixels-fixed-obs', 'obs', False), ('pixels-fixed-rtss', 'rtss', False)]:
        if detailed: name += '-raster2'
        no_player(); out = LOG / name; out.mkdir(parents=True, exist_ok=False)
        env = environment(name); env.update(VEYRA_TEST_DAMAGE_EVIDENCE=str(out), VEYRA_LOG_FILE=str(out / 'player.log'))
        if detailed: env['QT_SCALE_FACTOR'] = '1.5'
        qml_variant(partial)
        command = [str(APP / 'veyra_ui_damage_probe.exe'), '--data-dir', str(profile(name, mode, True)), '--page', 'pro', '--size', '1280x800', '--reduced-motion', '--exit-after', '15000']
        if mode == 'rtss': command.append('--overlay-compat-restart')
        with (out / 'console.log').open('wb') as stream:
            run = subprocess.run(command, cwd=APP, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=25)
        text = (out / 'console.log').read_text(encoding='utf8', errors='replace')
        assert run.returncode == 0 and not errors(text), text[-6000:]
        probe = json.loads((out / 'probe.json').read_text(encoding='utf8'))
        assert probe['foundInspector'] and probe['overflow'] > 100 and probe['scrollSteps'] == 80, probe
        assert probe['scrolledSaved'] and probe['referenceSaved'], probe
        assert not probe['scrolledGrabTriggeredRender'] and not probe['referenceGrabTriggeredRender'], probe
        assert probe['firstLayerExpanded'] and probe['nrLayers'] == 3, probe
        assert abs(probe['scrolledContentY']-probe['referenceContentY']) < 0.1, 'reference changed scroll position'
        assert abs(probe['scrolledContentHeight']-probe['referenceContentHeight']) < 0.1, 'reference changed layout'
        a = np.asarray(Image.open(out / 'scrolled.png').convert('RGB')).astype(np.int16)
        b = np.asarray(Image.open(out / 'reference.png').convert('RGB')).astype(np.int16)
        initial = np.asarray(Image.open(out / 'initial.png').convert('RGB')).astype(np.int16)
        assert a.shape == b.shape
        delta = np.max(np.abs(a - b), axis=2)
        scale = a.shape[1] / 1280
        # Include inspector borders and fixed header/footer where scroll damage
        # leaks; exclude the unrelated system GPU-monitor status light.
        roi = delta[:, int((probe['inspectorX']-15)*scale):]
        top = a[int(60*scale):int(probe['inspectorY']*scale), int(probe['inspectorX']*scale):int((probe['inspectorX']+65)*scale)]
        outsideBlue = (top[:,:,2] > top[:,:,0]+25) & (top[:,:,2] > top[:,:,1]+20) & (top[:,:,2] > 80)
        headerDifference = np.max(np.abs(a-initial), axis=2)[int(60*scale):int(probe['inspectorY']*scale), int(probe['inspectorX']*scale):]
        diff = np.clip(np.abs(a - b) * 5, 0, 255).astype(np.uint8)
        Image.fromarray(diff).save(out / 'difference-x5.png')
        row = {'case':name, 'mode':mode, 'partial':partial, 'exitCode':run.returncode,
            'pixelsDifferentOver10':int((delta > 10).sum()), 'pixelFractionOver10':float((delta > 10).mean()),
            'inspectorAndEdgesPixelsOver10':int((roi > 10).sum()),
            'srBluePixelsAboveViewport':int(outsideBlue.sum()),
            'fixedHeaderPixelsChangedOver10':int((headerDifference > 10).sum()),
            'meanAbsChannelDifference':float(np.abs(a-b).mean()), 'probe':probe, 'command':command,
            'pathConfirmed': 'RTSS compatibility: software UI' in text if mode == 'rtss' else 'OBS game capture compatibility: software UI' in text}
        (out / 'result.json').write_text(json.dumps(row, ensure_ascii=False, indent=2), encoding='utf8')
        assert row['pathConfirmed'], text[-4000:]
        if not partial:
            assert probe['initialRasterIcons'] > 0 and probe['initialRasterIconsNotReady'] == 0, probe
            assert probe['scrolledRasterIconsNotReady'] == 0, probe
            assert row['inspectorAndEdgesPixelsOver10'] == 0 and row['fixedHeaderPixelsChangedOver10'] == 0, row
        rows.append(row); print(json.dumps(row, ensure_ascii=False), flush=True)
    (BASE / 'reports' / TASK / ('pixels-expanded.json' if detailed else 'pixels.json')).write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding='utf8')

def priority(proc):
    fn = ctypes.WinDLL('gdi32').D3DKMTGetProcessSchedulingPriorityClass
    fn.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_int)]; fn.restype = wintypes.LONG
    value = ctypes.c_int(-1); status = fn(int(proc._handle), ctypes.byref(value))
    return {'status':hex(status & 0xffffffff), 'value':value.value}

def clipping():
    # Reuse the already captured PRESENTED frames; no extra rendering or replay.
    rows = json.loads((BASE / 'reports' / TASK / 'pixels-expanded.json').read_text(encoding='utf8'))
    result = []
    for row in rows:
        out = LOG / row['case']; p = row['probe']
        first = np.asarray(Image.open(out / 'initial.png').convert('RGB')).astype(np.int16)
        full = np.asarray(Image.open(out / 'reference.png').convert('RGB')).astype(np.int16)
        scale = first.shape[1] / 1280
        header = np.max(np.abs(first-full), axis=2)[int(60*scale):int(p['inspectorY']*scale), int(p['inspectorX']*scale):]
        count = int((header > 10).sum())
        # Old software Shape paints into this fixed header even on FULL repaint.
        assert count > 0 if row['partial'] else count == 0, (row['case'], count)
        result.append({'case':row['case'], 'fixedHeaderPixelsChangedAfterFullRepaintOver10':count,
                       'initialSha256':sha(out / 'initial.png'), 'referenceSha256':sha(out / 'reference.png')})
    (BASE / 'reports' / TASK / 'clipping.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf8')
    print(json.dumps(result, ensure_ascii=False), flush=True)

def focus(proc):
    user = ctypes.WinDLL('user32'); windows = []
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def collect(hwnd, _):
        pid = wintypes.DWORD(); user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == proc.pid and user.IsWindowVisible(hwnd): windows.append(hwnd)
        return True
    user.EnumWindows(callback(collect), 0)
    user.SetForegroundWindow.argtypes = [wintypes.HWND]
    if windows: user.SetForegroundWindow(windows[0])

def performance(final=False, candidate_main=None, cases_override=None, report_name=None):
    results = []; main = APP / 'qml/Veyra/Main.qml'; saved = main.read_bytes()
    perfQml = APP / 'qml/Veyra/RegressionPerf.qml'
    replace(ROOT / 'scripts/acceptance/stability-export-priority-perf.qml', perfQml)
    try:
        cases = [('perf-gpu-old', 'gpu', False, True), ('perf-gpu-fixed', 'gpu', False, False),
                 ('perf-obs-old', 'obs', True, False), ('perf-obs-fixed', 'obs', False, False)]
        if final: cases = [('perf-final-gpu-old', 'gpu', False, True), ('perf-final-gpu-fixed', 'gpu', False, False),
                           ('perf-final-obs-old', 'obs', True, False), ('perf-final-obs-fixed', 'obs', False, False)]
        if cases_override is not None: cases = cases_override
        for name, mode, partial, old in cases:
            no_player(); out = LOG / name; out.mkdir(parents=True, exist_ok=False)
            replace(DONOR / 'veyra_qml_ui.exe' if old else BUILD / 'veyra_qml_ui.exe', APP / 'veyra_qml_ui.exe')
            qml_variant(partial or old)
            saved = (DONOR / 'qml/Veyra/Main.qml' if old or candidate_main is None else candidate_main).read_bytes()
            config = {'modern':True, 'seconds':40, 'media':MEDIA.as_posix(), 'display':2, 'reducedMotion':False, 'page':'pro', 'oldUi':False}
            text = saved.decode('utf8'); at = text.rfind('}')
            write(main, (text[:at] + '\n Loader { source:"RegressionPerf.qml"; onLoaded:item.config=' + json.dumps(config, ensure_ascii=True) + ' }\n' + text[at:]).encode('utf8'))
            env = environment(name); env.update(VEYRA_LOG_FILE=str(out / 'player.log'), VEYRA_VERBOSE_FRAME_LOGS='1')
            command = [str(APP / 'veyra_qml_ui.exe'), '--data-dir', str(profile(name, mode, False)), '--page', 'pro', '--size', '1280x800', '--exit-after', '95000']
            meters = []; began = time.monotonic()
            with (out / 'console.log').open('wb') as stream:
                proc = subprocess.Popen(command, cwd=APP, env=env, stdout=stream, stderr=subprocess.STDOUT)
                watched = psutil.Process(proc.pid)
                try:
                    while proc.poll() is None and time.monotonic() - began < 105:
                        elapsed = time.monotonic() - began
                        if elapsed > 2: focus(proc)
                        try:
                            cpu = watched.cpu_times()
                            meters.append({'elapsed':elapsed, 'cpu':cpu.user+cpu.system, 'priority':priority(proc)})
                        except psutil.NoSuchProcess: break
                        time.sleep(1)
                    if proc.poll() is None: proc.kill()
                    rc = proc.wait(timeout=8)
                finally:
                    if proc.poll() is None: proc.kill(); proc.wait(timeout=8)
            text = (out / 'player.log').read_text(encoding='utf8', errors='replace')
            samples = [json.loads(line.split('REGRESSION_SAMPLE ', 1)[1]) for line in text.splitlines() if 'REGRESSION_SAMPLE ' in line]
            samples = [r for r in samples if r['ready'] and 10 <= r['position'] <= 39]
            assert rc == 0 and 'REGRESSION_PASS' in text and not errors(text) and len(samples) >= 15, text[-6000:]
            timings = {}
            names = sorted({r['label'] for s in samples for r in s['stages'] if r['measured']})
            for stage in names:
                values = [r['ms'] for s in samples for r in s['stages'] if r['measured'] and r['label'] == stage]
                timings[stage] = {'medianOneSecondMeans':statistics.median(values), 'min':min(values), 'max':max(values), 'n':len(values)}
            stableCpu = [r for r in meters if 15 <= r['elapsed'] <= 40]
            uiRate = [b['uiFrames']-a['uiFrames'] for a,b in zip(samples, samples[1:])]
            row = {'case':name, 'mode':mode, 'partial':partial, 'originalExe':old, 'exeSha256':sha(APP / 'veyra_qml_ui.exe'),
                'command':command, 'seconds':time.monotonic()-began, 'exitCode':rc, 'timings':timings,
                'uiFramesPerSecondMedian':statistics.median(uiRate), 'submitFpsMedian':statistics.median(s['fps'] for s in samples),
                'activeSamples':sum(s['active'] for s in samples), 'samples':len(samples),
                'processCpuSecondsPerSecond':(stableCpu[-1]['cpu']-stableCpu[0]['cpu'])/(stableCpu[-1]['elapsed']-stableCpu[0]['elapsed']),
                'priorityReadings':meters, 'nrSha256':sha(APP / 'runtime/experimental/nr-original/nvngx_dlssnr.dll'),
                'telemetrySha256':sha(APP / 'qml/Veyra/VTelemetryValue.qml')}
            assert all(r['priority'] == {'status':'0x0', 'value':2} for r in meters if r['elapsed'] > 10), 'normal GPU priority changed'
            (out / 'result.json').write_text(json.dumps(row, ensure_ascii=False, indent=2), encoding='utf8')
            (out / 'samples.json').write_text(json.dumps(samples, ensure_ascii=False, indent=2), encoding='utf8')
            results.append(row); print(json.dumps({k:v for k,v in row.items() if k != 'priorityReadings'}, ensure_ascii=False), flush=True)
    finally:
        write(main, saved); replace(BUILD / 'veyra_qml_ui.exe', APP / 'veyra_qml_ui.exe'); qml_variant(False)
    (BASE / 'reports' / TASK / (report_name or ('performance-final.json' if final else 'performance.json'))).write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf8')

if __name__ == '__main__':
    assert sys.argv[1:] in (['pixels'], ['pixels-expanded'], ['clipping'], ['performance'], ['performance-final'])
    no_player(); count = verify_donor(); stage()
    if sys.argv[1] == 'clipping': clipping()
    elif sys.argv[1].startswith('pixels'): pixels('expanded' in sys.argv[1])
    else: performance('final' in sys.argv[1])
    assert verify_donor() == count
    print('Donor files unchanged:', count, flush=True)

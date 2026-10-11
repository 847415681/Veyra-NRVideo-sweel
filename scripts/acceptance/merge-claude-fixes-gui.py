"""Reuse the accepted UI pixel/performance harness against the merged product.

Run with Python 3.11 -B (user-site psutil, numpy and Pillow). GPU cases are serial.
"""
from pathlib import Path
import importlib.util, json, os, re, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[2]
BASE = Path('E:/项目/Veyra'); TASK = 'merge-claude-fixes-20261011'
spec = importlib.util.spec_from_file_location('damage_harness', ROOT / 'scripts/acceptance/ui-software-damage-test.py')
h = importlib.util.module_from_spec(spec); spec.loader.exec_module(h)
h.TASK = TASK; h.TEST = BASE / 'tests' / TASK; h.APP = h.TEST / 'regress-merged'
h.BUILD = BASE / 'build' / TASK; h.LOG = BASE / 'logs' / TASK

def watchdog():
    original = h.APP / 'qml/Veyra/Main.qml'; saved = original.read_bytes()
    driver = BASE / 'tests/claude-freeze-20261010/harness/ProbeAB.qml'
    target = h.APP / 'qml/Veyra/MergeWatchdogProbe.qml'; h.replace(driver, target)
    rows = []
    try:
        for name, stall, seconds in [('watchdog-recover',12000,22), ('watchdog-shutdown',-1,16)]:
            h.no_player(); out = h.LOG / name; out.mkdir(parents=True, exist_ok=False)
            prefs = h.profile(name, 'gpu', True)
            config = {'media':h.MEDIA.as_posix(), 'seconds':seconds, 'warmup':0, 'display':2, 'nr':0}
            text = saved.decode('utf8'); at = text.rfind('}')
            h.write(original, (text[:at] + '\n Loader { source:"MergeWatchdogProbe.qml"; onLoaded:item.config=' + json.dumps(config,ensure_ascii=True) + ' }\n' + text[at:]).encode('utf8'))
            env = h.environment(name); env.update(VEYRA_LOG_FILE=str(out / 'player.log'), VEYRA_TEST_ENGINE_HANG_MS=str(stall))
            command = [str(h.APP / 'veyra_qml_ui.exe'),'--data-dir',str(prefs),'--page','pro','--size','1280x800','--exit-after','45000']
            began = time.monotonic()
            with (out / 'console.log').open('wb') as stream:
                result = subprocess.run(command,cwd=h.APP,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=55)
            elapsed = time.monotonic()-began
            text = (out / 'player.log').read_text('utf8',errors='replace')
            bad = [line for line in h.errors(text) if 'engine-hang' not in line]
            dumps = list(out.glob('veyra-hang-*.dmp'))
            assert result.returncode == 0 and not bad and 'PROBE_AB_PASS' in text, text[-6000:]
            assert len(dumps) == 1 and 'minidump written=true' in text, text[-6000:]
            stalls = re.findall(r'owner=\d+ stalledMs=(\d+) .*stackFrames=(\d+)', text)
            assert len(stalls) == 1 and int(stalls[0][0]) >= 8000 and int(stalls[0][1]) > 0, stalls
            recovered = 'moved again' in text
            bounded = 'shutdown: owner thread still blocked' in text
            assert recovered and not bounded if stall > 0 else bounded and not recovered
            assert elapsed < (40 if stall > 0 else 30), elapsed
            row = {'case':name,'exit':result.returncode,'seconds':elapsed,'command':command,'stallMs':stall,
                'reportedAtMs':int(stalls[0][0]),'stackFrames':int(stalls[0][1]),'recovered':recovered,'boundedShutdown':bounded,
                'dump':str(dumps[0]),'dumpBytes':dumps[0].stat().st_size,'dumpSha256':h.sha(dumps[0]),
                'exeSha256':h.sha(h.APP / 'veyra_qml_ui.exe'),'driverSha256':h.sha(driver)}
            (out / 'result.json').write_text(json.dumps(row,ensure_ascii=False,indent=2),'utf8')
            rows.append(row); print(json.dumps(row,ensure_ascii=False),flush=True)
    finally:
        h.write(original,saved)
        target.unlink(missing_ok=True)
    (BASE / 'reports' / TASK / 'watchdog.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),'utf8')

assert sys.argv[1:] in (['pixels'], ['performance'], ['performance-reverse'], ['watchdog'])
h.no_player(); count = h.verify_donor(); h.stage()
if sys.argv[1] == 'pixels':
    h.pixels(True); h.clipping()
    h.qml_variant(False)
elif sys.argv[1] == 'performance':
    # Exercise the merged overlay-restart signal signature in the candidate.
    h.performance(True, ROOT / 'qml/Veyra/Main.qml')
elif sys.argv[1] == 'performance-reverse':
    h.performance(True, ROOT / 'qml/Veyra/Main.qml',
        [('perf-reverse-obs-fixed','obs',False,False), ('perf-reverse-obs-old','obs',True,False)],
        'performance-reverse.json')
else:
    watchdog()
assert h.verify_donor() == count
print('PASS merged GUI',sys.argv[1],'; donor unchanged',count,flush=True)

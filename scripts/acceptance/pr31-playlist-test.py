"""Bounded real playlist/window and WASAPI tests; no user profile changes."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

BASE = Path('E:/项目/Veyra')
TASK = 'pr31-playlist-20261009'
BUILD = BASE/'build'/TASK
DONOR = BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
FFMPEG = BASE/'deps/ffmpeg-subtitle-formats-20261009/installed/bin'
QT = Path('C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64')
stamp = time.strftime('%Y%m%d-%H%M%S')
LOG = BASE/'logs'/TASK/('tests-'+stamp)
TMP = BASE/'tmp'/TASK/('tests-'+stamp)
FIX = BASE/'tests'/TASK/('fixtures-'+stamp)
for path in (LOG, TMP, FIX): path.mkdir(parents=True, exist_ok=False)
env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('VEYRA_', 'QT_', 'QSG_'))}
env.update(PATH=os.pathsep.join(map(str, (FFMPEG, QT/'bin', DONOR)))+os.pathsep+env['PATH'],
           TEMP=str(TMP), TMP=str(TMP), QML_DISK_CACHE_PATH=str(TMP/'qml'),
           QT_PLUGIN_PATH=str(QT/'plugins'), QT_FORCE_STDERR_LOGGING='1',
           CUDA_CACHE_PATH=str(TMP/'cuda'))
commands = [('model', [BUILD/'playlist-tests.exe', FIX/'model']),
            ('window', [BUILD/'playlist-window-tests.exe', FIX/'window'])]
commands += [(mode, [BUILD/'capture-audio-tests.exe', '--'+mode]) for mode in
             ('endpoint-switch', 'endpoint-loss', 'slow-start', 'transient', 'xbox-float-rtp')]
selected = sys.argv[1:]
if selected:
    assert set(selected) <= {name for name, _ in commands}
    commands = [(name, values) for name, values in commands if name in selected]
results = []
for name, values in commands:
    command = list(map(str, values)); env['VEYRA_LOG_FILE'] = str(LOG/(name+'-engine.log'))
    start = time.monotonic()
    with (LOG/(name+'.log')).open('wb') as output:
        run = subprocess.run(command, cwd=TMP, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=180)
    text = (LOG/(name+'.log')).read_text(encoding='utf8', errors='replace')
    failed = [line for line in text.splitlines() if line.startswith('FAIL') or 'ReferenceError' in line or 'TypeError' in line]
    result = {'name': name, 'command': command, 'exit': run.returncode, 'seconds': time.monotonic()-start,
              'failures': failed, 'checks': [line for line in text.splitlines() if line.startswith(('PASS', 'FAIL', 'SKIP'))],
              'exeSha256': hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
              'skipped': name == 'endpoint-switch' and run.returncode == 77}
    results.append(result)
    (LOG/'receipt.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf8')
    print(name, 'exit', run.returncode, 'seconds', round(result['seconds'], 2), 'failures', failed, flush=True)
(BASE/'reports'/TASK/('tests-'+('-'.join(selected) if selected else 'all')+'-current.json')).write_text(json.dumps({'receipt': str(LOG/'receipt.json'), 'fixtures': str(FIX)}, indent=2), encoding='utf8')
print(LOG, flush=True)
raise SystemExit(any((r['exit'] and not r['skipped']) or r['failures'] for r in results))

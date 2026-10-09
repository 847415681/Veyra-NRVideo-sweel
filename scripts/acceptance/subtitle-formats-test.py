"""Run actual subtitle decoders and the native subtitle raster, with bounded tests."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import time

BASE=Path('E:/项目/Veyra');TASK='subtitle-formats-20261009'
BUILD=BASE/'build'/TASK;FIXTURES=BASE/'tests'/TASK/'formats'
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
FFMPEG=BASE/'deps'/('ffmpeg-'+TASK)/'installed/bin'
LOG=BASE/'logs'/TASK/('tests-'+time.strftime('%Y%m%d-%H%M%S'));TMP=BASE/'tmp'/TASK/'tests'
LOG.mkdir(parents=True,exist_ok=False);TMP.mkdir(parents=True,exist_ok=True)
env=dict(os.environ);env.update(PATH=str(FFMPEG)+os.pathsep+str(DONOR)+os.pathsep+env['PATH'],TEMP=str(TMP),TMP=str(TMP))
commands=[('formats',[BUILD/'subtitle-format-tests.exe',FIXTURES,BASE/'downloads'/TASK/'pgs-real.mkv',BASE/'downloads'/TASK/'pgs-dragon.mkv']),
          ('overlay',[BUILD/'subtitle-overlay-tests.exe',FIXTURES/'bitmap.sup']),
          ('text',[BUILD/'subtitle-text-tests.exe',BASE/'tests/playback-smoothness-20261004/fixtures'])]
for kind in ('pgs','dvd','dvb'):
    name='bitmap.mkv' if kind=='pgs' else 'bitmap-'+kind+'.mkv'
    commands.append(('bitmap-'+kind,[BUILD/'subtitle-bitmap-tests.exe',BASE/'tests/5060-presets-subtitles-20260918'/name]))
# This fixture has two embedded languages plus its same-name external SRT.
commands.append(('loading',[BUILD/'subtitle-loading-tests.exe',BASE/'tests/playback-smoothness-20261004/fixtures/golden.mkv','3']))
results=[]
for name,values in commands:
    command=list(map(str,values));env['VEYRA_LOG_FILE']=str(LOG/(name+'-engine.log'))
    start=time.monotonic()
    with (LOG/(name+'.log')).open('wb') as stream:run=subprocess.run(command,cwd=TMP,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=180)
    text=(LOG/(name+'.log')).read_text(encoding='utf8',errors='replace')
    failures=[line for line in text.splitlines() if line.startswith('FAIL') or 'MISMATCH' in line]
    results.append({'name':name,'command':command,'exit':run.returncode,'seconds':time.monotonic()-start,'failures':failures,'assertions':sum(line.startswith('PASS') for line in text.splitlines()),'exeSha256':hashlib.sha256(Path(command[0]).read_bytes()).hexdigest()})
    print(name,'exit',run.returncode,'checks',results[-1]['assertions'],'failures',failures,flush=True)
(LOG/'receipt.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf8')
(BASE/'reports'/TASK/'tests-current.json').write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
print(LOG,flush=True)
raise SystemExit(any(r['exit'] or r['failures'] for r in results))

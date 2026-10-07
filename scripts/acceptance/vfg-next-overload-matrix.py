"""Same-binary normal-playback controls for retained severe-overload estimates."""
from pathlib import Path
import json,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='vfg-next-20261007'
EXPECTED='daf3b27b5ea904ac1483c61610d07b3a3fd14ef41b472c69ebcf8ae2f89fb018'
cases=[('overload-queue-only-high8',2,8,20,['--no-overload-estimate']),
       ('overload-control-high8',2,8,20,['--no-overload-estimate','--full-reduced-queue-cost']),
       ('overload-high8-b',2,8,40,[]),
       ('overload-control-high3',2,3,20,['--no-overload-estimate','--full-reduced-queue-cost']),
       ('overload-high3',2,3,20,[]),
       ('overload-control-medium7',1,7,20,['--no-overload-estimate','--full-reduced-queue-cost']),
       ('overload-medium7',1,7,20,[]),
       ('overload-low8',0,8,20,[]),('overload-medium4',1,4,20,[]),('overload-high2',2,2,20,[])]
results=[]
for label,quality,multiplier,seconds,flags in cases:
 log=BASE/'logs'/TASK/(label+'-driver.log')
 cmd=[sys.executable,'-B',str(ROOT/'scripts/acceptance/vfg-next-preview.py'),'case',label,'--quality',str(quality),'--multiplier',str(multiplier),'--seconds',str(seconds),'--no-verbose',*flags]
 with log.open('xb') as stream:rc=subprocess.run(cmd,cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,timeout=280).returncode
 assert rc==0,(label,rc,str(log))
 data=json.loads((BASE/'logs'/TASK/label/'result.json').read_text(encoding='utf8'))
 assert data['passed'] and not data['errors'] and data['exeSha256']==EXPECTED
 assert any('requested=2 actual=2' in line for line in data['normalSchedulingClass'])
 results.append({'label':label,'quality':quality,'multiplier':multiplier,'seconds':seconds,'flags':flags,'fps':data['fps'],'timing':data.get('observationTiming')})
 (BASE/'logs'/TASK/'overload-matrix.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf8')
 print(json.dumps(results[-1],ensure_ascii=False),flush=True)

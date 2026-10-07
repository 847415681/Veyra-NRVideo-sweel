"""Final serial same-binary checks, including bounded recovery and a noisy tier."""
from pathlib import Path
import json,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='vfg-next-20261007'
EXPECTED='c007c7fc9072a6114eb2479b1880b65f5386a5fd445e6c58513faf3c1328a2b2'
OLD=['--no-overload-estimate','--full-reduced-queue-cost']
cases=[('final-control-high8',2,8,40,OLD),('final-high8',2,8,40,[]),
       ('final-control-high3',2,3,20,OLD),('final-high3',2,3,20,[]),
       ('final-control-medium7-a',1,7,40,OLD),('final-medium7-a',1,7,40,[]),
       ('final-medium7-b',1,7,40,[]),('final-control-medium7-b',1,7,40,OLD),
       ('final-control-medium8',1,8,20,OLD),('final-medium8',1,8,20,[]),
       ('final-low8',0,8,20,[]),('final-medium4',1,4,20,[]),('final-high2',2,2,20,[])]
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
 (BASE/'logs'/TASK/'final-matrix.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf8')
 print(json.dumps(results[-1],ensure_ascii=False),flush=True)

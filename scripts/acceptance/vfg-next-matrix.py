"""Serial normal-playback A/B cases, with a separate bounded process per case."""
from pathlib import Path
import json,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='vfg-next-20261007'
cases=[('queue-control-medium8-b',1,8,True),('queue-medium8-b',1,8,False),
       ('queue-control-medium7',1,7,True),('queue-medium7',1,7,False),
       ('queue-control-high4',2,4,True),('queue-high4',2,4,False),
       ('queue-control-high8',2,8,True),('queue-high8',2,8,False),
       ('queue-low8',0,8,False),('queue-medium4',1,4,False),('queue-high2',2,2,False)]
results=[]
for label,quality,multiplier,control in cases:
 log=BASE/'logs'/TASK/(label+'-driver.log')
 cmd=[sys.executable,'-B',str(ROOT/'scripts/acceptance/vfg-next-preview.py'),'case',label,'--quality',str(quality),'--multiplier',str(multiplier),'--no-verbose']
 if control:cmd.append('--full-reduced-queue-cost')
 with log.open('xb') as stream:rc=subprocess.run(cmd,cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,timeout=280).returncode
 assert rc==0,(label,rc,str(log))
 data=json.loads((BASE/'logs'/TASK/label/'result.json').read_text(encoding='utf8'))
 assert data['passed'] and not data['errors'] and data['exeSha256']=='93e8b2703c1316d6978f35c884e644d98af0682da1026b9f31de4995c8cc9e0c'
 assert any('requested=2 actual=2' in line for line in data['normalSchedulingClass'])
 results.append({'label':label,'quality':quality,'multiplier':multiplier,'control':control,'fps':data['fps'],'timing':data.get('observationTiming')})
 (BASE/'logs'/TASK/'matrix.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf8')
 print(json.dumps(results[-1],ensure_ascii=False),flush=True)

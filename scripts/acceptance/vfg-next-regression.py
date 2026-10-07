"""Serial GPU correctness, lifecycle and offline-export regression of the final binary."""
from pathlib import Path
import hashlib,json,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='vfg-next-20261007'
PACKAGE=BASE/'releases/release-2.0.6-20261007/packages/Veyra-2.0.6-NVIDIA-win64-portable';APP=BASE/'tests'/TASK/'app'
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
manifest=json.loads((PACKAGE/'package-manifest.json').read_text(encoding='utf8'))
for row in manifest['files']:
 if row['path']!='veyra_qml_ui.exe':assert sha(APP/row['path'])==row['sha256'],row['path']
assert sha(APP/'veyra_qml_ui.exe')==sha(BASE/'build'/TASK/'veyra_qml_ui.exe')=='c007c7fc9072a6114eb2479b1880b65f5386a5fd445e6c58513faf3c1328a2b2'
matrix=json.loads((BASE/'logs'/TASK/'final-matrix.json').read_text(encoding='utf8'));assert len(matrix)==13
cases=[('native','final-native-720-sync',[],360),('native','final-native-720-async',['--async-submit'],360),
       ('native','final-native-4k-async',['--width','3840','--height','2160','--async-submit'],360),
       ('lifecycle','final-lifecycle',[],1200),('exports','final-exports',[],1200)]
results=[]
for script,label,flags,limit in cases:
 start=time.monotonic();log=BASE/'logs'/TASK/(label+'-driver.log')
 with log.open('xb') as stream:
  rc=subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance'/('vfg-next-'+script+'.py')),label,*flags],cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,timeout=limit).returncode
 results.append({'script':script,'label':label,'exit':rc,'suiteSeconds':time.monotonic()-start,'log':str(log)})
 (BASE/'logs'/TASK/'regression.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf8')
 print(json.dumps(results[-1],ensure_ascii=False),flush=True);assert rc==0,(label,rc,str(log))

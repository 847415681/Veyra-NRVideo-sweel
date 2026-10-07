"""Collect final measurements and actual GPU/GUI/export evidence without changing logs."""
from pathlib import Path
import hashlib,json,re,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='vfg-next-20261007';LOG=BASE/'logs'/TASK;VERIFY=BASE/'verify'/TASK
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT,encoding='utf8').strip()
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/vfg-next-control.py')],cwd=ROOT,check=True)
expected='c007c7fc9072a6114eb2479b1880b65f5386a5fd445e6c58513faf3c1328a2b2'
assert sha(BASE/'build'/TASK/'veyra_qml_ui.exe')==expected
build=read(LOG/'overload-v2.json');assert build['exit']==0
for path,digest in build['sourceFiles'].items():assert sha(ROOT/path)==digest,path
products=set(git('diff','--name-only','96a7c8de','--','src','include','shaders','qml','CMakeLists.txt').splitlines())
assert products=={'src/engine/EngineController.cpp','include/veyra/engine/VfgQueueBudget.h','include/veyra/engine/VfgOverloadEstimate.h'},products
matrix=read(LOG/'final-matrix.json');assert len(matrix)==13
for row in matrix:
 data=read(LOG/row['label']/'result.json');assert data['passed'] and not data['errors'] and data['exeSha256']==expected
 assert any('requested=2 actual=2' in x for x in data['normalSchedulingClass'])
 samples=read(LOG/row['label']/'samples.json')
 assert all(s['nr'] and s['fg'] and s['backend']=='vfg' and s['cap']==0 and not s['strict'] for s in samples)
 assert all(s['multiplier']==row['multiplier'] and s['quality']==row['quality'] for s in samples)
 row.update(receiptSha256=sha(LOG/row['label']/'result.json'),rawLogSha256=sha(LOG/row['label']/'player.log'))
by={x['label']:x for x in matrix}
assert by['final-high3']['fps']>by['final-control-high3']['fps']
assert by['final-high8']['fps']>by['final-control-high8']['fps'] and by['final-high8']['timing']['expiredGenerated']<by['final-control-high8']['timing']['expiredGenerated']
assert by['final-high8']['timing']['previewSkipped']<by['final-control-high8']['timing']['previewSkipped']
for label,target in [('final-low8',240),('final-medium4',120),('final-high2',60)]:assert by[label]['fps']>=target*.97
native=[]
def pixels(path):
 text=path.read_text(encoding='utf8',errors='replace');values={}
 for line in text.splitlines():
  m=re.match(r'VFG format=(\d+) size=(\d+)x(\d+) quality=(\d+) multiplier=(\d+).* hashes=([0-9a-f,]+)',line)
  if m:
   key=tuple(int(x) for x in m.groups()[:5]);hashes=m.group(6).strip(',').split(',');assert len(hashes)==key[4]-1
   values[key]=hashes
 assert len(values)==42 and sum(map(len,values.values()))==168,path
 return values
golden=pixels(LOG/'baseline-native-720.log')
for label in ('final-native-720-sync','final-native-720-async','final-native-4k-async'):
 receipt=read(LOG/(label+'.json'));text=(LOG/(label+'.log')).read_text(encoding='utf8',errors='replace')
 assert receipt['exit']==0 and receipt['seconds']<300 and 'Native GPU VFG: 1033 checks 0 failures.' in text,label
 image=pixels(LOG/(label+'.log'))
 if '720' in label:assert image==golden,label
 else:assert image==pixels(BASE/'logs/vfg-optimization-20261007/async-native-4k-v2.log'),label
 native.append({'label':label,'checks':1033,'seconds':receipt['seconds'],'pixelGroups':42,'pixelImages':168,'allPixelFingerprintsMatch':True,'rawLogSha256':sha(LOG/(label+'.log'))})
lifecycle=read(LOG/'final-lifecycle/summary.json');assert len(lifecycle)==4 and all(x['passed'] and x['seconds']<300 for x in lifecycle)
exports=read(LOG/'final-exports/summary.json');assert len(exports)==24 and all(x['seconds']<300 for x in exports)
assert sum(x['expected']=='success' for x in exports)==22
assert 'VFG settings 353 checks, 0 failures' in (LOG/'overload-v2-settings.log').read_text(encoding='utf8')
assert '[vfg-overload-probe]' in (LOG/'final-high8/player.log').read_text(encoding='utf8',errors='replace')
result={'passed':True,'productSourceCommit':git('rev-parse','ba38f7e'),'exeSha256':expected,'compiledInputFiles':len(build['sourceFiles']),
 'compiledInputSha256':build['sourceFiles'],'productFiles':sorted(products),'settingsChecks':353,'matrix':matrix,'native':native,'lifecycle':lifecycle,
 'exportCases':len(exports),'exportSuccessCases':22,'exportBoundaryCases':['cancel','missing-runtime'],
 'method':'RTX5070; original GTA 4K30; original NR 1080p strength1; VFG native4K; normal requested/actual GPU priority2; 5s warmup; 20/40s observations; median 1Hz software Present FPS, not scanout or end-to-end latency',
 'knownLimits':['High8 remains a 2X fallback in most pairs; a 30s full probe can still be late','Medium7 has run-to-run variance; no consistent tier-wide gain is claimed','High3 expired counts vary; the final pair was 4 before and 7 after despite higher valid throughput','NVIDIA VRAM growth and other GPUs/real capture/streaming long stability were not evaluated here']}
VERIFY.mkdir(parents=True,exist_ok=True);assert not (VERIFY/'measurements.json').exists()
(VERIFY/'measurements.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8')
print('VFG FINAL MEASUREMENTS PASS',len(matrix),'preview cases;',len(native),'GPU suites;',len(exports),'export cases',flush=True)

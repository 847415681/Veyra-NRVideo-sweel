"""Serial, bounded tests. One shared task app, independent profiles/logs/TEMP."""
from pathlib import Path
import hashlib,json,os,re,shutil,statistics,subprocess,sys,time
import psutil
SRC=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='fg-flash-fix-20261009'
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
APP=BASE/'tests'/TASK/'app';BUILD=BASE/'build'/TASK
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
manifest=json.loads((DONOR/'package-manifest.json').read_text(encoding='utf8'))
def verify():
 for row in manifest['files']:assert sha(DONOR/row['path'])==row['sha256'],row['path']
verify()
for row in manifest['files']:
 rel=Path(row['path'])
 if rel.parts[0]=='shaders':
  p=BUILD/rel;p.parent.mkdir(parents=True,exist_ok=True)
  if not p.exists():os.link(DONOR/rel,p)
  assert sha(p)==row['sha256']
if not APP.exists():
 APP.mkdir(parents=True)
 for row in manifest['files']:
  rel=Path(row['path'])
  if rel.parts[0] in ('runtime_local','logs') or rel.name=='veyra_qml_ui.exe':continue
  p=APP/rel;p.parent.mkdir(parents=True,exist_ok=True);os.link(DONOR/rel,p)
 (APP/'runtime_local').mkdir(exist_ok=True)
exe=APP/'veyra_qml_ui.exe'
swap=APP/'candidate.swap';shutil.copy2(BUILD/'veyra-hdr-flash-fix.exe',swap);os.replace(swap,exe)
mode,label=sys.argv[1:3];assert mode in ('native','gui') and re.fullmatch('[a-z0-9-]+',label)
for proc in psutil.process_iter(['pid','name']):
 name=(proc.info['name'] or '').lower()
 assert not name.startswith(('veyra','fg-presentation','flash-pixel')),f'Existing player/probe: {name} PID {proc.pid}'
case=BASE/'logs'/TASK/label;case.mkdir(parents=True,exist_ok=False)
tmp=BASE/'tmp'/TASK/label;tmp.mkdir(parents=True,exist_ok=False)
env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
env.update(PATH=str(APP)+os.pathsep+env['PATH'],TEMP=str(tmp),TMP=str(tmp),CUDA_CACHE_PATH=str(tmp/'cuda-cache'),QML_DISK_CACHE_PATH=str(tmp/'qml-cache'),QT_FORCE_STDERR_LOGGING='1',VEYRA_LOG_FILE=str(case/'engine.log'))
copy=sys.argv[3] if len(sys.argv)>3 else 'auto'
assert copy in ('auto','0','1')
if copy!='auto':env['VEYRA_TEST_HDR_PRESENT_COPY']=copy
main=APP/'qml/Veyra/Main.qml';original=main.read_bytes();mutated=False
if mode=='native':
 testmode=sys.argv[4] if len(sys.argv)>4 else 'hdr-copy'
 if testmode=='hdr-copy-vfg':env['VEYRA_TEST_HDR_VFG']='1';env['VEYRA_TEST_HDR_4K_WINDOW']='1';testmode='hdr-copy'
 if testmode=='hdr-copy-4k':env['VEYRA_TEST_HDR_4K_WINDOW']='1';testmode='hdr-copy'
 command=[str(BUILD/'fg-presentation-tests.exe'),str(case),str(APP/'runtime/experimental')]
 if testmode!='sdr':command.append(testmode)
else:
 backend=sys.argv[4] if len(sys.argv)>4 else 'dlss';assert backend in ('dlss','vfg','xess','off')
 seconds=int(sys.argv[5]) if len(sys.argv)>5 else 35;assert 15<=seconds<=90
 profile=BASE/'tests'/TASK/'profiles'/label
 shutil.copytree(BASE/'tests/vfg-next-20261007/candidate-cold/profile',profile)
 prefs=json.loads((profile/'qml-preferences.v1.json').read_text(encoding='utf8'));prefs['positions']={};prefs['gpuPriority']='normal'
 (profile/'qml-preferences.v1.json').write_text(json.dumps(prefs),encoding='utf8')
 # Fresh private session. Configure the existing bridge, then open the media.
 timer='''
    Timer { interval: 700; running: true; repeat: false; onTriggered: {
        veyra.fgEnabled = false; veyra.nrEnabled = true; veyra.fgMultiplier = 2;
        veyra.fgBackendName = "%s"; veyra.contentRate = 0;
        veyra.fgEnabled = %s;
        veyra.openPath("E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4");
    } }
'''%(backend if backend!='off' else 'dlss','true' if backend!='off' else 'false')
 # Match the source invocation used by the earlier private QML smoke tests.
 text=original.decode('utf8');assert text.count('    id: root')==1
 modified=text.replace('    id: root','    id: root'+timer,1);swap=main.with_suffix('.test-swap');swap.write_text(modified,encoding='utf8');os.replace(swap,main);mutated=True
 (case/'Main.test.qml').write_text(modified,encoding='utf8')
 command=[str(exe),'--data-dir',str(profile),'--page','min','--size','1280x800','--exit-after',str(seconds*1000)]
begin=time.monotonic()
try:
 with (case/'console.log').open('xb') as stream:r=subprocess.run(command,cwd=case if mode=='native' else APP,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=180)
finally:
 if mutated:
  swap=main.with_suffix('.test-swap');swap.write_bytes(original);os.replace(swap,main)
text=(case/'console.log').read_text(encoding='utf8',errors='replace')
if (case/'engine.log').is_file():
 engine=(case/'engine.log').read_text(encoding='utf8',errors='replace')
 text=engine+'\n'+'\n'.join(l for l in text.splitlines() if l not in engine)
errors=[l for l in text.splitlines() if re.search(r'\[ERROR\s*\]|\[FATAL\s*\]|ReferenceError:|TypeError:|D3D12 ERROR|D3D12 CORRUPTION',l)]
receipt={'mode':mode,'command':command,'copyOverride':copy,'exit':r.returncode,'elapsedSeconds':time.monotonic()-begin,'exeSha256':sha(Path(command[0])),'errors':errors,'copyEvents':[l for l in text.splitlines() if '[present-copy]' in l],'summaries':[l for l in text.splitlines() if re.match(r'^(HDR_COPY|PRESENTATION|EARLY_ORIGINAL|D3D12 errors)',l)]}
if mode=='gui':
 receipt['graphEvents']=[l for l in text.splitlines() if '[graph]' in l and ('processing=' in l or 'initialized src' in l)]
 receipt['validity']=[l for l in text.splitlines() if '[fg-validity]' in l][-3:]
 receipt['rates']=[l for l in text.splitlines() if '[frame-rate]' in l][-5:]
 receipt['stageTimings']=[l for l in text.splitlines() if '[player-timing]' in l][-5:]
 receipt['closed']=[l for l in text.splitlines() if '[frame-flow] state=closed' in l]
 receipt['presentCosts']=[l for l in text.splitlines() if '[present-cost]' in l][-8:]
receipt['passed']=r.returncode==0 and not errors and (any('SUMMARY' in l and 'pass=1' in l for l in receipt['summaries']) if mode=='native' and testmode=='hdr-copy' else True)
if mode=='gui':
 receipt['passed']=receipt['passed'] and any('processing=1920x1080' in l for l in receipt['graphEvents']) and bool(receipt['closed'])
 if backend!='off':receipt['passed']=receipt['passed'] and any(re.search(r' valid=[1-9]\d* invalid=0',l) for l in receipt['validity'])
 if copy!='auto':receipt['passed']=receipt['passed'] and any(('enabled='+('true' if copy=='1' else 'false')) in l for l in receipt['copyEvents'])
verify();receipt['donorUnchanged']=True
(case/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps(receipt,ensure_ascii=False,indent=2),flush=True)
raise SystemExit(0 if receipt['passed'] else 1)

"""Bounded serial contract, GPU and real USB tests; BMD hardware is not local."""
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,time,psutil
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='blackmagic-followup-20261009';BUILD=BASE/'build'/TASK
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
label=sys.argv[1];assert label.replace('-','').isalnum()
OUT=BASE/'tests'/TASK/label;TMP=BASE/'tmp'/TASK/label;LOG=BASE/'logs'/TASK/label
for p in (OUT,TMP,LOG):p.mkdir(parents=True,exist_ok=False)
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/bmd-followup-control.py')],check=True)
build=json.loads(Path(json.loads((BASE/'reports'/TASK/'build-current.json').read_text(encoding='utf8'))['receipt']).read_text(encoding='utf8'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for n,d in build['sources'].items():assert sha(ROOT/n)==d,n
for n,d in build['executables'].items():assert sha(Path(n))==d['sha256'],n
# GPU fixtures locate shader binaries relative to their own EXE, not PATH.
for p in (DONOR/'shaders').rglob('*'):
 if p.is_file():
  target=BUILD/'shaders'/p.relative_to(DONOR/'shaders');target.parent.mkdir(parents=True,exist_ok=True)
  if not target.exists():os.link(p,target)
  assert sha(target)==sha(p),target
busy=[p.info for p in psutil.process_iter(['pid','name']) if p.info['name'] and p.info['name'].lower() in ('veyra.exe','veyra_qml_ui.exe','veyra-bmd-followup.exe','capture-gpu.exe')]
assert not busy,busy
env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
env.update(TEMP=str(TMP),TMP=str(TMP),CUDA_CACHE_PATH=str(TMP/'cuda-cache'))
win=Path(env['WINDIR']);env['PATH']=os.pathsep.join(map(str,(BUILD,DONOR,win/'System32',win,win/'System32/Wbem')))
cases=[('pre-fix-contract','capture-contract-red.exe',[],True),('candidate-contract','capture-contract.exe',[],False),('gpu-pixels','capture-gpu.exe',[],False),('devices','capture-source.exe',['--list'],False)]
if '--usb' in sys.argv:cases+=[('usb-1080p60','capture-source.exe',['--rate-test','capture:0:1:-1','0'],False)]
if '--cases' in sys.argv:
 selected=set(sys.argv[sys.argv.index('--cases')+1].split(','))
 assert selected<=set(c[0] for c in cases),selected
 cases=[c for c in cases if c[0] in selected]
if '--gui' in sys.argv:cases=[]
results=[]
for name,exe,a,red in cases:
 log=LOG/(name+'.log');child=env.copy();child['VEYRA_LOG_FILE']=str(LOG/(name+'-engine.log'));cmd=[str(BUILD/exe),*a];start=time.monotonic()
 with log.open('xb') as f:
  try:rc=subprocess.run(cmd,cwd=OUT,env=child,stdout=f,stderr=subprocess.STDOUT,timeout=290).returncode
  except subprocess.TimeoutExpired:rc=124
 text=log.read_text(encoding='utf8',errors='replace');failures=re.findall(r'^FAIL .+$',text,re.M)
 if red:
  passed=rc==1 and len(failures)>=7 and all('BMD ' in f for f in failures) and 'failures=' in text
 else:passed=rc==0 and not failures and not re.search(r'\bpass=0\b',text)
 if name=='candidate-contract':passed=passed and text.count('PASS BMD source pixels delivered exactly')==2 and 'failures=0' in text
 if name=='gpu-pixels':passed=passed and len(re.findall(r'BLACKMAGIC_HDYC_GPU flip=[01] identical709=1.*pass=1',text))==2 and len(re.findall(r'CAPTURE_GPU subtype=43594448.*pass=1',text))==2
 # pass=0 is the first measurement index in this fixture, not a failed boolean.
 if name=='usb-1080p60':passed=rc==0 and not failures and len(re.findall(r'PASS RATE pass=[01]',text))==2 and not re.search(r'\[capture-bmd-compat\]',text)
 result={'case':name,'command':cmd,'exit':rc,'seconds':time.monotonic()-start,'passed':bool(passed),'expectedOldFailure':red,'failedChecks':failures,'checksPrinted':len(re.findall(r'^PASS\b|\bpass=1\b',text,re.M)),'exeSha256':sha(BUILD/exe),'log':str(log)}
 results.append(result);(LOG/'summary.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf8')
 print(json.dumps(result,ensure_ascii=False),flush=True);print('\n'.join(text.splitlines()[-4:]),flush=True)
 if not passed:raise SystemExit(1)
if '--gui' in sys.argv:
 app=BASE/'tests'/TASK/'app';main=app/'qml/Veyra/Main.qml';original=main.read_bytes()
 assert sha(app/'veyra_qml_ui.exe')==sha(BUILD/'veyra-bmd-followup.exe')
 assert os.stat(main).st_ino!=os.stat(DONOR/'qml/Veyra/Main.qml').st_ino,'Main.qml must be a private copy before loader injection'
 profile=OUT/'profile';images=OUT/'screenshots';profile.mkdir();images.mkdir()
 (profile/'qml-preferences.v1.json').write_text(json.dumps({'overlayCompat':'off','gpuPriority':'normal','obsGameCapture':False,'screenshotDir':str(images)},ensure_ascii=False),encoding='utf8')
 source=original.decode('utf8');at=source.rfind('}')
 loader='\nLoader { source: '+json.dumps((ROOT/'scripts/acceptance/blackmagic-ui.qml').as_uri())+'; onLoaded: { item.dialogHost=dialogs;item.evidence='+json.dumps(str(images))+';item.phase="capture"; } }\n'
 child=env.copy();child.update(VEYRA_LOG_FILE=str(LOG/'gui-engine.log'),CUDA_CACHE_PATH=str(TMP/'cuda-cache'),QML_DISK_CACHE_PATH=str(TMP/'qml-cache'))
 child['PATH']=os.pathsep.join(map(str,(app,win/'System32',win,win/'System32/Wbem')))
 command=[str(app/'veyra_qml_ui.exe'),'--page','pro','--size','1280x900','--data-dir',str(profile),'--exit-after','110000']
 start=time.monotonic();log=LOG/'gui.log'
 try:
  main.write_text(source[:at]+loader+source[at:],encoding='utf8')
  with log.open('xb') as f:
   try:rc=subprocess.run(command,cwd=app,env=child,stdout=f,stderr=subprocess.STDOUT,timeout=125).returncode
   except subprocess.TimeoutExpired:rc=124
 finally:main.write_bytes(original)
 text=log.read_text(encoding='utf8',errors='replace')
 passed=rc==0 and 'BLACKMAGIC_UI_PASS' in text and not re.search(r'BLACKMAGIC_UI_FAIL|ReferenceError:|TypeError:|no root object',text)
 imageFiles=list(images.glob('veyra-*.png'));passed=passed and len(imageFiles)==1
 result={'case':'production-gui-usb','command':command,'exit':rc,'seconds':time.monotonic()-start,'passed':bool(passed),'exeSha256':sha(app/'veyra_qml_ui.exe'),'mainRestored':sha(main)==hashlib.sha256(original).hexdigest(),'screenshotFiles':[str(p) for p in images.glob('*.png')],'fixtureSha256':sha(ROOT/'scripts/acceptance/blackmagic-ui.qml'),'log':str(log)}
 (LOG/'summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8');print(json.dumps(result,ensure_ascii=False),flush=True)
 print('\n'.join(text.splitlines()[-6:]),flush=True);assert passed
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/bmd-followup-control.py')],check=True)

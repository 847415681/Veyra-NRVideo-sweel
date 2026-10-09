"""Serial HDR presentation and real-QML preference persistence checks."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,time
import psutil
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='hdr-fg-toggle-20261009'
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
APP=BASE/'tests'/TASK/'app';BUILD=BASE/'build'/TASK
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
manifest=json.loads((DONOR/'package-manifest.json').read_text(encoding='utf8'))
def verify():
 for row in manifest['files']:assert sha(DONOR/row['path'])==row['sha256'],row['path']
verify()
for proc in psutil.process_iter(['pid','name']):
 name=(proc.info['name'] or '').lower()
 assert not name.startswith(('veyra','fg-presentation','flash-pixel')),f'Existing player/probe: {name} PID {proc.pid}'
if not APP.exists():
 APP.mkdir(parents=True)
 for row in manifest['files']:
  rel=Path(row['path'])
  if rel.parts[0] in ('runtime_local','logs') or rel.name=='veyra_qml_ui.exe':continue
  p=APP/rel;p.parent.mkdir(parents=True,exist_ok=True)
  if rel.as_posix() in ('qml/Veyra/Main.qml','qml/Veyra/SettingsPage.qml'):shutil.copy2(DONOR/rel,p)
  else:os.link(DONOR/rel,p)
 (APP/'runtime_local').mkdir(exist_ok=True)
for rel in ('veyra_qml_ui.exe','qml/Veyra/SettingsPage.qml'):
 source=BUILD/rel if rel.endswith('.exe') else ROOT/rel
 swap=APP/(rel+'.swap');shutil.copy2(source,swap);os.replace(swap,APP/rel)
for row in manifest['files']:
 rel=Path(row['path'])
 if rel.parts[0]=='shaders':
  p=BUILD/rel;p.parent.mkdir(parents=True,exist_ok=True)
  if not p.exists():os.link(DONOR/rel,p)
  assert sha(p)==row['sha256']
mode,label=sys.argv[1:3];assert mode in ('native','gui','capture') and re.fullmatch('[a-z0-9-]+',label)
case=BASE/'logs'/TASK/label;case.mkdir(parents=True,exist_ok=False)
tmp=BASE/'tmp'/TASK/label;tmp.mkdir(parents=True,exist_ok=False)
env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
env.update(PATH=str(APP)+os.pathsep+env['PATH'],TEMP=str(tmp),TMP=str(tmp),CUDA_CACHE_PATH=str(tmp/'cuda-cache'),QML_DISK_CACHE_PATH=str(tmp/'qml-cache'),QT_FORCE_STDERR_LOGGING='1',VEYRA_LOG_FILE=str(case/'engine.log'))
main=APP/'qml/Veyra/Main.qml';original=main.read_bytes();mutated=False
if mode=='capture':
 test=sys.argv[3];assert test in ('contracts','pixels')
 command=[str(BUILD/('capture-contract.exe' if test=='contracts' else 'capture-gpu.exe'))]
elif mode=='native':
 test=sys.argv[3] if len(sys.argv)>3 else 'hdr'
 assert test in ('hdr','hdr-4k','vfg-4k','sdr','direct')
 if test in ('hdr-4k','vfg-4k'):env['VEYRA_TEST_HDR_4K_WINDOW']='1'
 if test=='vfg-4k':env['VEYRA_TEST_HDR_VFG']='1'
 if test=='direct':env['VEYRA_TEST_HDR_PRESENT_COPY']='0'
 command=[str(BUILD/'fg-presentation-tests.exe'),str(case),str(APP/'runtime/experimental')]
 if test!='sdr':command.append('hdr-copy')
else:
 phase=sys.argv[3];assert phase in ('fresh','restore-on','restore-off','legacy','visual')
 profile=BASE/'tests'/TASK/'profiles'/(phase if phase in ('legacy','visual') else 'persistence')
 if phase in ('fresh','legacy','visual'):
  profile.mkdir(parents=True,exist_ok=False)
  prefs={'gpuPriority':'normal','language':'en' if phase=='legacy' else 'zh-CN','autoResume':False,'prewarmEnhancement':False,'sentinel':'preserved'}
  if phase=='legacy':prefs['presentation']={'lowQueue':True,'display':1,'outputRate':0,'customFps':60}
  (profile/'qml-preferences.v1.json').write_text(json.dumps(prefs),encoding='utf8')
 expected=phase=='restore-on';toggle='true' if phase=='fresh' else 'false'
 timer='''
    Timer { interval: 1500; running: true; repeat: false; onTriggered: {
        root.goPage("set"); settingsPage.section = "compat";
        function find(item) {
            if (item.objectName === "set-hdr-fg-compatibility") return item;
            if (item.children) for (let i = 0; i < item.children.length; ++i) { const hit = find(item.children[i]); if (hit) return hit; }
            return null;
        }
        const sw = find(settingsPage);
        if (!sw || sw.checked !== EXPECTED || !!veyra.preferences.hdrFgCompatibility !== EXPECTED) { console.error("HDRGUI_FAIL initial"); return; }
        console.log("HDRGUI_INITIAL", sw.checked);
        console.log("HDRGUI_LABEL", qsTr("HDR 补帧闪黑兼容"));
        sw.toggled(TOGGLE);
        Qt.callLater(() => {
            if (sw.checked !== TOGGLE || veyra.preferences.hdrFgCompatibility !== TOGGLE) { console.error("HDRGUI_FAIL saved"); return; }
            // Other presentation controls must not erase this global choice.
            veyra.outputCustomFps = 61;
            console.log("HDRGUI_PASS");
        });
    } }
    Timer { interval: 3200; running: true; repeat: false; onTriggered: {
        root.contentItem.grabToImage(result => result.saveToFile("SCREENSHOT"));
    } }
'''.replace('EXPECTED',str(expected).lower()).replace('TOGGLE',toggle).replace('SCREENSHOT',(case/'settings.png').as_posix())
 text=original.decode('utf8');assert text.count('    id: root')==1
 swap=main.with_suffix('.swap');swap.write_text(text.replace('    id: root','    id: root'+timer,1),encoding='utf8');os.replace(swap,main);mutated=True
 command=[str(APP/'veyra_qml_ui.exe'),'--data-dir',str(profile),'--page','set','--size','1280x800','--exit-after','6000']
begin=time.monotonic()
try:
 with (case/'console.log').open('xb') as stream:r=subprocess.run(command,cwd=case if mode=='native' else APP,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=180)
finally:
 if mutated:
  swap=main.with_suffix('.swap');swap.write_bytes(original);os.replace(swap,main)
text=(case/'console.log').read_text(encoding='utf8',errors='replace')
if (case/'engine.log').is_file():text+='\n'+(case/'engine.log').read_text(encoding='utf8',errors='replace')
errors=[l for l in text.splitlines() if re.search(r'\[ERROR\s*\]|\[FATAL\s*\]|ReferenceError:|TypeError:|D3D12 ERROR|D3D12 CORRUPTION|HDRGUI_FAIL',l)]
expectedErrors=[]
if mode=='capture' and test=='contracts':
 # These negative cases deliberately reject bad source contracts. Require the
 # exact error counts AND their successful rejection assertions; do not ignore
 # arbitrary ERROR lines or use a blanket exception for contract tests.
 expected=[(r'\[ERROR\] \[capture-elgato\] Tonemap readback disagrees; refusing a mislabeled color contract$',1),
           (r'\[ERROR\] \[capture\] dynamic capture contract changed; reopen required hr=0x80040200$',5)]
 assertions={'PASS reject successful Set with contradictory readback':1,
             'PASS BMD mode change requires reopen rather than reinterpretation':2,
             'PASS BMD dynamic interlace change cannot keep stale temporal contract':2,
             'PASS dynamic mode cannot reuse stale layout':1}
 if all(sum(bool(re.search(pattern,line)) for line in errors)==count for pattern,count in expected) and all(text.count(line)==count for line,count in assertions.items()):
  expectedErrors=[line for line in errors if any(re.search(pattern,line) for pattern,count in expected)]
  errors=[line for line in errors if line not in expectedErrors]
passed=r.returncode==0 and not errors
receipt={'mode':mode,'command':command,'exit':r.returncode,'seconds':time.monotonic()-begin,'errors':errors,'exeSha256':sha(Path(command[0])),'events':[l for l in text.splitlines() if '[present-copy]' in l or l.startswith(('HDR_COPY','PRESENTATION','D3D12 errors')) or 'HDRGUI_' in l]}
if expectedErrors:receipt['expectedNegativeCaseErrors']=expectedErrors
if mode=='gui':
 prefs=json.loads((profile/'qml-preferences.v1.json').read_text(encoding='utf8'))
 passed=passed and 'HDRGUI_PASS' in text and prefs.get('hdrFgCompatibility')==(phase=='fresh') and prefs.get('sentinel')=='preserved' and (case/'settings.png').is_file()
 if phase=='legacy':passed=passed and 'HDR frame generation flash compatibility' in text
 receipt['savedPreference']=prefs.get('hdrFgCompatibility');receipt['screenshot']=str(case/'settings.png')
elif mode=='native':
 passed=passed and (('HDR_COPY_SUMMARY' in text and 'pass=1' in text) if test!='sdr' else 'D3D12 errors=0' in text)
 if test in ('hdr','hdr-4k','vfg-4k'):
  passed=passed and 'requested=true eligible=true enabled=true' in text and 'requested=false eligible=true enabled=false' in text
else:
 passed=passed and not re.search(r'^FAIL\b|\bpass=0\b',text,re.M)
 if test=='contracts':passed=passed and text.count('PASS BMD source pixels delivered exactly')==2 and 'failures=0' in text
 else:passed=passed and len(re.findall(r'BLACKMAGIC_HDYC_GPU flip=[01] identical709=1.*pass=1',text))==2
 receipt['checksPrinted']=len(re.findall(r'^PASS\b|\bpass=1\b',text,re.M))
verify();receipt.update(passed=passed,donorUnchanged=True)
(case/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps({k:v for k,v in receipt.items() if k!='events'},ensure_ascii=False,indent=2),flush=True)
print('\n'.join(receipt['events'][-10:]),flush=True)
raise SystemExit(0 if passed else 1)

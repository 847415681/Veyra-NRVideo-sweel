"""Real capture settings, saved switch and ordinary USB; isolated app/profile."""
from pathlib import Path
import hashlib, json, os, re, shutil, subprocess, sys, time
import psutil

ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='decklink-native-20261009'
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
BUILD=BASE/'build'/TASK;APP=BASE/'tests'/TASK/'app'
FFMPEG=BASE/'deps/ffmpeg-subtitle-formats-20261009/installed/bin'
case=sys.argv[1] if len(sys.argv)>1 else 'switch'
assert case in ('switch','usb')
LOG=BASE/'logs'/TASK/('gui-'+case+'-'+time.strftime('%Y%m%d-%H%M%S'))
TMP=BASE/'tmp'/TASK/LOG.name
for p in (LOG,TMP):p.mkdir(parents=True,exist_ok=False)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
manifest=json.loads((DONOR/'package-manifest.json').read_text(encoding='utf8'))
def verify():
    for row in manifest['files']:assert sha(DONOR/row['path'])==row['sha256'],row['path']
verify()
for proc in psutil.process_iter(['name']):
    assert not (proc.info['name'] or '').lower().startswith(('veyra','fg-presentation','flash-pixel')),proc.info
if not APP.exists():
    APP.mkdir(parents=True)
    for row in manifest['files']:
        rel=Path(row['path'])
        if rel.parts[0] in ('runtime_local','logs') or rel.name=='veyra_qml_ui.exe':continue
        target=APP/rel;target.parent.mkdir(parents=True,exist_ok=True);os.link(DONOR/rel,target)
    (APP/'runtime_local').mkdir(exist_ok=True)
def replace(source,target):
    target.parent.mkdir(parents=True,exist_ok=True)
    swap=target.with_name(target.name+'.swap');shutil.copy2(source,swap);os.replace(swap,target)
def write(target,content):
    swap=target.with_name(target.name+'.swap');swap.write_bytes(content);os.replace(swap,target)
replace(BUILD/'veyra_qml_ui.exe',APP/'veyra_qml_ui.exe')
for source in (ROOT/'qml').rglob('*'):
    if source.is_file() and (source.suffix in ('.qml','.js') or source.name=='qmldir'):
        replace(source,APP/'qml'/source.relative_to(ROOT/'qml'))
for source in (BUILD/'shaders').rglob('*.dxil'):replace(source,APP/'shaders'/source.relative_to(BUILD/'shaders'))
for source in FFMPEG.glob('*.dll'):replace(source,APP/source.name)
profile=BASE/'tests'/TASK/'profiles'/LOG.name;profile.mkdir(parents=True,exist_ok=False)
preferences=profile/'qml-preferences.v1.json'
preferences.write_text(json.dumps({'language':'zh-CN','autoResume':False,'rememberPosition':False,
    'prewarmEnhancement':False,'gpuPriority':'normal','subtitleEnabled':False,
    'screenshotDir':str(LOG/'screenshots'),'sentinel':'preserved'}),encoding='utf8')
env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
env.update(PATH=str(APP)+os.pathsep+env['PATH'],TEMP=str(TMP),TMP=str(TMP),CUDA_CACHE_PATH=str(TMP/'cuda'),
    QML_DISK_CACHE_PATH=str(TMP/'qml-cache'),QT_FORCE_STDERR_LOGGING='1')
main=APP/'qml/Veyra/Main.qml';saved=main.read_bytes();results=[]
try:
    for phase in (('fresh','restore','english') if case=='switch' else ('capture',)):
        evidence=LOG/phase;evidence.mkdir()
        if phase=='english':
            prefs=json.loads(preferences.read_text(encoding='utf8'));prefs['language']='en';preferences.write_text(json.dumps(prefs),encoding='utf8')
        test=ROOT/'scripts/acceptance'/('decklink-native-ui.qml' if case=='switch' else 'blackmagic-ui.qml')
        loader='\nLoader { source: '+json.dumps(test.as_uri())+'; onLoaded: { item.dialogHost=dialogs; item.phase='+json.dumps(phase)+'; item.evidence='+json.dumps(evidence.as_posix(),ensure_ascii=False)+' } }\n'
        text=saved.decode('utf8');at=text.rfind('}');assert at>0
        write(main,(text[:at]+loader+text[at:]).encode('utf8'))
        env['VEYRA_LOG_FILE']=str(evidence/'engine.log')
        command=[str(APP/'veyra_qml_ui.exe'),'--data-dir',str(profile),'--page','home','--size','1280x900','--exit-after','29000' if case=='switch' else '100000']
        began=time.monotonic()
        with (evidence/'console.log').open('wb') as stream:
            run=subprocess.run(command,cwd=APP,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=45 if case=='switch' else 115)
        console=(evidence/'console.log').read_text(encoding='utf8',errors='replace')
        errors=[line for line in console.splitlines() if re.search(r'NATIVE_UI_FAIL|BLACKMAGIC_UI_FAIL|ReferenceError:|TypeError:|\[ERROR\s*\]|\[FATAL\s*\]|D3D12 ERROR|D3D12 CORRUPTION|Cannot assign|Binding loop',line)]
        prefs=json.loads(preferences.read_text(encoding='utf8'));assert prefs.get('sentinel')=='preserved'
        if case=='switch':assert bool(prefs.get('blackmagicNativeCapture',False))==(phase!='restore')
        passed=run.returncode==0 and not errors and ('NATIVE_UI_PASS' if case=='switch' else 'BLACKMAGIC_UI_PASS') in console
        row={'phase':phase,'command':command,'exit':run.returncode,'seconds':time.monotonic()-began,'passed':passed,
             'errors':errors,'events':[l for l in console.splitlines() if 'NATIVE_UI_' in l or 'BLACKMAGIC_UI_' in l],'profile':str(profile)}
        results.append(row);print(json.dumps(row,ensure_ascii=False),flush=True)
        if not passed:break
finally:write(main,saved)
verify()
receipt={'results':results,'passed':all(r['passed'] for r in results),'exeSha256':sha(APP/'veyra_qml_ui.exe'),'donorUnchanged':len(manifest['files'])}
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
(BASE/'reports'/TASK/('gui-'+case+'-current.json')).write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
raise SystemExit(not receipt['passed'])

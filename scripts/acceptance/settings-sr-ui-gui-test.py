"""Actual production capture/node/inspector layouts, private profiles and runtime."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,time
import psutil
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='settings-sr-ui-20261010'
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
BUILD=BASE/'build'/TASK;APP=BASE/'tests'/TASK/'app'
FFMPEG=BASE/'deps/ffmpeg-subtitle-formats-20261009/installed/bin'
LOG=BASE/'logs'/TASK/('gui-'+time.strftime('%Y%m%d-%H%M%S'));LOG.mkdir(parents=True)
manifest=json.loads((DONOR/'package-manifest.json').read_text(encoding='utf8'))
def sha(p):return hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
def verify():
    for row in manifest['files']:assert sha(DONOR/row['path'])==row['sha256'],row['path']
def replace(source,target):
    target.parent.mkdir(parents=True,exist_ok=True);swap=target.with_name(target.name+'.swap');shutil.copy2(source,swap);os.replace(swap,target)
def write(target,content):
    swap=target.with_name(target.name+'.swap');swap.write_bytes(content);os.replace(swap,target)
verify()
for proc in psutil.process_iter(['name']):assert not (proc.info['name'] or '').lower().startswith(('veyra','fg-presentation','flash-pixel')),proc.info
assert APP.is_dir(),'Run settings-sr-ui-test.py first to stage the private app'
replace(BUILD/'veyra_qml_ui.exe',APP/'veyra_qml_ui.exe')
for source in (ROOT/'qml').rglob('*'):
    if source.is_file() and (source.suffix in ('.qml','.js') or source.name=='qmldir'):replace(source,APP/'qml'/source.relative_to(ROOT/'qml'))
for source in (BUILD/'shaders').glob('*.dxil'):replace(source,APP/'shaders'/source.name)
for source in FFMPEG.glob('*.dll'):replace(source,APP/source.name)
main=APP/'qml/Veyra/Main.qml';saved=main.read_bytes();results=[]
try:
    cases=[('zh-CN',1280,900,'1'),('en',1280,900,'1'),('zh-TW',900,800,'1'),('ja',900,800,'1'),('en',900,800,'1.5')]
    if sys.argv[1:]==['one']:cases=cases[:1]
    for language,width,height,scale in cases:
        name=f'{language}-{width}-{scale}';evidence=LOG/name;evidence.mkdir()
        tmp=BASE/'tmp'/TASK/LOG.name/name;tmp.mkdir(parents=True)
        profile=BASE/'tests'/TASK/'profiles'/LOG.name/name;profile.mkdir(parents=True)
        preferences=profile/'qml-preferences.v1.json'
        preferences.write_text(json.dumps({'language':language,'autoResume':False,'rememberPosition':False,
            'prewarmEnhancement':False,'gpuPriority':'normal','subtitleEnabled':False,
            'screenshotDir':str(evidence/'screenshots'),'sentinel':'preserved'}),encoding='utf8')
        env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
        env.update(PATH=str(APP)+os.pathsep+env['PATH'],TEMP=str(tmp),TMP=str(tmp),CUDA_CACHE_PATH=str(tmp/'cuda'),
            QML_DISK_CACHE_PATH=str(tmp/'qml-cache'),QT_FORCE_STDERR_LOGGING='1',QT_SCALE_FACTOR=scale,VEYRA_LOG_FILE=str(evidence/'engine.log'))
        loader='\nLoader { source: '+json.dumps((ROOT/'scripts/acceptance/settings-sr-ui.qml').as_uri())+'; onLoaded: { item.appRoot=root; item.dialogHost=dialogs; item.nodePage=nodePage; item.proPage=proPage; item.evidence='+json.dumps(evidence.as_posix(),ensure_ascii=False)+' } }\n'
        text=saved.decode('utf8');at=text.rfind('}');assert at>0;write(main,(text[:at]+loader+text[at:]).encode('utf8'))
        command=[str(APP/'veyra_qml_ui.exe'),'--data-dir',str(profile),'--page','home','--size',f'{width}x{height}','--exit-after','41000']
        began=time.monotonic()
        with (evidence/'console.log').open('wb') as stream:run=subprocess.run(command,cwd=APP,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=50)
        console=(evidence/'console.log').read_text(encoding='utf8',errors='replace')
        errors=[line for line in console.splitlines() if re.search(r'LAYOUT_UI_FAIL|ReferenceError:|TypeError:|\[ERROR\s*\]|\[FATAL\s*\]|D3D12 ERROR|D3D12 CORRUPTION|Cannot assign|Binding loop',line)]
        assert json.loads(preferences.read_text(encoding='utf8')).get('sentinel')=='preserved'
        passed=run.returncode==0 and not errors and 'LAYOUT_UI_PASS' in console
        row={'case':name,'command':command,'exit':run.returncode,'seconds':time.monotonic()-began,'passed':passed,'errors':errors,
            'events':[l for l in console.splitlines() if 'LAYOUT_UI_' in l],'profile':str(profile)}
        results.append(row);print(json.dumps({k:v for k,v in row.items() if k != "events"},ensure_ascii=False),flush=True)
        if not passed:break
finally:write(main,saved)
verify();receipt={'results':results,'passed':len(results)==len(cases) and all(r['passed'] for r in results),'exeSha256':sha(APP/'veyra_qml_ui.exe'),'donorUnchanged':len(manifest['files'])}
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
(BASE/'reports'/TASK/'gui-current.json').write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
raise SystemExit(not receipt['passed'])

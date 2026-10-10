"""Production QML setter and restart regression, isolated profile and existing private app."""
from pathlib import Path
import hashlib,json,os,re,subprocess,time
import psutil
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='inactive-effects-20261010'
APP=BASE/'tests'/TASK/'app';BUILD=BASE/'build'/TASK
LOG=BASE/'logs'/TASK/('gui-'+time.strftime('%Y%m%d-%H%M%S'));LOG.mkdir(parents=True)
profile=BASE/'tests'/TASK/'profiles'/LOG.name;profile.mkdir(parents=True)
preferences=profile/'qml-preferences.v1.json'
preferences.write_text(json.dumps({'language':'en','autoResume':False,'rememberPosition':False,'prewarmEnhancement':False,'gpuPriority':'normal','subtitleEnabled':False,'sentinel':'preserved'}),encoding='utf8')
for proc in psutil.process_iter(['name']):assert not (proc.info['name'] or '').lower().startswith(('veyra','fg-presentation','flash-pixel')),proc.info
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert sha(APP/'veyra_qml_ui.exe')==sha(BUILD/'veyra_qml_ui.exe')
main=APP/'qml/Veyra/Main.qml';saved=main.read_bytes();results=[]
def write(data):
    swap=main.with_name(main.name+'.swap');swap.write_bytes(data);os.replace(swap,main)
try:
    for phase in ('edit','restore','invalid'):
        if phase=='invalid':
            savedPrefs=json.loads(preferences.read_text(encoding='utf8'))
            savedPrefs.update(nrHoldStrength=2,nrHoldTolerance='invalid')
            preferences.write_text(json.dumps(savedPrefs),encoding='utf8')
        evidence=LOG/phase;evidence.mkdir();tmp=BASE/'tmp'/TASK/LOG.name/phase;tmp.mkdir(parents=True)
        loader='\nLoader { source: '+json.dumps((ROOT/'scripts/acceptance/inactive-effects.qml').as_uri())+'; onLoaded: item.phase="'+phase+'" }\n'
        text=saved.decode('utf8');at=text.rfind('}');assert at>0;write((text[:at]+loader+text[at:]).encode('utf8'))
        env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
        env.update(PATH=str(APP)+os.pathsep+env['PATH'],TEMP=str(tmp),TMP=str(tmp),CUDA_CACHE_PATH=str(tmp/'cuda'),QML_DISK_CACHE_PATH=str(tmp/'qml'),QT_FORCE_STDERR_LOGGING='1',VEYRA_LOG_FILE=str(evidence/'engine.log'))
        command=[str(APP/'veyra_qml_ui.exe'),'--data-dir',str(profile),'--page','pro','--size','1280x900','--exit-after','55000']
        start=time.monotonic()
        with (evidence/'console.log').open('wb') as stream:run=subprocess.run(command,cwd=APP,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=60)
        console=(evidence/'console.log').read_text(encoding='utf8',errors='replace')
        errors=[l for l in console.splitlines() if re.search(r'INACTIVE_UI_FAIL|ReferenceError:|TypeError:|\[ERROR\s*\]|\[FATAL\s*\]|D3D12 ERROR|D3D12 CORRUPTION|Cannot assign|Binding loop',l)]
        engine=(evidence/'engine.log').read_text(encoding='utf8',errors='replace')
        details={}
        if phase=='edit' and 'INACTIVE_UI_PASS edit' in console:
            middle=engine.split('BEGIN inactive UI edits',1)[1].split('END inactive UI edits',1)[0]
            revisions=re.findall(r'GPU untouched revision=(\d+)',middle)
            details={'storedEdits':len(revisions),'gpuRevisions':sorted(set(revisions)),'settingsResets':len(re.findall(r'\[reset-lifecycle\]',middle)),'graphUpdates':len(re.findall(r'\[graph\] initialized|\[nr-correction\] live|\[settings\] requested revision=',middle))}
            if len(revisions)<8 or len(set(revisions))!=1 or details['settingsResets'] or details['graphUpdates']:errors.append('GUI edits changed running GPU history or were not stored')
        assert json.loads(preferences.read_text(encoding='utf8')).get('sentinel')=='preserved'
        row={'phase':phase,'command':command,'exit':run.returncode,'seconds':time.monotonic()-start,'passed':run.returncode==0 and not errors and 'INACTIVE_UI_PASS '+phase in console,'errors':errors,**details}
        results.append(row);print(json.dumps(row,ensure_ascii=False),flush=True)
        if not row['passed']:break
finally:write(saved)
receipt={'results':results,'passed':len(results)==3 and all(r['passed'] for r in results),'profile':str(profile),'exeSha256':sha(APP/'veyra_qml_ui.exe'),'mainRestored':main.read_bytes()==saved}
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
(BASE/'reports'/TASK/'gui-current.json').write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
raise SystemExit(not receipt['passed'])

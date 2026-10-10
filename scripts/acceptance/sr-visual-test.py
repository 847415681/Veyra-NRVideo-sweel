"""Bounded actual GPU/QML regressions; shared private runtime, immutable donor."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,time
import psutil
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='sr-visual-quality-20261010'
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
BUILD=BASE/'build'/TASK;APP=BASE/'tests'/TASK/'app'
QT=Path('C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64')
FFMPEG=BASE/'deps/ffmpeg-subtitle-formats-20261009/installed/bin'
quality_only='--quality-only-concurrent' in sys.argv[1:]
cases=[v for v in sys.argv[1:] if v!='--quality-only-concurrent'];assert cases
assert not quality_only or all(re.fullmatch(r'quality-\d+-(still|moving)',v) for v in cases)
manifest=json.loads((DONOR/'package-manifest.json').read_text(encoding='utf8'))
def sha(p):return hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
def verify():
    for row in manifest['files']:assert sha(DONOR/row['path'])==row['sha256'],row['path']
def replace(source,target):
    target.parent.mkdir(parents=True,exist_ok=True);swap=target.with_name(target.name+'.swap');shutil.copy2(source,swap);os.replace(swap,target)
verify()
running=[proc.info for proc in psutil.process_iter(['pid','name']) if (proc.info['name'] or '').lower().startswith(('veyra','fg-presentation','flash-pixel'))]
def quality_headroom():
    if not running:return None
    assert quality_only and all(p['name'].lower()=='veyra_qml_ui.exe' for p in running),running
    query=subprocess.run(['nvidia-smi','--query-gpu=memory.free,utilization.gpu','--format=csv,noheader,nounits'],capture_output=True,text=True,check=True,timeout=10)
    free,util=[int(v.strip()) for v in query.stdout.strip().split(',')]
    assert free>=6000 and util<=20,{'freeMiB':free,'utilPercent':util,'running':running}
    return {'freeMiB':free,'utilPercent':util,'running':running,'performanceValid':False}
headroom=quality_headroom()
if not APP.exists():
    APP.mkdir(parents=True)
    for row in manifest['files']:
        rel=Path(row['path'])
        if rel.parts[0] in ('runtime_local','logs') or rel.name=='veyra_qml_ui.exe':continue
        target=APP/rel;target.parent.mkdir(parents=True,exist_ok=True);os.link(DONOR/rel,target)
    (APP/'runtime_local').mkdir(exist_ok=True)
for source in BUILD.glob('*.exe'):replace(source,APP/source.name)
for source in (ROOT/'qml').rglob('*'):
    if source.is_file() and (source.suffix in ('.qml','.js') or source.name=='qmldir'):replace(source,APP/'qml'/source.relative_to(ROOT/'qml'))
for source in (BUILD/'shaders').glob('*.dxil'):replace(source,APP/'shaders'/source.name)
for source in FFMPEG.glob('*.dll'):
    target=APP/source.name;swap=target.with_name(target.name+'.swap');os.link(source,swap);os.replace(swap,target)
for source in (DONOR/'runtime_local/amd/fidelityfx').glob('*.dll'):
    target=APP/'runtime_local/amd/fidelityfx'/source.name
    if not target.exists():
        target.parent.mkdir(parents=True,exist_ok=True);os.link(source,target)
    assert sha(source)==sha(target)
for source in (ROOT/'tests/qml/quick').glob('*.qml'):replace(source,APP/'qml-tests'/source.name)
log=BASE/'logs'/TASK/('tests-'+time.strftime('%Y%m%d-%H%M%S'));log.mkdir(parents=True,exist_ok=False)
results=[]
for case in cases:
    quality_headroom()
    output=log/case;output.mkdir();temporary=BASE/'tmp'/TASK/log.name/case;temporary.mkdir(parents=True)
    env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
    env.update(TEMP=str(temporary),TMP=str(temporary),CUDA_CACHE_PATH=str(temporary/'cuda'),QML_DISK_CACHE_PATH=str(temporary/'qml'),
        PATH=str(APP)+os.pathsep+str(QT/'bin')+os.pathsep+env['PATH'],QML_IMPORT_PATH=str(QT/'qml'),QT_FORCE_STDERR_LOGGING='1',VEYRA_EXPORT_TEST_ARTIFACTS=str(output))
    if case in ('slider','menu','components'):
        file={'slider':'tst_slider_value.qml','menu':'tst_menu_layout.qml','components':'tst_components.qml'}[case]
        env['QSG_RHI_BACKEND']='d3d12'
        command=[str(APP/'veyra_qml_quick_tests.exe'),'-input',str(APP/'qml-tests'/file),'-o','-,txt']
    elif case in ('sr-baseline','sr','sr-gta'):
        command=[str(APP/'veyra_sr_quality_tests.exe'),str(output)]+(['baseline'] if case=='sr-baseline' else ['natural','E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4'] if case=='sr-gta' else [])
    elif case.startswith('quality-'):
        seconds=case.split('-')[1];mode='-'.join(case.split('-')[2:]) if len(case.split('-'))>2 else 'still'
        command=[str(APP/'veyra_sr_quality_tests.exe'),str(output),'reference','E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4',seconds,mode]
    elif case=='inactive':command=[str(APP/'veyra_live_presentation_tests.exe'),'E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4',str(output),'--inactive-effects']
    elif case=='inactive-node':command=[str(APP/'veyra_live_presentation_tests.exe'),'E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4',str(output),'--inactive-effects-node']
    elif case=='chain':command=[str(APP/'veyra_effect_chain_tests.exe')]
    elif case=='contracts':command=[str(APP/'veyra_repair_contract_tests.exe')]
    elif case=='video':command=[str(APP/'veyra_live_presentation_tests.exe'),'E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4',str(output),'--settings-rebuild']
    elif case=='rollback':command=[str(APP/'veyra_live_presentation_tests.exe'),'E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4',str(output),'--reset-rollback']
    elif case=='presenter':command=[str(APP/'veyra_settings_transaction_tests.exe'),str(output)]
    elif case=='lifecycle':command=[str(APP/'veyra_present_sink_lifecycle_tests.exe'),str(output)]
    elif case=='hdr':command=[str(APP/'veyra_hdr_color_tests.exe')]
    elif case=='color':command=[str(APP/'veyra_capture_color_tests.exe')]
    elif case=='formats':command=[str(APP/'veyra_capture_format_selection_tests.exe')]
    else:raise AssertionError(case)
    start=time.monotonic()
    with (output/'console.log').open('wb') as stream:
        try:run=subprocess.run(command,cwd=APP,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=290);code=run.returncode
        except subprocess.TimeoutExpired:code=-1
    text=(output/'console.log').read_text(encoding='utf8',errors='replace')
    errors=[line for line in text.splitlines() if re.search(r'^FAIL |FAIL!|passed=0|ReferenceError:|TypeError:|Binding loop|Cannot assign|D3D12 ERROR|D3D12 CORRUPTION',line)]
    passed=code==0 and not errors
    row={'case':case,'command':command,'exit':code,'seconds':time.monotonic()-start,'passed':passed,'errors':errors,'events':[l for l in text.splitlines() if re.search('PASS|SR_CASE|SR_POLICY|SR_NATIVE|SR_PRECOLOR|SR_QUALITY|SR_REFERENCE|RESULT|SETTINGS_REUSE|SETTINGS_HDR|Totals:',l)]}
    results.append(row);print(json.dumps({k:v for k,v in row.items() if k!='events'}|{'eventCount':len(row['events'])},ensure_ascii=False),flush=True)
    if not passed:break
verify();receipt={'results':results,'passed':len(results)==len(cases) and all(r['passed'] for r in results),'qualityOnlyConcurrent':headroom,'donorUnchanged':len(manifest['files']),'executables':{p.name:sha(p) for p in APP.glob('*.exe')}}
(log/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
(BASE/'reports'/TASK/'tests-current.json').write_text(json.dumps({'receipt':str(log/'receipt.json')},indent=2),encoding='utf8')
raise SystemExit(not receipt['passed'])

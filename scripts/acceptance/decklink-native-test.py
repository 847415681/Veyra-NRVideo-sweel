"""Bounded serial native protocol, actual GPU pixels, legacy USB/audio checks."""
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,time
BASE=Path('E:/项目/Veyra');TASK='decklink-native-20261009';BUILD=BASE/'build'/TASK
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
FFMPEG=BASE/'deps/ffmpeg-subtitle-formats-20261009/installed/bin';QT=Path('C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64')
LOG=BASE/'logs'/TASK/('tests-'+time.strftime('%Y%m%d-%H%M%S'));TMP=BASE/'tmp'/TASK/LOG.name
for p in (LOG,TMP):p.mkdir(parents=True,exist_ok=False)
env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
env.update(PATH=os.pathsep.join(map(str,(FFMPEG,QT/'bin',DONOR)))+os.pathsep+env['PATH'],TEMP=str(TMP),TMP=str(TMP),CUDA_CACHE_PATH=str(TMP/'cuda'))
cases=[('native','decklink-tests',[]),('no-sdk','decklink-stub-tests',[]),('legacy-color','veyra_capture_color_tests',[]),('selection','veyra_capture_format_selection_tests',[]),('gpu','veyra_hdr_color_tests',[]),('devices','veyra_capture_tests',['--list']),('usb-rate','veyra_capture_tests',['--rate-test','capture:0:1:-1','0'])]
cases +=[(name,'veyra_capture_audio_tests',['--'+name]) for name in ('endpoint-switch','endpoint-loss','slow-start','transient','xbox-float-rtp')]
selected=sys.argv[1:]
if selected:assert set(selected)<={n for n,_,_ in cases};cases=[row for row in cases if row[0] in selected]
results=[]
for name,exe,extra in cases:
    if name=='usb-rate':
        # Verify this run's physical fixture before the existing rate test
        # converts its ordinal to the actual device + saved format identity.
        listing=subprocess.check_output([str(BUILD/'veyra_capture_tests.exe'),'--list'],cwd=BUILD,env=env,timeout=20)
        (LOG/'usb-rate-devices.log').write_bytes(listing)
        first=listing.splitlines()[0]
        assert first.startswith(b'VIDEO 0 KUHAIMI 27P ') and b'vid_048d&pid_c027' in first,first
    cmd=[str(BUILD/(exe+'.exe')),*extra];env['VEYRA_LOG_FILE']=str(LOG/(name+'-engine.log'));began=time.monotonic()
    with (LOG/(name+'.log')).open('wb') as stream:
        try:rc=subprocess.run(cmd,cwd=BUILD,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=290).returncode
        except subprocess.TimeoutExpired:rc=124
    output=(LOG/(name+'.log')).read_text(encoding='utf8',errors='replace');skipped=name=='endpoint-switch' and rc==77
    # RATE uses pass=0/1 as a phase index; only GPU records use it as a result.
    failed=bool(re.search(r'^FAIL\b|D3D12 ERROR|D3D12 CORRUPTION',output,re.M)) or (name=='gpu' and bool(re.search(r'\bpass=0\b',output)))
    ok=(rc==0 or skipped or name=='devices' and rc==1) and not failed
    if name=='gpu':ok=ok and len(re.findall(r'^DECKLINK_GPU .*pass=1',output,re.M))==24
    checks=sum(line.startswith('PASS ') or name=='gpu' and bool(re.search(r'\bpass=1\b',line)) for line in output.splitlines())
    row={'name':name,'command':cmd,'exit':rc,'seconds':time.monotonic()-began,'passed':bool(ok),'skipped':skipped,'checks':checks,'exeSha256':hashlib.sha256(Path(cmd[0]).read_bytes()).hexdigest(),'log':str(LOG/(name+'.log'))}
    results.append(row);(LOG/'receipt.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps(row,ensure_ascii=False),flush=True)
    if not ok:print(output[-6500:],flush=True);break
(BASE/'reports'/TASK/('tests-'+('-'.join(selected) if selected else 'all')+'-current.json')).write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
raise SystemExit(any(not r['passed'] for r in results))

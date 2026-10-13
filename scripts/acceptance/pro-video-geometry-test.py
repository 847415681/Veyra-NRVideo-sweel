"""Drive the real GUI in a private stage; compare QML video/transport to its HWND.

No pixel readback in the player, no changes to the donor/user profile. Runtime
files are hardlinked unchanged; changed QML is replaced atomically in this stage.
Use --baseline-ref with 'before' to retain a failing historical negative control.
"""
from pathlib import Path
import argparse, ctypes, hashlib, json, math, os, re, shutil, subprocess, sys, time
from ctypes import wintypes as W
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('variant',choices=('before','after'))
p.add_argument('--renderer',choices=('d3d12','software'),default='d3d12')
p.add_argument('--scale',choices=('1','1.5'),default='1')
p.add_argument('--artifacts-root',type=Path,default=Path('E:/项目/Veyra'))
p.add_argument('--task',default='pro-video-geometry-20261012')
p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[2])
p.add_argument('--donor-app',type=Path,required=True)
p.add_argument('--media',required=True)
p.add_argument('--baseline-ref')
p.add_argument('--case',action='append',dest='selected_cases',help='Run only these named layout cases')
p.add_argument('--page',choices=('pro','node'),default='pro')
opts=p.parse_args()
assert re.fullmatch(r'[a-z0-9-]+',opts.task),'unsafe artifact directory name'
assert opts.variant!='before' or opts.baseline_ref,'negative control requires an explicit Git reference'
B=opts.artifacts_root.resolve();T=opts.task;R=opts.source_root.resolve()
DONOR=opts.donor_app.resolve();APP=B/'tests'/T/'app'
assert DONOR!=APP and not APP.is_relative_to(R),'private stage must be separate from source and donor'
for category in ('reports','tests','logs','tmp'):(B/category/T).mkdir(parents=True,exist_ok=True)
VARIANT=opts.variant;MODE=opts.renderer;SCALE=opts.scale
O=B/'logs'/T/(VARIANT+'-'+MODE+'-'+SCALE+'-'+time.strftime('%Y%m%d-%H%M%S'));O.mkdir(parents=True)
TMP=B/'tmp'/T/O.name;TMP.mkdir(parents=True)
PROFILE=B/'tests'/T/('profile-'+VARIANT+'-'+MODE+'-'+SCALE);PROFILE.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def write(p,data):
    p.parent.mkdir(parents=True,exist_ok=True);tmp=p.with_name(p.name+'.swap');tmp.write_bytes(data);os.replace(tmp,p)
def no_player():
    found=subprocess.check_output(['powershell','-NoProfile','-Command',"Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'veyra_qml_ui.exe' } | Select-Object -ExpandProperty ProcessId"],text=True).split()
    assert not found,('player already running',found)
no_player()
if not APP.exists():
    for base,dirs,files in os.walk(DONOR):
        dirs[:]=[n for n in dirs if n not in ('logs','cache','data')]
        target=APP/Path(base).relative_to(DONOR);target.mkdir(parents=True,exist_ok=True)
        for name in files:os.link(Path(base)/name,target/name)
restore={}
for p in (R/'qml').rglob('*'):
    if p.is_file():
        target=APP/'qml'/p.relative_to(R/'qml');restore[target]=p.read_bytes();write(target,restore[target])
if VARIANT=='before':
    files=subprocess.check_output(['git','-C',str(R),'ls-tree','-r','--name-only',opts.baseline_ref,'qml'],text=True,encoding='utf8').splitlines()
    for relative in files:
        write(APP/relative,subprocess.check_output(['git','-C',str(R),'show',opts.baseline_ref+':'+relative]))
assert sha(APP/'veyra_qml_ui.exe')==sha(DONOR/'veyra_qml_ui.exe')
cases=[dict(name='wide-no-policy',width=1900,height=900),dict(name='wide-active-policy',width=1900,height=900,options=True),
 dict(name='normal-active-policy',width=1400,height=900,options=True),dict(name='normal-english',width=1400,height=900,options=True,language='en'),
 dict(name='compact-header',width=960,height=800,options=True,language='en'),dict(name='compact-transport',width=900,height=800,options=True),
 dict(name='minimum-width',width=720,height=800,options=True,language='ja'),dict(name='wide-inspector',width=1400,height=900,inspector=650,options=True),
 dict(name='traditional',width=1400,height=900,options=True,language='zh-TW'),dict(name='cinema-switch',width=1280,height=800,page='min'),
 dict(name='professional-return',width=1400,height=900,options=True),dict(name='fullscreen',width=1400,height=900,options=True,fullscreen=True),
 dict(name='fullscreen-return',width=1400,height=900,options=True)]
if opts.page=='node':
    cases=[dict(name='node-wide',width=1900,height=900),
        dict(name='node-compact',width=960,height=800,options=True),
        dict(name='node-english',width=1400,height=900,options=True,language='en'),
        dict(name='node-minimum',width=720,height=700,options=True,language='ja'),
        dict(name='node-tall-picture',width=1400,height=900,pictureHeight=600,options=True),
        dict(name='node-shrink-window',width=960,height=650,pictureHeight=600,options=True),
        dict(name='node-short-picture',width=1400,height=900,pictureHeight=180,options=True),
        dict(name='node-fullscreen',width=1400,height=900,fullscreen=True),
        dict(name='node-fullscreen-return',width=1400,height=900,options=True)]
if opts.selected_cases:
    selected=set(opts.selected_cases)
    assert selected.issubset({c['name'] for c in cases}),selected
    cases=[c for c in cases if c['name'] in selected]
MAIN=APP/'qml/Veyra/Main.qml';original=MAIN.read_bytes();driver=APP/'qml/Veyra/ProGeometryProbe.qml'
write(driver,(R/'scripts/acceptance'/('node-video-geometry-probe.qml' if opts.page=='node' else 'pro-video-geometry-probe.qml')).read_bytes())
text=original.decode('utf8');at=text.rfind('}')
loader='\n Loader { source:"ProGeometryProbe.qml"; onLoaded: { item.cases='+json.dumps(cases)+'; item.media='+json.dumps(opts.media,ensure_ascii=False)+' } }\n'
write(MAIN,(text[:at]+loader+text[at:]).encode('utf8'))
env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
env.update(TEMP=str(TMP),TMP=str(TMP),CUDA_CACHE_PATH=str(TMP/'cuda'),QML_DISK_CACHE_PATH=str(TMP/'qml'),QML_DISABLE_DISK_CACHE='1',QT_FORCE_STDERR_LOGGING='1',QT_SCALE_FACTOR=SCALE,VEYRA_LOG_FILE=str(O/'engine.log'))
if MODE=='software':env['QT_QUICK_BACKEND']='software'
else:env['VEYRA_UI_RHI']='d3d12'
U=ctypes.WinDLL('user32',use_last_error=True);U.SetProcessDpiAwarenessContext.argtypes=[ctypes.c_void_p];U.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
CALLBACK=ctypes.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
U.EnumWindows.argtypes=[CALLBACK,W.LPARAM];U.EnumChildWindows.argtypes=[W.HWND,CALLBACK,W.LPARAM]
U.GetWindowThreadProcessId.argtypes=[W.HWND,ctypes.POINTER(W.DWORD)];U.GetClassNameW.argtypes=[W.HWND,W.LPWSTR,ctypes.c_int]
U.GetParent.argtypes=[W.HWND];U.GetParent.restype=W.HWND
U.GetWindowRect.argtypes=[W.HWND,ctypes.POINTER(W.RECT)];U.ClientToScreen.argtypes=[W.HWND,ctypes.POINTER(W.POINT)]
U.GetDpiForWindow.argtypes=[W.HWND];U.GetDpiForWindow.restype=W.UINT
def native(pid):
    hits=[]
    @CALLBACK
    def visit(hwnd,_):
        p=W.DWORD();U.GetWindowThreadProcessId(hwnd,ctypes.byref(p))
        if p.value!=pid:return True
        cls=ctypes.create_unicode_buffer(256);U.GetClassNameW(hwnd,cls,256)
        if cls.value=='VeyraQmlVideoHost':hits.append(hwnd)
        return True
    @CALLBACK
    def top(hwnd,_):
        visit(hwnd,0);U.EnumChildWindows(hwnd,visit,0);return True
    U.EnumWindows(top,0);assert len(set(hits))==1,hits
    hwnd=hits[0];parent=U.GetParent(hwnd);r=W.RECT();U.GetWindowRect(hwnd,ctypes.byref(r));origin=W.POINT();U.ClientToScreen(parent,ctypes.byref(origin))
    pr=W.RECT();U.GetWindowRect(parent,ctypes.byref(pr))
    return dict(x=r.left-origin.x,y=r.top-origin.y,w=r.right-r.left,h=r.bottom-r.top,dpr=U.GetDpiForWindow(hwnd)/96,screen=[pr.left,pr.top,pr.right,pr.bottom])
rows=[];seen=set();console=O/'console.log';start=time.monotonic();rc=None;proc=None
try:
    with console.open('wb') as stream:
        command=[str(APP/'veyra_qml_ui.exe'),'--data-dir',str(PROFILE),'--page','home','--size','1400x900','--exit-after','70000']
        if MODE=='software':command.append('--obs-game-capture')
        if MODE=='software':command.append('--obs-game-capture')
        proc=subprocess.Popen(command,cwd=APP,env=env,stdout=stream,stderr=subprocess.STDOUT)
        while proc.poll() is None:
            if time.monotonic()-start>80:proc.kill();raise TimeoutError('GUI timeout')
            data=console.read_text('utf8',errors='replace')
            for raw in re.findall(r'PRO_GEOMETRY (\{[^\r\n]+\})',data):
                row=json.loads(raw)
                if row['index'] in seen:continue
                seen.add(row['index']);n=native(proc.pid);row['native']=n
                expected=row.get('area') or dict(x=0,y=0,w=row['window']['w'],h=row['window']['h'])
                row['hostDifference']={k:round(row['host'][k]-expected[k],4) for k in ('x','y','w','h')}
                dpr=row.get('qtDpr',n['dpr'])
                row['nativeDifference']={k:round(n[k]-math.floor(expected[k]*dpr),4) for k in ('x','y','w','h')}
                row['overlapDip']=round(max(0,row['host']['y']+row['host']['h']-row['bar']['y']),4) if 'bar' in row else 0
                row['passed']=all(abs(v)<=1 for v in row['nativeDifference'].values()) and all(abs(v)<.1 for v in row['hostDifference'].values())
                rows.append(row);print(row['name'],'hostDelta',row['hostDifference'],'overlapDip',row['overlapDip'],'pass',row['passed'],flush=True)
            time.sleep(.08)
        rc=proc.returncode
finally:
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:proc.wait(timeout=10)
        except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=10)
    for path,data in restore.items():write(path,data)
    driver.unlink(missing_ok=True)
logs=console.read_text('utf8',errors='replace')+'\n'+(O/'engine.log').read_text('utf8',errors='replace')
errors=[l for l in logs.splitlines() if re.search(r'PRO_GEOMETRY_FAIL|TypeError:|ReferenceError:|Binding loop|Cannot assign|D3D12 ERROR|D3D12 CORRUPTION|failed to load component|no root object',l)]
if 'interface renderer='+MODE not in logs:errors.append('Requested UI renderer was not observed')
completed=rc==0 and not errors and 'PRO_GEOMETRY_DONE' in logs and len(rows)==len(cases)
result=dict(variant=VARIANT,mode=MODE,scale=SCALE,exit=rc,seconds=round(time.monotonic()-start,2),errors=errors,rows=rows,
 passed=completed and all(r['passed'] for r in rows),completed=completed,
 mainRestored=MAIN.read_bytes()==restore[MAIN],exeSha256=sha(APP/'veyra_qml_ui.exe'),command=command,baselineRef=opts.baseline_ref)
(O/'receipt.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),'utf8')
(B/'reports'/T/(VARIANT+'-'+MODE+'-'+SCALE+'.json')).write_text(json.dumps(dict(receipt=str(O/'receipt.json')),ensure_ascii=False),'utf8')
print('RESULT',json.dumps(dict(passed=result['passed'],errors=errors,exit=rc,seconds=result['seconds'],receipt=str(O/'receipt.json')),ensure_ascii=False),flush=True)
raise SystemExit(not completed or (VARIANT!='before' and not result['passed']))

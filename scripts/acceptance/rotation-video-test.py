"""Sample the actual native video HWND while playing display-matrix fixtures.
Only this private test window is captured; product playback has no readback.
"""
from pathlib import Path
import argparse,ctypes,hashlib,json,os,re,shutil,subprocess,time
from ctypes import wintypes as W
from PIL import ImageGrab
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('variant',choices=('before','after'))
p.add_argument('--task',required=True)
p.add_argument('--donor-app',type=Path,required=True)
p.add_argument('--media-dir',type=Path,required=True)
p.add_argument('--stage-label',default='')
a=p.parse_args();assert re.fullmatch(r'[a-z0-9-]+',a.task)
assert not a.stage_label or re.fullmatch(r'[a-z0-9-]+',a.stage_label)
base=Path('E:/项目/Veyra');root=Path(__file__).resolve().parents[2]
stage=base/'tests'/a.task/('rotation-'+a.variant+('-'+a.stage_label if a.stage_label else ''));log=base/'logs'/a.task/('rotation-'+a.variant+'-'+time.strftime('%H%M%S'));log.mkdir()
temp=base/'tmp'/a.task/log.name;temp.mkdir(parents=True)
running=subprocess.check_output(['tasklist','/fo','csv','/nh'],text=True,encoding='mbcs',errors='replace')
assert not any(line.lower().startswith('"veyra') for line in running.splitlines()),'Another player is active'
if not stage.exists():
    for source in a.donor_app.rglob('*'):
        rel=source.relative_to(a.donor_app)
        if rel.parts[0] in ('logs','scratch') or (rel.parts[0]=='runtime_local' and len(rel.parts)>1 and rel.parts[1].startswith('user-data')):continue
        dest=stage/rel
        if source.is_dir():dest.mkdir(parents=True,exist_ok=True)
        else:
            dest.parent.mkdir(parents=True,exist_ok=True)
            if rel.parts[0]=='qml':shutil.copy2(source,dest)
            else:os.link(source,dest)
assert hashlib.sha256((stage/'veyra_qml_ui.exe').read_bytes()).digest()==hashlib.sha256((a.donor_app/'veyra_qml_ui.exe').read_bytes()).digest(),'Stage belongs to a different donor build'
qml=stage/'qml/Veyra/Main.qml';original=qml.read_bytes()
angles=(0,90,180,270,0)
paths=[str(a.media_dir/f'rotation{angle}.mp4') for angle in angles]
assert all(Path(path).is_file() for path in paths)
fixture=json.loads((a.media_dir/'rotation0.json').read_text('utf8'))['streams'][0]
codedAspect=fixture['width']/fixture['height']
probe=stage/'qml/Veyra/RotationVideoProbe.qml';shutil.copy2(root/'scripts/acceptance/rotation-video-probe.qml',probe)
text=original.decode('utf8');at=text.rfind('}')
qml.write_text(text[:at]+'\nLoader { source:"RotationVideoProbe.qml"; onLoaded:item.paths='+json.dumps(paths,ensure_ascii=False)+' }\n'+text[at:],'utf8')
profile=base/'tests'/a.task/('rotation-profile-'+a.variant+'-'+log.name);profile.mkdir()
(profile/'qml-preferences.v1.json').write_text(json.dumps(dict(autoResume=False,prewarmEnhancement=False,gpuPriority='normal',overlayCompat='off')),'utf8')
env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_','QML_'))}
env.update(TEMP=str(temp),TMP=str(temp),CUDA_CACHE_PATH=str(temp/'cuda'),QML_DISABLE_DISK_CACHE='1',QT_FORCE_STDERR_LOGGING='1',VEYRA_LOG_FILE=str(log/'engine.log'))
u=ctypes.WinDLL('user32',use_last_error=True)
u.SetProcessDpiAwarenessContext.argtypes=[ctypes.c_void_p];u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
callback=ctypes.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
u.EnumWindows.argtypes=[callback,W.LPARAM];u.EnumChildWindows.argtypes=[W.HWND,callback,W.LPARAM]
u.GetWindowThreadProcessId.argtypes=[W.HWND,ctypes.POINTER(W.DWORD)];u.GetClassNameW.argtypes=[W.HWND,W.LPWSTR,ctypes.c_int]
u.GetWindowRect.argtypes=[W.HWND,ctypes.POINTER(W.RECT)];u.GetParent.argtypes=[W.HWND];u.GetParent.restype=W.HWND
u.SetWindowPos.argtypes=[W.HWND,W.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,W.UINT]


def video_window(pid):
    found=[]
    @callback
    def visit(hwnd,_):
        owner=W.DWORD();u.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
        if owner.value==pid:
            name=ctypes.create_unicode_buffer(256);u.GetClassNameW(hwnd,name,256)
            if name.value=='VeyraQmlVideoHost':found.append(hwnd)
        return True
    @callback
    def top(hwnd,_):visit(hwnd,0);u.EnumChildWindows(hwnd,visit,0);return True
    u.EnumWindows(top,0);assert len(set(found))==1
    return found[0]


command=[str(stage/'veyra_qml_ui.exe'),'--data-dir',str(profile),'--page','min','--size','720x500','--exit-after','80000']
rows=[];seen=set();references=None;proc=None;rc=None;error='';start=time.monotonic()
try:
    with (log/'console.log').open('wb') as stream:
        proc=subprocess.Popen(command,cwd=stage,env=env,stdout=stream,stderr=subprocess.STDOUT)
        while proc.poll() is None:
            if time.monotonic()-start>90:raise TimeoutError('Rotation GUI deadline')
            text=(log/'console.log').read_text('utf8',errors='replace')
            for match in re.findall(r'GUI_ROTATION (\{[^\r\n]+\})',text):
                row=json.loads(match);index=row['index']
                if index in seen:continue
                seen.add(index);hwnd=video_window(proc.pid)
                u.SetWindowPos(u.GetParent(hwnd),W.HWND(-1),0,0,0,0,0x0013)
                time.sleep(.15)
                rect=W.RECT();assert u.GetWindowRect(hwnd,ctypes.byref(rect))
                image=ImageGrab.grab(bbox=(rect.left,rect.top,rect.right,rect.bottom),all_screens=True)
                image.save(log/f'rotation-{index}-{angles[index]}.png')
                points=[(.25,.25),(.75,.25),(.25,.75),(.75,.75)]
                values=[sum(image.getpixel((int(x*image.width),int(y*image.height)))[:3])/3 for x,y in points]
                if references is None:
                    references=values;assert all(references[k]-references[k+1]>25 for k in range(3)),values
                order={0:(0,1,2,3),90:(2,0,3,1),180:(3,2,1,0),270:(1,3,0,2)}[angles[index]]
                expected=[references[k] for k in order]
                expectedAspect=1/codedAspect if angles[index] in (90,270) else codedAspect
                aspectPassed=abs(row['aspect']-expectedAspect)<.001
                row.update(angle=angles[index],samples=values,expected=expected,expectedAspect=expectedAspect,aspectPassed=aspectPassed,
                           passed=aspectPassed and max(abs(x-y) for x,y in zip(values,expected))<15,
                           hwndExtent=[image.width,image.height])
                rows.append(row);print(json.dumps(row),flush=True)
            time.sleep(.08)
        rc=proc.returncode
except Exception as exc:
    error=type(exc).__name__+': '+str(exc);print(error,flush=True)
finally:
    if proc is not None and proc.poll() is None:
        proc.terminate();proc.wait(timeout=10)
    qml.write_bytes(original);probe.unlink(missing_ok=True)
text=(log/'console.log').read_text('utf8',errors='replace')
completed=not error and rc==0 and len(rows)==len(angles) and 'GUI_ROTATION_DONE' in text and 'GUI_ROTATION_FAIL' not in text
passed=completed and (all(row['passed'] for row in rows) if a.variant=='after' else all(row['passed'] for row in rows if row['angle']==0) and all(not row['passed'] for row in rows if row['angle']!=0))
result=dict(passed=passed,completed=completed,variant=a.variant,command=command,rows=rows,error=error,exit=rc,
            exeSha256=hashlib.sha256((stage/'veyra_qml_ui.exe').read_bytes()).hexdigest(),scope='Actual GUI/native video pixels, SDR synthetic display-matrix files; no NR/FG enabled')
(log/'receipt.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),'utf8')
(base/'reports'/a.task).mkdir(parents=True,exist_ok=True)
(base/'reports'/a.task/('rotation-'+a.variant+'.json')).write_text(json.dumps(dict(passed=passed,receipt=str(log/'receipt.json')),indent=2),'utf8')
print('RESULT',passed,log,flush=True)
raise SystemExit(not passed)

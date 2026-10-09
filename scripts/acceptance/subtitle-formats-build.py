"""Scoped build against verified current-main objects; never writes old caches."""
from pathlib import Path
import ctypes
import hashlib
import json
import os
import re
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
BASE=Path('E:/项目/Veyra');TASK='subtitle-formats-20261009'
CACHE=BASE/'build/vfg-next-20261007';OUT=BASE/'build'/TASK
LOG=BASE/'logs'/TASK/('build-'+time.strftime('%Y%m%d-%H%M%S'));TMP=BASE/'tmp'/TASK/'build'
for p in (OUT,LOG,TMP):p.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
shell=ctypes.WinDLL('shell32');shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p);ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p]
def args(s):
    n=ctypes.c_int();p=shell.CommandLineToArgvW(s,ctypes.byref(n));a=[p[i] for i in range(n.value)];ctypes.windll.kernel32.LocalFree(p);return a
def unescape(s):return s.replace('$ ',' ').replace('$:',':').replace('$$','$')
tool=re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.*)$',(CACHE/'CMakeCache.txt').read_text(encoding='utf8'),re.M).group(1)
ninja=(CACHE/'build.ninja').read_text(encoding='utf8');inputs={};sources={};commands=[];start=time.monotonic()
def protect(p):
    p=Path(p);p=p if p.is_absolute() else CACHE/p
    assert p.is_file(),p
    if str(p) not in inputs:inputs[str(p)]=sha(p)
    return p
def rule(name,objectsRequired=True):
    lines=re.search(r'^build '+re.escape(name)+r': .*?(?=\n\n)',ninja,re.M|re.S).group().splitlines()
    var=dict(re.match(r'  ([A-Z_]+) = (.*)',l).groups() for l in lines[1:] if re.match(r'  ([A-Z_]+) = (.*)',l))
    objects=[unescape(n).replace('\\','/') for n in lines[0].split('Release ',1)[1].split(' | ',1)[0].split(' ')]
    if objectsRequired:
        for p in objects:protect(p)
    for value in args(unescape(var.get('LINK_LIBRARIES',''))):
        if value.lower().endswith('.lib') and (Path(value).is_file() or (CACHE/value).is_file()):protect(value)
    return objects,var
def compiler(name):
    return args(subprocess.check_output([tool,'-t','commands',name],cwd=CACHE,text=True,encoding='utf8').splitlines()[-1])
env=dict(os.environ);env.update(TEMP=str(TMP),TMP=str(TMP))
setup=subprocess.run(['cmd.exe','/u','/d','/c',r'call C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && set'],capture_output=True,text=True,encoding='utf-16-le',env=env,check=True)
for line in setup.stdout.splitlines():
    if '=' in line:
        key,value=line.split('=',1)
        if key and not key.startswith('='):env[key]=value
env.update(TEMP=str(TMP),TMP=str(TMP))
def run(command,name):
    commands.append(command);remaining=890-(time.monotonic()-start);assert remaining>0
    with (LOG/(name+'.log')).open('wb') as stream:r=subprocess.run(command,cwd=CACHE,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=min(300,remaining))
    print(name,'exit',r.returncode,flush=True)
    if r.returncode:print((LOG/(name+'.log')).read_text(encoding='utf8',errors='replace')[-5000:]);raise SystemExit(r.returncode)
def compile(source,name,a):
    sources[str(source)]=sha(source);target=OUT/(name+'.obj')
    command=a[:1]+['/I'+str(ROOT/'include')]+[v for v in a[1:-1] if not v.startswith(('/Fo','/Fd')) and v!='/showIncludes']
    run(command+['/Fo'+str(target),'/Fd'+str(OUT/(name+'.pdb')),str(source)],'compile-'+name)
    return target

# The cached CMake build predates HDR compatibility and the BMD follow-up.
# Retain ALL current-main overrides, verifying sources and immutable objects.
previous=json.loads((BASE/'logs/hdr-fg-toggle-20261009/build-20261009-113817/receipt.json').read_text(encoding='utf8'))
overrides={}
for name,row in previous['overrides'].items():
    if name.endswith(('QmlPlayerBridge.cpp.obj','qrc_veyra_i18n.cpp.obj')):continue
    source=name.split('.dir/',1)[1][:-4]
    if source.startswith(('src/','apps/')):
        original=next((p for p in previous['sources'] if p.replace('\\','/').endswith('/'+source)),None)
        assert original and sha(ROOT/source)==previous['sources'][original],source
        sources[str(ROOT/source)]=sha(ROOT/source)
    assert sha(Path(row['path']))==row['sha256'],name
    overrides[name]=protect(row['path'])
for relative in ('src/engine/Subtitles.cpp','src/ui/QmlPlayerBridge.cpp'):
    group='veyra_engine' if relative.startswith('src/engine/') else 'veyra_qml_bridge'
    name=f'CMakeFiles/{group}.dir/{relative}.obj'
    overrides[name]=compile(ROOT/relative,Path(relative).stem,compiler(name))
sources[str(ROOT/'include/veyra/engine/Subtitles.h')]=sha(ROOT/'include/veyra/engine/Subtitles.h')

gui=rule('veyra_qml_ui.exe')
resource=next(p for p in gui[0] if p.endswith('/qrc_veyra_i18n.cpp.obj'))
lines=subprocess.check_output([tool,'-t','commands',resource],cwd=CACHE,text=True,encoding='utf8').splitlines()
rccLine=next(l for l in lines if 'rcc.exe' in l.lower() and 'veyra_i18n' in l)
rcc=next(a for part in args(rccLine) for a in args(part) if a.lower().endswith('rcc.exe'))
qrc=OUT/'i18n.qrc';qrc.write_text('<RCC><qresource prefix="/i18n"><file alias="catalog.json">'+(ROOT/'i18n/catalog.json').as_posix()+'</file></qresource></RCC>',encoding='utf8')
generated=OUT/'qrc_veyra_i18n.cpp';run([rcc,'--name','veyra_i18n','--no-zstd','-o',str(generated),str(qrc)],'generate-i18n')
sources[str(ROOT/'i18n/catalog.json')]=sha(ROOT/'i18n/catalog.json')
overrides[resource]=compile(generated,'qrc_veyra_i18n',compiler(resource))
core=[p for name,p in overrides.items() if name.startswith(('CMakeFiles/veyra_engine.','CMakeFiles/veyra_sources.'))]
assert any(p.name=='Subtitles.obj' for p in core) and len(core)==11

cmake=r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
link=r'C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Tools\MSVC\1444~1.352\bin\Hostx64\x64\link.exe'
executables=[]
def linkTarget(name,objects,variables):
    rsp=OUT/(name+'.rsp')
    rsp.write_text('\n'.join(subprocess.list2cmdline([str(p)]) for p in objects)+'\n'+unescape(variables.get('LINK_PATH',''))+'\n'+unescape(variables['LINK_LIBRARIES']),encoding='utf8')
    directory=OUT/('manifest-'+name);directory.mkdir(exist_ok=True)
    flags=[('/MANIFESTINPUT:'+str(CACHE/'veyra-qml.manifest')) if a.startswith('/MANIFESTINPUT:') else a for a in args(unescape(variables.get('LINK_FLAGS','')))]
    protect(CACHE/'veyra-qml.manifest')
    command=[cmake,'-E','vs_link_exe','--msvc-ver=1944','--intdir='+str(directory),r'--rc=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\rc.exe',r'--mt=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\mt.exe','--manifests','--',link,'/nologo','@'+str(rsp),'/out:'+str(OUT/(name+'.exe')),'/implib:'+str(OUT/(name+'.lib')),'/pdb:'+str(OUT/(name+'.pdb')),'/version:0.0']+flags
    run(command,'link-'+name);executables.append(OUT/(name+'.exe'))
linkTarget('veyra_qml_ui',list(overrides.values())+[CACHE/p for p in gui[0] if p not in overrides],gui[1])
for target in ('bitmap','loading','text','overlay'):
    objectNames,var=rule(f'veyra_{"bitmap_subtitle" if target=="bitmap" else "subtitle_"+target}_tests.exe',False)
    assert len(objectNames)==1,objectNames
    source=ROOT/objectNames[0].split('.dir/',1)[1][:-4]
    obj=compile(source,'test-'+target,compiler(objectNames[0]))
    linkTarget('subtitle-'+target+'-tests',core+[obj],var)
objects,var=rule('veyra_bitmap_subtitle_tests.exe',False)
obj=compile(ROOT/'tests/integration/SubtitleFormatTests.cpp','test-formats',compiler(objects[0]))
linkTarget('subtitle-format-tests',core+[obj],var)
for p,d in inputs.items():assert sha(Path(p))==d,p
for relative in ('CMakeLists.txt','qml/Veyra/DialogHost.qml'):sources[str(ROOT/relative)]=sha(ROOT/relative)
receipt={'commands':commands,'sources':sources,'overrides':{n:{'path':str(p),'sha256':sha(p)} for n,p in overrides.items()},'inputsUnchanged':inputs,'seconds':time.monotonic()-start,'executables':{p.name:sha(p) for p in executables}}
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
(BASE/'reports'/TASK/'build-current.json').write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
print('Built current-main GUI and subtitle tests; original inputs unchanged;',LOG,flush=True)

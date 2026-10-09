"""Build only the two capture sources and related tests; preserve cached inputs."""
from pathlib import Path
import ctypes,hashlib,json,os,re,subprocess,time,sys
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='blackmagic-followup-20261009'
CACHE=BASE/'build/vfg-next-20261007';OUT=BASE/'build'/TASK;LOG=BASE/'logs'/TASK/('build-'+time.strftime('%Y%m%d-%H%M%S'));TMP=BASE/'tmp'/TASK/'build'
for p in (OUT,LOG,TMP):p.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/bmd-followup-control.py')],check=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
shell=ctypes.WinDLL('shell32');shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)];shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p);ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p]
def args(s):
 n=ctypes.c_int();p=shell.CommandLineToArgvW(s,ctypes.byref(n));a=[p[i] for i in range(n.value)];ctypes.windll.kernel32.LocalFree(p);return a
def unescape(s):return s.replace('$ ', ' ').replace('$:', ':').replace('$$','$')
ninja=(CACHE/'build.ninja').read_text(encoding='utf8');settings=(CACHE/'CMakeCache.txt').read_text(encoding='utf8')
tool=re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.*)$',settings,re.M).group(1)
env=dict(os.environ);env.update(TEMP=str(TMP),TMP=str(TMP))
setup=subprocess.run(['cmd.exe','/u','/d','/c',r'call C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && set'],capture_output=True,text=True,encoding='utf-16-le',env=env,check=True)
for line in setup.stdout.splitlines():
 if '=' in line:
  k,v=line.split('=',1)
  if k and not k.startswith('='):env[k]=v
env.update(TEMP=str(TMP),TMP=str(TMP))
inputs={};commands=[];started=time.monotonic()
def protect(p):
 p=Path(p);p=p if p.is_absolute() else CACHE/p
 assert p.is_file(),p
 if str(p) not in inputs:inputs[str(p)]=sha(p)
 return p
def rule(name,protectObjects=True):
 lines=re.search(r'^build '+re.escape(name)+r': .*?(?=\n\n)',ninja,re.M|re.S).group().splitlines()
 variables=dict(re.match(r'  ([A-Z_]+) = (.*)',line).groups() for line in lines[1:] if re.match(r'  ([A-Z_]+) = (.*)',line))
 objects=[(protect(unescape(n)) if protectObjects else CACHE/unescape(n)) for n in lines[0].split('Release ',1)[1].split(' | ',1)[0].split(' ')]
 for a in args(unescape(variables['LINK_LIBRARIES'])):
  if a.lower().endswith('.lib') and (Path(a).is_file() or (CACHE/a).is_file()):protect(a)
 return objects,variables
gui=rule('veyra_qml_ui.exe');color=rule('veyra_capture_color_tests.exe',False);capture=rule('veyra_capture_tests.exe',False);gpu=rule('veyra_hdr_color_tests.exe',False)
for p in (CACHE/'veyra_qml_ui.exe',CACHE/'veyra-qml.manifest',CACHE/'build.ninja',CACHE/'CMakeCache.txt'):protect(p)
(LOG/'inputs-before.json').write_text(json.dumps(inputs,indent=2),encoding='utf8')
def run(cmd,name):
 assert time.monotonic()-started<880
 commands.append(cmd);t=time.monotonic()
 with (LOG/(name+'.log')).open('w',encoding='utf8') as f:r=subprocess.run(cmd,cwd=CACHE,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=min(300,890-(time.monotonic()-started)))
 print(name,'exit',r.returncode,'seconds',round(time.monotonic()-t,2),flush=True)
 if r.returncode:print((LOG/(name+'.log')).read_text(encoding='utf8',errors='replace')[-2500:]);raise SystemExit(r.returncode)
def compile(source,name,objectRule,extra=()):
 # -t commands also prints order-only dependency generators. Select only the
 # requested object's compiler line; never execute those cached-tree writers.
 command=subprocess.check_output([tool,'-t','commands',objectRule],cwd=CACHE,encoding='utf8').splitlines()[-1];a=args(command)
 assert a[0].lower().endswith('cl.exe') and '/Fo'+objectRule.replace('/','\\') in a,(a[0],objectRule)
 # The old source path is the final argument, all outputs are redirected to E task storage.
 a=a[:-1];a=[v for v in a if not v.startswith(('/Fo','/Fd')) and v!='/showIncludes']
 target=OUT/(name+'.obj');a=a[:1]+['/I'+str(ROOT/'include'),'/I'+str(ROOT/'tests/unit')]+a[1:]+list(extra)+['/Fo'+str(target),'/Fd'+str(OUT/(name+'.pdb')),str(source)]
 run(a,'compile-'+name);return target
sinkRule=r'CMakeFiles/veyra_sources.dir/src/source/NativeCaptureSink.cpp.obj'
sourceRule=r'CMakeFiles/veyra_sources.dir/src/source/CaptureCardSource.cpp.obj'
testRule=r'CMakeFiles/veyra_capture_color_tests.dir/tests/unit/CaptureColorContractTests.cpp.obj'
sink=compile(ROOT/'src/source/NativeCaptureSink.cpp','NativeCaptureSink',sinkRule)
source=compile(ROOT/'src/source/CaptureCardSource.cpp','CaptureCardSource',sourceRule)
test=compile(ROOT/'tests/unit/CaptureColorContractTests.cpp','capture-contract',testRule)
field=compile(ROOT/'tests/integration/CaptureSourceTests.cpp','capture-source-fixture',r'CMakeFiles/veyra_capture_tests.dir/tests/integration/CaptureSourceTests.cpp.obj')
gpuObject=gpu[0][0];gpuSource=str(gpuObject.relative_to(CACHE)).replace('\\','/').split('.dir/',1)[1][:-4]
gpuFixture=compile(ROOT/gpuSource,'capture-gpu-fixture',str(gpuObject.relative_to(CACHE)).replace('\\','/'))
old=TMP/'NativeCaptureSink-original.cpp';old.write_bytes(subprocess.check_output(['git','show','dbfceab:src/source/NativeCaptureSink.cpp'],cwd=ROOT))
oldSink=compile(old,'NativeCaptureSink-original',sinkRule)
oldTest=compile(ROOT/'tests/unit/CaptureColorContractTests.cpp','capture-contract-red',testRule,['/DVEYRA_TEST_BMD_LEGACY_FACTORY=1'])
link=r'C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Tools\MSVC\1444~1.352\bin\Hostx64\x64\link.exe'
cmake=r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
def linkexe(name,entry,overrides,replaceObjects=None):
 objects,var=entry;objects=objects if replaceObjects is None else replaceObjects
 response=OUT/(name+'.rsp');response.write_text('\n'.join(subprocess.list2cmdline([str(p)]) for p in overrides+objects)+'\n'+unescape(var.get('LINK_PATH',''))+'\n'+unescape(var['LINK_LIBRARIES']),encoding='utf8')
 exe=OUT/(name+'.exe');manifest=OUT/('manifest-'+name);manifest.mkdir(exist_ok=True)
 flags=[('/MANIFESTINPUT:'+str(CACHE/'veyra-qml.manifest')) if a.startswith('/MANIFESTINPUT:') else a for a in args(unescape(var.get('LINK_FLAGS','')))]
 run([cmake,'-E','vs_link_exe','--msvc-ver=1944','--intdir='+str(manifest),r'--rc=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\rc.exe',r'--mt=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\mt.exe','--manifests','--',link,'/nologo','@'+str(response),'/out:'+str(exe),'/implib:'+str(OUT/(name+'.lib')),'/pdb:'+str(OUT/(name+'.pdb')),'/version:0.0']+flags,'link-'+name)
 return exe
red=linkexe('capture-contract-red',color,[oldSink],[oldTest])
green=linkexe('capture-contract',color,[sink,source],[test])
physical=linkexe('capture-source',capture,[sink,source],[field])
app=linkexe('veyra-bmd-followup',gui,[sink,source])
gpuExe=linkexe('capture-gpu',gpu,[sink,source],[gpuFixture])
for p,d in inputs.items():assert sha(Path(p))==d,p
sources={str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for p in [ROOT/'src/source/NativeCaptureSink.cpp',ROOT/'src/source/CaptureCardSource.cpp',ROOT/'include/veyra/source/NativeCaptureSink.h',ROOT/'tests/unit/CaptureColorContractTests.cpp',ROOT/'tests/unit/BmdAllocatorCases.h']}
receipt={'baseMain':'dbfceabee804cd81bac4a4781f0b7b4cf272902e','cachedProduct':'71723e0249a17764696796fd204a6eb69d6da276','commands':commands,'inputsUnchanged':inputs,'sources':sources,'objects':{str(p):sha(p) for p in (sink,source,test,oldSink,oldTest,field,gpuFixture)},'originalSinkSha256':sha(old),'executables':{str(p):{'sha256':sha(p),'bytes':p.stat().st_size} for p in (red,green,physical,app,gpuExe)},'seconds':time.monotonic()-started}
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
(BASE/'reports'/TASK/'build-current.json').write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/bmd-followup-control.py')],check=True)
print('BMD build and original input preservation passed',flush=True)

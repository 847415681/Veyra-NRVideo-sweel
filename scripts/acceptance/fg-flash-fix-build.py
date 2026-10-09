"""Build isolated presenter/layout dependents against immutable current libraries."""
from pathlib import Path
import ctypes,hashlib,json,os,re,subprocess,sys,time

SRC=Path(__file__).resolve().parents[2]
BASE=Path('E:/项目/Veyra');TASK='fg-flash-fix-20261009'
BUILD=BASE/'build/vfg-next-20261007';OUT=BASE/'build'/TASK;LOG=BASE/'logs'/TASK/'build';TMP=BASE/'tmp'/TASK
for p in (OUT,LOG,TMP):p.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
shell32=ctypes.WinDLL('shell32',use_last_error=True)
shell32.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
shell32.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p]
def args(text):
 n=ctypes.c_int();p=shell32.CommandLineToArgvW(text,ctypes.byref(n));result=[p[i] for i in range(n.value)];ctypes.windll.kernel32.LocalFree(p);return result
def unescape(s):return s.replace('$ ', ' ').replace('$:', ':').replace('$$', '$')
ninja=(BUILD/'build.ninja').read_text(encoding='utf-8')
gui=re.search(r'^build veyra_qml_ui.exe: .*?(?=\n\n)',ninja,re.M|re.S).group().splitlines()
variables=dict(re.match(r'  ([A-Z_]+) = (.*)',line).groups() for line in gui[1:] if re.match(r'  ([A-Z_]+) = (.*)',line))
guiobjects=[BUILD/unescape(n) for n in gui[0].split('Release ',1)[1].split(' | ',1)[0].split(' ')]
original=json.loads((BASE/'reports/fg-source-flash-20261008/native-build-inputs.json').read_text(encoding='utf8'))
native=args(original[1].split(' -- ',1)[1].rsplit(' && cd .',1)[0])
inputs={}
for p in guiobjects+[BUILD/'veyra_qml_ui.exe',BUILD/'veyra-qml.manifest']:
 assert p.is_file(),p;inputs[str(p)]=sha(p)
for item in args(unescape(variables['LINK_LIBRARIES']))+native:
 if item.lower().endswith('.lib'):
  p=Path(item);p=p if p.is_absolute() else BUILD/p
  if p.is_file():inputs[str(p)]=sha(p)
(LOG/'inputs-before.json').write_text(json.dumps(inputs,indent=2),encoding='utf8')
env=dict(os.environ);env.update(TEMP=str(TMP),TMP=str(TMP))
vcvars=r'C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat'
setup=subprocess.run(['cmd.exe','/u','/d','/c','call '+vcvars+' >nul && set'],capture_output=True,text=True,encoding='utf-16-le',check=True,env=env)
for line in setup.stdout.splitlines():
 if '=' in line:
  key,value=line.split('=',1)
  if key and not key.startswith('='):env[key]=value
env.update(TEMP=str(TMP),TMP=str(TMP))
commands=[]
def run(command,name):
 commands.append(command);start=time.monotonic()
 with (LOG/(name+'.log')).open('w',encoding='utf8') as stream:
  r=subprocess.run(command,cwd=BUILD,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=300)
 print(name,'exit',r.returncode,'seconds',round(time.monotonic()-start,2),flush=True)
 if r.returncode:raise SystemExit(r.returncode)
compile_args=args(original[0])
def compile(source,name):
 target=OUT/(name+'.obj')
 command=compile_args[:1]+['/I'+str(SRC/'include')]+[a for a in compile_args[1:] if not a.startswith(('/Fo','/Fd')) and a!='/showIncludes' and not a.endswith('FgPresentationTests.cpp')]
 command+=['/Fo'+str(target),'/Fd'+str(OUT/(name+'.pdb')),str(source)]
 run(command,'compile-'+name);return target
only_tests=sys.argv[1:]==['--tests-only']
objects=[OUT/(file[:-4]+'.obj') for file in ('VideoPresenter.cpp','EngineController.cpp','EngineControllerImage.cpp')] if only_tests else [compile(SRC/'src/engine'/file,file[:-4]) for file in ('VideoPresenter.cpp','EngineController.cpp','EngineControllerImage.cpp')]
if only_tests:
 before=json.loads((LOG/'receipt.json').read_text(encoding='utf8'))
 for rel,digest in before['sources'].items():
  if rel.startswith(('src/','include/')):assert sha(SRC/rel)==digest,rel
test=compile(SRC/'tests/integration/FgPresentationTests.cpp','fg-presentation-tests')
native_link=[]
native_exe=OUT/'fg-presentation-tests.exe'
for a in native:
 if a.endswith('FgPresentationTests.cpp.obj'):native_link+=list(map(str,objects+[test]))
 elif a.startswith('/out:'):native_link.append('/out:'+str(native_exe))
 elif a.startswith('/implib:'):native_link.append('/implib:'+str(OUT/'fg-tests.lib'))
 elif a.startswith('/pdb:'):native_link.append('/pdb:'+str(OUT/'fg-tests.pdb'))
 else:native_link.append(a)
run(native_link,'link-native')
if only_tests:
 for p,digest in inputs.items():assert sha(Path(p))==digest,p
 receipt={'commands':commands,'exeSha256':sha(native_exe),'guiUnchanged':sha(OUT/'veyra-hdr-flash-fix.exe'),'fixtureSha256':sha(SRC/'tests/integration/HdrCopyPresentationFixture.h'),'productObjectsReused':{str(p):sha(p) for p in objects},'inputsUnchanged':inputs}
 (LOG/'native-receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8');print('Native test rebuilt; GUI unchanged',flush=True);raise SystemExit(0)
response=OUT/'gui-link.rsp'
response.write_text('\n'.join(subprocess.list2cmdline([str(p)]) for p in objects+guiobjects)+'\n'+unescape(variables.get('LINK_PATH',''))+'\n'+unescape(variables['LINK_LIBRARIES']),encoding='utf8')
exe=OUT/'veyra-hdr-flash-fix.exe';(OUT/'manifest').mkdir(exist_ok=True)
link=r'C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Tools\MSVC\1444~1.352\bin\Hostx64\x64\link.exe'
cmake=r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
flags=[('/MANIFESTINPUT:'+str(BUILD/'veyra-qml.manifest')) if a.startswith('/MANIFESTINPUT:') else a for a in args(unescape(variables['LINK_FLAGS']))]
run([cmake,'-E','vs_link_exe','--msvc-ver=1944','--intdir='+str(OUT/'manifest'),r'--rc=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\rc.exe',r'--mt=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\mt.exe','--manifests','--',link,'/nologo','@'+str(response),'/out:'+str(exe),'/implib:'+str(OUT/'gui.lib'),'/pdb:'+str(OUT/'gui.pdb'),'/version:0.0']+flags,'link-gui')
for p,digest in inputs.items():assert sha(Path(p))==digest,p
receipt={'baseProduct':'71723e0249a17764696796fd204a6eb69d6da276','baseMain':'dbfceabee804cd81bac4a4781f0b7b4cf272902e','commands':commands,'inputsUnchanged':inputs,'sources':{str(p.relative_to(SRC)):sha(p) for p in [SRC/'include/veyra/engine/VideoPresenter.h']+[SRC/'src/engine'/f for f in ('VideoPresenter.cpp','EngineController.cpp','EngineControllerImage.cpp')]+[SRC/'tests/integration/FgPresentationTests.cpp']},'executables':{str(p):{'sha256':sha(p),'bytes':p.stat().st_size} for p in (exe,native_exe)}}
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
print('Build and immutable input verification passed',flush=True)

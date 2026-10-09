"""Build all cached consumers of the changed presentation headers, without writing the cache."""
from pathlib import Path
import ctypes, hashlib, json, os, re, subprocess, sys, time

ROOT=Path(__file__).resolve().parents[2]
BASE=Path('E:/项目/Veyra'); TASK='hdr-fg-toggle-20261009'
CACHE=BASE/'build/vfg-next-20261007'; OUT=BASE/'build'/TASK
LOG=BASE/'logs'/TASK/('build-'+time.strftime('%Y%m%d-%H%M%S')); TMP=BASE/'tmp'/TASK/'build'
for p in (OUT,LOG,TMP):p.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
shell=ctypes.WinDLL('shell32'); shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p); ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p]
def args(s):
 n=ctypes.c_int();p=shell.CommandLineToArgvW(s,ctypes.byref(n));a=[p[i] for i in range(n.value)];ctypes.windll.kernel32.LocalFree(p);return a
def unescape(s):return s.replace('$ ', ' ').replace('$:', ':').replace('$$','$')
ninja=(CACHE/'build.ninja').read_text(encoding='utf8')
tool=re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.*)$',(CACHE/'CMakeCache.txt').read_text(encoding='utf8'),re.M).group(1)
inputs={}; sources={}; commands=[]; start=time.monotonic()
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
 for a in args(unescape(var.get('LINK_LIBRARIES',''))):
  if a.lower().endswith('.lib') and (Path(a).is_file() or (CACHE/a).is_file()):protect(a)
 return objects,var
gui=rule('veyra_qml_ui.exe')
original=json.loads((BASE/'reports/fg-source-flash-20261008/native-build-inputs.json').read_text(encoding='utf8'))
native=args(original[1].split(' -- ',1)[1].rsplit(' && cd .',1)[0])
for a in native:
 if a.lower().endswith('.lib') and not a.startswith('/') and (Path(a).is_file() or (CACHE/a).is_file()):protect(a)
for p in ('build.ninja','CMakeCache.txt','veyra_qml_ui.exe','veyra-qml.manifest'):protect(CACHE/p)
env=dict(os.environ);env.update(TEMP=str(TMP),TMP=str(TMP))
setup=subprocess.run(['cmd.exe','/u','/d','/c',r'call C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && set'],capture_output=True,text=True,encoding='utf-16-le',env=env,check=True)
for line in setup.stdout.splitlines():
 if '=' in line:
  k,v=line.split('=',1)
  if k and not k.startswith('='):env[k]=v
env.update(TEMP=str(TMP),TMP=str(TMP))
def run(cmd,name):
 commands.append(cmd);t=time.monotonic();remaining=890-(t-start);assert remaining>0
 with (LOG/(name+'.log')).open('w',encoding='utf8') as stream:r=subprocess.run(cmd,cwd=CACHE,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=min(300,remaining))
 print(name,'exit',r.returncode,'seconds',round(time.monotonic()-t,2),flush=True)
 if r.returncode:print((LOG/(name+'.log')).read_text(encoding='utf8',errors='replace')[-3000:]);raise SystemExit(r.returncode)
def compiler(objectRule):
 line=subprocess.check_output([tool,'-t','commands',objectRule],cwd=CACHE,text=True,encoding='utf8').splitlines()[-1]
 a=args(line);assert a[0].lower().endswith('cl.exe'),line;return a
def compileSource(source,name,a):
 target=OUT/(name+'.obj');sources[str(source)]=sha(source)
 command=a[:1]+['/I'+str(ROOT/'include'),'/I'+str(ROOT/'tests/unit')]+[v for v in a[1:-1] if not v.startswith(('/Fo','/Fd')) and v!='/showIncludes']
 command+=['/Fo'+str(target),'/Fd'+str(OUT/(name+'.pdb')),str(source)]
 run(command,'compile-'+name);return target
if '--tests-only' in sys.argv:
 previous=json.loads((BASE/'reports'/TASK/'build-current.json').read_text(encoding='utf8'))
 previous=json.loads(Path(previous['receipt']).read_text(encoding='utf8'))
 for p,d in previous['sources'].items():
  if not p.endswith('FgPresentationTests.cpp'):assert sha(Path(p))==d,p
 for row in previous['overrides'].values():assert sha(Path(row['path']))==row['sha256'],row['path']
 guiSha=sha(OUT/'veyra_qml_ui.exe')
 compileSource(ROOT/'tests/integration/FgPresentationTests.cpp','fg-presentation-tests',args(original[0]))
 command=next(cmd for cmd in previous['commands'] if any(a.startswith('/out:') and a.endswith('fg-presentation-tests.exe') for a in cmd))
 run(command,'link-native')
 assert sha(OUT/'veyra_qml_ui.exe')==guiSha
 for p,d in inputs.items():assert sha(Path(p))==d,p
 receipt={'commands':commands,'sources':sources,'fixtureSha256':sha(ROOT/'tests/integration/HdrCopyPresentationFixture.h'),'guiUnchanged':guiSha,'nativeSha256':sha(OUT/'fg-presentation-tests.exe'),'inputsUnchanged':inputs}
 (LOG/'native-receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
 print('Native fixture rebuilt; GUI and original inputs unchanged',flush=True);raise SystemExit(0)
for file in ('include/veyra/engine/PresentationSettings.h','include/veyra/engine/VideoPresenter.h','tests/integration/HdrCopyPresentationFixture.h'):
 sources[str(ROOT/file)]=sha(ROOT/file)
if '--combined' in sys.argv:
 for file in ('include/veyra/source/NativeCaptureSink.h','tests/unit/BmdAllocatorCases.h'):
  sources[str(ROOT/file)]=sha(ROOT/file)
deps=subprocess.check_output([tool,'-t','deps'],cwd=CACHE,text=True,encoding='utf8')
headers=('include/veyra/engine/PresentationSettings.h','include/veyra/engine/VideoPresenter.h')
dependent=[]
for block in deps.split('\n\n'):
 if any(h in block.replace('\\','/') for h in headers):
  name=block.splitlines()[0].split(': #deps',1)[0].replace('\\','/')
  if name.endswith('.obj') and not name.startswith(('CMakeFiles/veyra_fg_settings_tests.','CMakeFiles/veyra_vfg_export_probe.')):dependent.append(name)
assert len(dependent)>=10,dependent
overrides={}
for name in dependent:
 a=compiler(name);source=Path(a[-1]);relative=name.split('.dir/',1)[1][:-4]
 if not relative.startswith('veyra_qml_bridge_autogen/'):
  source=ROOT/relative
 else:protect(source)
 overrides[name]=compileSource(source,Path(relative).stem,a)
# catalog.json is an executable resource, so replacing the staged JSON alone
# would leave the new settings untranslated. Regenerate only our private qrc.
resource=next(p for p in gui[0] if p.endswith('/qrc_veyra_i18n.cpp.obj'))
lines=subprocess.check_output([tool,'-t','commands',resource],cwd=CACHE,text=True,encoding='utf8').splitlines()
rccLine=next(l for l in lines if 'rcc.exe' in l.lower() and 'veyra_i18n' in l)
rcc=next(a for part in args(rccLine) for a in args(part) if a.lower().endswith('rcc.exe'))
qrc=OUT/'i18n.qrc';qrc.write_text('<RCC><qresource prefix="/i18n"><file alias="catalog.json">'+(ROOT/'i18n/catalog.json').as_posix()+'</file></qresource></RCC>',encoding='utf8')
generated=OUT/'qrc_veyra_i18n.cpp';run([rcc,'--name','veyra_i18n','--no-zstd','-o',str(generated),str(qrc)],'generate-i18n')
sources[str(ROOT/'i18n/catalog.json')]=sha(ROOT/'i18n/catalog.json')
overrides[resource]=compileSource(generated,'qrc_veyra_i18n',compiler(resource))
combined='--combined' in sys.argv
if combined:
 for file in ('NativeCaptureSink.cpp','CaptureCardSource.cpp'):
  name='CMakeFiles/veyra_sources.dir/src/source/'+file+'.obj'
  overrides[name]=compileSource(ROOT/'src/source'/file,file[:-4],compiler(name))
fixture=compileSource(ROOT/'tests/integration/FgPresentationTests.cpp','fg-presentation-tests',args(original[0]))
core=[p for name,p in overrides.items() if name.startswith(('CMakeFiles/veyra_engine.','CMakeFiles/veyra_sources.'))]
assert sum(p.stem in ('VideoPresenter','EngineController','EngineControllerImage') for p in core)==3,core
assert any(p.stem=='PreviewGpuSession' for p in core),core
nativeLink=[]
for a in native:
 if a.endswith('FgPresentationTests.cpp.obj'):nativeLink+=list(map(str,core+[fixture]))
 elif a.startswith('/out:'):nativeLink.append('/out:'+str(OUT/'fg-presentation-tests.exe'))
 elif a.startswith('/implib:'):nativeLink.append('/implib:'+str(OUT/'fg-tests.lib'))
 elif a.startswith('/pdb:'):nativeLink.append('/pdb:'+str(OUT/'fg-tests.pdb'))
 else:nativeLink.append(a)
run(nativeLink,'link-native')
objects,var=gui;response=OUT/'gui-link.rsp'
objects=[CACHE/p for p in objects if p not in overrides]
response.write_text('\n'.join(subprocess.list2cmdline([str(p)]) for p in list(overrides.values())+objects)+'\n'+unescape(var.get('LINK_PATH',''))+'\n'+unescape(var['LINK_LIBRARIES']),encoding='utf8')
manifest=OUT/'manifest';manifest.mkdir(exist_ok=True)
link=r'C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Tools\MSVC\1444~1.352\bin\Hostx64\x64\link.exe'
cmake=r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
flags=[('/MANIFESTINPUT:'+str(CACHE/'veyra-qml.manifest')) if a.startswith('/MANIFESTINPUT:') else a for a in args(unescape(var.get('LINK_FLAGS','')))]
run([cmake,'-E','vs_link_exe','--msvc-ver=1944','--intdir='+str(manifest),r'--rc=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\rc.exe',r'--mt=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\mt.exe','--manifests','--',link,'/nologo','@'+str(response),'/out:'+str(OUT/'veyra_qml_ui.exe'),'/implib:'+str(OUT/'gui.lib'),'/pdb:'+str(OUT/'gui.pdb'),'/version:0.0']+flags,'link-gui')
extras=[]
if combined:
 captureObjects=[p for n,p in overrides.items() if n.startswith('CMakeFiles/veyra_sources.')]
 for target,name in (('veyra_capture_color_tests.exe','capture-contract'),('veyra_hdr_color_tests.exe','capture-gpu')):
  names,variables=rule(target,False);assert len(names)==1,(target,names)
  rel=names[0].split('.dir/',1)[1][:-4]
  testObject=compileSource(ROOT/rel,name,compiler(names[0]))
  # HdrColorTests constructs VideoPresenter directly, so it must receive the
  # same updated engine objects as the GUI, not the cached class implementation.
  selectedObjects=core if name=='capture-gpu' else captureObjects
  rsp=OUT/(name+'.rsp');rsp.write_text('\n'.join(subprocess.list2cmdline([str(p)]) for p in selectedObjects+[testObject])+'\n'+unescape(variables.get('LINK_PATH',''))+'\n'+unescape(variables['LINK_LIBRARIES']),encoding='utf8')
  directory=OUT/('manifest-'+name);directory.mkdir(exist_ok=True)
  testFlags=args(unescape(variables.get('LINK_FLAGS','')))
  run([cmake,'-E','vs_link_exe','--msvc-ver=1944','--intdir='+str(directory),r'--rc=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\rc.exe',r'--mt=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\mt.exe','--manifests','--',link,'/nologo','@'+str(rsp),'/out:'+str(OUT/(name+'.exe')),'/implib:'+str(OUT/(name+'.lib')),'/pdb:'+str(OUT/(name+'.pdb')),'/version:0.0']+testFlags,'link-'+name)
  extras.append(OUT/(name+'.exe'))
for p,d in inputs.items():assert sha(Path(p))==d,p
receipt={'root':str(ROOT),'combined':combined,'commands':commands,'sources':sources,'headerDependents':dependent,'overrides':{n:{'path':str(p),'sha256':sha(p)} for n,p in overrides.items()},'inputsUnchanged':inputs,'seconds':time.monotonic()-start,'executables':{p.name:sha(p) for p in [OUT/'veyra_qml_ui.exe',OUT/'fg-presentation-tests.exe']+extras}}
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
report=BASE/'reports'/TASK;report.mkdir(exist_ok=True,parents=True);(report/'build-current.json').write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
print('Build passed; cached inputs unchanged; receipt',LOG/'receipt.json',flush=True)

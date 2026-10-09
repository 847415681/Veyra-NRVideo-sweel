"""Relink diagnostic-only overrides; never write into the cached product build."""
from pathlib import Path
import ctypes, hashlib, json, os, re, shutil, subprocess

TASK='nv-vram-field-20261008'
BASE=Path('E:/项目/Veyra')
HERE=Path(__file__).parent
SOURCE=HERE.parents[2]
BUILD=BASE/'build/vfg-next-20261007'
OUT=BASE/'tests'/TASK/'compiled'
LOG=BASE/'logs'/TASK/'build'
TMP=BASE/'tmp'/TASK/'build'
for path in (OUT,LOG,TMP):path.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
shell=ctypes.WinDLL('shell32',use_last_error=True)
shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p]
def args(s):
 n=ctypes.c_int();p=shell.CommandLineToArgvW(s,ctypes.byref(n))
 result=[p[i] for i in range(n.value)];ctypes.windll.kernel32.LocalFree(p);return result
NINJA=r'C:/PROGRA~2/MICROS~3/2022/BUILDT~1/Common7/IDE/COMMON~1/MICROS~1/CMake/Ninja/ninja.exe'
commands=subprocess.check_output([NINJA,'-t','commands','veyra_fg_presentation_tests'],cwd=BUILD).decode('utf-8')
compile_line=next(x for x in commands.splitlines() if ' -c ' in x and x.endswith('FgPresentationTests.cpp'))
compile_args=args(compile_line)
(LOG/'compile-template.json').write_text(json.dumps(compile_args,ensure_ascii=False,indent=2),encoding='utf-8')
env=dict(os.environ);env.update(TEMP=str(TMP),TMP=str(TMP))
vcvars=r'C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat'
setup=subprocess.run(['cmd.exe','/u','/d','/c','call '+vcvars+' >nul && set'],capture_output=True,text=True,encoding='utf-16-le',check=True,env=env)
for line in setup.stdout.splitlines():
 if '=' in line:
  k,v=line.split('=',1)
  if k and not k.startswith('='):env[k]=v
env.update(TEMP=str(TMP),TMP=str(TMP))

inputs={};objects=[];build_commands=[];changes={}
for name in ('gfx/D3D12DeviceContext.cpp','pipeline/EnhanceGraph.cpp','pipeline/NrInstance.cpp'):
 product=SOURCE/'src'/name;text=product.read_text(encoding='utf-8');inputs[str(product)]=sha(product)
 def replace(old,new):
  global text
  assert text.count(old)==1,(name,old,text.count(old))
  text=text.replace(old,new)
 if name.startswith('gfx'):
  replace('    initialized_ = true;', '    initialized_ = true;\n    vramField::contextOpened(this,adapter_.Get(),fence_.Get(),adapterInfo_.luid);')
  replace('    // Slot work is drained', '    vramField::contextClosing(this);\n    // Slot work is drained')
 elif name.endswith('EnhanceGraph.cpp'):
  text=text.replace('makeTexture(', 'vramField::graphTexture(').replace('makeUploadBuffer(', 'vramField::graphUpload(')
  replace('bool EnhanceGraph::initialize(const EnhanceGraphDesc& desc)\n{',
          'bool EnhanceGraph::initialize(const EnhanceGraphDesc& desc)\n{\n    vramField::event("graph-initialize",this);')
  # The event marks entry, not successful construction or final GPU retirement.
  replace('void EnhanceGraph::shutdown()\n{', 'void EnhanceGraph::shutdown()\n{\n    vramField::event("graph-shutdown-entry",this);')
 else:
  text=text.replace('makeTexture(', 'vramField::nrTexture(')
  replace('    historyValid = false;\n    return true;', '    historyValid = false;\n    vramField::event("nr-layer-created",this);\n    return true;')
  replace('void NrInstance::close() {', 'void NrInstance::close() {\n    if(created())vramField::event("nr-layer-close-entry",this);')
 text='#include "VramField.h"\n'+text
 output=OUT/(Path(name).stem+'.diagnostic.cpp');output.write_text(text,encoding='utf-8')
 obj=OUT/(Path(name).stem+'.obj')
 command=[x for x in compile_args if not x.startswith(('/Fo','/Fd')) and x!='/showIncludes' and not x.endswith('FgPresentationTests.cpp')]
 command+=['/I'+str(HERE),'/Fo'+str(obj),'/Fd'+str(OUT/'diagnostic.pdb'),str(output)]
 with (LOG/(obj.stem+'-compile.log')).open('w',encoding='utf-8') as stream:
  r=subprocess.run(command,cwd=BUILD,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=300)
 print(obj.stem,'compile',r.returncode,flush=True)
 if r.returncode:raise SystemExit(r.returncode)
 build_commands.append(command);objects.append(obj);changes[str(output)]=sha(output)

ninja=(BUILD/'build.ninja').read_text(encoding='utf-8')
match=re.search(r'^build veyra_qml_ui.exe: .*?(?=\n\n)',ninja,re.M|re.S);assert match
lines=match.group().splitlines()
def unescape(s):return s.replace('$ ',' ').replace('$:',':').replace('$$','$')
original_objects=[BUILD/unescape(x) for x in lines[0].split('Release ',1)[1].split(' | ',1)[0].split(' ')]
v=dict(re.match(r'  ([A-Z_]+) = (.*)',x).groups() for x in lines[1:] if re.match(r'  ([A-Z_]+) = (.*)',x))
libraries=args(unescape(v['LINK_LIBRARIES']))
for item in list(map(str,original_objects))+libraries+['veyra_qml_ui.exe','veyra-qml.manifest']:
 p=Path(item);p=p if p.is_absolute() else BUILD/p
 if p.is_file():inputs[str(p)]=sha(p)
rsp=OUT/'diagnostic-link.rsp'
rsp.write_text('\n'.join(subprocess.list2cmdline([str(p)]) for p in objects+original_objects)+'\n'+unescape(v.get('LINK_PATH',''))+'\n'+unescape(v['LINK_LIBRARIES']),encoding='utf-8')
exe=OUT/'veyra-vram-diagnostic.exe'
link=r'C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Tools\MSVC\1444~1.352\bin\Hostx64\x64\link.exe'
cmake=r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
flags=[('/MANIFESTINPUT:'+str(BUILD/'veyra-qml.manifest')) if x.startswith('/MANIFESTINPUT:') else x for x in args(unescape(v['LINK_FLAGS']))]
(OUT/'manifest').mkdir(exist_ok=True)
command=[cmake,'-E','vs_link_exe','--msvc-ver=1944','--intdir='+str(OUT/'manifest'),
 r'--rc=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\rc.exe',r'--mt=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\mt.exe','--manifests','--',link,'/nologo','@'+str(rsp),
 '/out:'+str(exe),'/implib:'+str(OUT/'diagnostic.lib'),'/pdb:'+str(OUT/'diagnostic-link.pdb'),'/version:0.0']+flags
with (LOG/'link.log').open('w',encoding='utf-8') as stream:
 r=subprocess.run(command,cwd=BUILD,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=300)
print('GUI link',r.returncode,flush=True);build_commands.append(command)
for name,digest in inputs.items():assert sha(Path(name))==digest,name
receipt={'exit':r.returncode,'baseProduct':'71723e0249a17764696796fd204a6eb69d6da276','inputsUnchanged':inputs,'generatedSources':changes,'commands':build_commands}
if exe.exists():receipt.update(exeSha256=sha(exe),exeBytes=exe.stat().st_size)
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf-8')
raise SystemExit(r.returncode)

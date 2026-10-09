"""Scoped settings/SR/UI build; immutable accepted cache, current dependency closure."""
from pathlib import Path
import ctypes, hashlib, json, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='settings-sr-ui-20261010'
CACHE=BASE/'build/vfg-next-20261007';OUT=BASE/'build'/TASK
SDK=BASE/'deps/decklink-sdk-obs31-20261009'
LOG=BASE/'logs'/TASK/('build-'+time.strftime('%Y%m%d-%H%M%S'));TMP=BASE/'tmp'/TASK/'build'
for p in (OUT,LOG,TMP):p.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
shell=ctypes.WinDLL('shell32');shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)];shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p);ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p]
def args(s):
    n=ctypes.c_int();p=shell.CommandLineToArgvW(s,ctypes.byref(n));a=[p[i] for i in range(n.value)];ctypes.windll.kernel32.LocalFree(p);return a
def unescape(s):return s.replace('$ ',' ').replace('$:',':').replace('$$','$')
tool=re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.*)$',(CACHE/'CMakeCache.txt').read_text(encoding='utf8'),re.M).group(1)
ninja=(CACHE/'build.ninja').read_text(encoding='utf8');inputs={};sources={};commands=[];start=time.monotonic()
def protect(p):
    p=Path(p);p=p if p.is_absolute() else CACHE/p;assert p.is_file(),p
    if str(p) not in inputs:inputs[str(p)]=sha(p)
    return p
def rule(name):
    lines=re.search(r'^build '+re.escape(name)+r': .*?(?=\n\n)',ninja,re.M|re.S).group().splitlines()
    var=dict(re.match(r'  ([A-Z_]+) = (.*)',l).groups() for l in lines[1:] if re.match(r'  ([A-Z_]+) = (.*)',l))
    objects=[unescape(n).replace('\\','/') for n in lines[0].split('Release ',1)[1].split(' | ',1)[0].split(' || ',1)[0].split(' ')]
    for value in args(unescape(var.get('LINK_LIBRARIES',''))):
        if value.lower().endswith('.lib') and (Path(value).is_file() or (CACHE/value).is_file()):protect(value)
    return objects,var
def compiler(name):return args(subprocess.check_output([tool,'-t','commands',name],cwd=CACHE,text=True,encoding='utf8').splitlines()[-1])
env=dict(os.environ);env.update(TEMP=str(TMP),TMP=str(TMP),VSLANG='1033')
setup=subprocess.run(['cmd.exe','/u','/d','/c',r'call C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && set'],capture_output=True,text=True,encoding='utf-16-le',env=env,check=True)
for line in setup.stdout.splitlines():
    if '=' in line:
        k,v=line.split('=',1)
        if k and not k.startswith('='):env[k]=v
env.update(TEMP=str(TMP),TMP=str(TMP))
def run(command,name,cwd=TMP):
    commands.append(command);remaining=890-(time.monotonic()-start);assert remaining>0
    with (LOG/(name+'.log')).open('wb') as stream:r=subprocess.run(command,cwd=cwd,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=min(300,remaining))
    print(name,'exit',r.returncode,flush=True)
    if r.returncode:print((LOG/(name+'.log')).read_text(encoding='utf8',errors='replace')[-6000:]);raise RuntimeError(name)

if sys.argv[1:]==['cmake-check']:
    cache=dict(re.findall(r'^([^#/:=]+):[^=]+=(.*)$',(CACHE/'CMakeCache.txt').read_text(encoding='utf8'),re.M))
    cmake=r'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    names=('VEYRA_DLSS_SDK_ROOT','VEYRA_NVOF_SDK_ROOT','VEYRA_NVENC_HEADERS_ROOT','VEYRA_XESS_ROOT',
           'VEYRA_FIDELITYFX_ROOT','VEYRA_FIDELITYFX2_ROOT','VEYRA_MAGEWELL_SDK_DIR','VEYRA_LIBASS_ROOT','CMAKE_PREFIX_PATH')
    for native in (True,False):
        name='cmake-sdk' if native else 'cmake-no-sdk';directory=OUT/name
        flags=['-D'+key+'='+cache[key] for key in names]
        # The accepted vcpkg prefix supplies MSVC import libraries. The patched
        # runtime prefix used in tests contains DLLs/DEFs, not MSVC libraries.
        flags+=['-DVEYRA_FFMPEG_ROOT='+cache['VEYRA_FFMPEG_ROOT'],
                '-DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON','-DVEYRA_ENABLE_D3D12_DEBUG=OFF','-DVEYRA_BUILD_QML_UI=ON',
                '-DVEYRA_ENABLE_REMOTEPLAY=OFF','-DVEYRA_ENABLE_MOONLIGHT=OFF','-DVEYRA_ENABLE_XBOX=OFF',
                '-DVEYRA_DECKLINK_SDK_DIR='+(str(SDK) if native else ''),'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_MAKE_PROGRAM='+tool]
        run([cmake,'-S',str(ROOT),'-B',str(directory),'-G','Ninja',*flags],name+'-configure')
        run([cmake,'--build',str(directory),'--target','CMakeFiles/veyra_sources.dir/src/source/DeckLinkDriver.cpp.obj','-j','2'],name+'-compile')
        if native:
            run([cmake,'--build',str(directory),'--target',
                'CMakeFiles/veyra_settings_transaction_tests.dir/tests/integration/SettingsTransactionTests.cpp.obj',
                'CMakeFiles/veyra_sr_quality_tests.dir/tests/integration/SrQualityTests.cpp.obj','-j','2'],name+'-test-objects')
    (LOG/'receipt.json').write_text(json.dumps({'commands':commands,'passed':True,'seconds':time.monotonic()-start},ensure_ascii=False,indent=2),encoding='utf8')
    (BASE/'reports'/TASK/'cmake-current.json').write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
    print('CMake native/no-SDK configurations, driver objects and current test objects passed;',LOG,flush=True)
    raise SystemExit(0)
headerKey=hashlib.sha256(''.join(str(p)+sha(p) for folder in ('include','tests') for p in sorted((ROOT/folder).rglob('*.h'))).encode()).hexdigest()
def compile(source,name,a,extra=(),dependencyKey=''):
    sources[str(source)]=sha(source);target=OUT/(name+'.obj')
    command=a[:1]+['/utf-8','/I'+str(OUT),'/I'+str(ROOT/'include'),'/I'+str(ROOT/'src')]+[v.replace(str(CACHE).replace('\\','/'),str(OUT).replace('\\','/')) if v.startswith(('/DVEYRA_SHADER_DIR','-DVEYRA_SHADER_DIR')) else v for v in a[1:-1] if not v.startswith(('/Fo','/Fd')) and v!='/showIncludes']
    command+=list(extra)+['/Fo'+str(target),'/Fd'+str(OUT/(name+'.pdb')),str(source)]
    key=hashlib.sha256((json.dumps(command)+sources[str(source)]+headerKey+dependencyKey).encode()).hexdigest();stamp=target.with_suffix('.build.json')
    if target.is_file() and stamp.is_file():
        old=json.loads(stamp.read_text());
        if old['key']==key and old['object']==sha(target):print('reuse',name,flush=True);return target
    run(command,'compile-'+name);stamp.write_text(json.dumps({'key':key,'object':sha(target)}),encoding='utf8');return target
previous=json.loads((BASE/'logs/decklink-native-20261009/build-20261009-205406/receipt.json').read_text(encoding='utf8'))
overrides={name:protect(row['path']) for name,row in previous['overrides'].items()}
for name,row in previous['overrides'].items():assert sha(row['path'])==row['sha256'],name
targets={n:rule(n+'.exe') for n in ('veyra_qml_ui','veyra_capture_tests','veyra_capture_color_tests','veyra_capture_format_selection_tests','veyra_hdr_color_tests','veyra_capture_audio_tests','veyra_qml_quick_tests','veyra_present_sink_lifecycle_tests','veyra_live_presentation_tests','veyra_nr_video_quality_probe')}
objects=set(p for obj,var in targets.values() for p in obj)
def libraries(var):return {Path(v).stem for v in args(unescape(var.get('LINK_LIBRARIES',''))) if v.endswith('.lib') and Path(v).stem.startswith('veyra_')}
groups=set().union(*(libraries(var) for objs,var in targets.values()))
for group in groups:
    if re.search(r'^build '+re.escape(group+'.lib')+r': (?:CXX|C)_STATIC_',ninja,re.M):objects.update(rule(group+'.lib')[0])
baseline=json.loads((BASE/'archives'/TASK/'start.json').read_text(encoding='utf8'))['base']
changed_headers=[p.replace('\\','/') for p in subprocess.check_output(['git','diff','--name-only',baseline],cwd=ROOT,text=True,encoding='utf8').splitlines() if p.startswith('include/')]
dependencies={};current=None
for line in subprocess.check_output([tool,'-t','deps'],cwd=CACHE,text=True,encoding='utf8').splitlines():
    if line and not line[0].isspace():current=line.split(': #deps ',1)[0].replace('\\','/');dependencies[current]=[]
    elif current and line.strip():dependencies[current].append(line.strip().replace('\\','/'))
rebuild=[]
for obj in objects:
    relative=obj.split('.dir/',1)[-1][:-4]
    if not relative.startswith(('src/','apps/','tests/')) or not (ROOT/relative).is_file():continue
    original=next((p for p in previous['sources'] if p.replace('\\','/').endswith('/'+relative)),None)
    source_changed=original is not None and Path(original).read_bytes().replace(b'\r\n',b'\n')!=(ROOT/relative).read_bytes().replace(b'\r\n',b'\n')
    directly=relative in ('src/source/CaptureCardSource.cpp','src/engine/EngineController.cpp','src/pipeline/EnhanceGraph.cpp','src/ui/QmlPlayerBridge.cpp','tests/integration/HdrColorTests.cpp','src/engine/VideoPresenter.cpp','src/gfx/PresentSink.cpp')
    dep_changed=any(any(d.endswith('/'+h) for d in dependencies.get(obj,())) for h in changed_headers)
    if source_changed or directly or dep_changed or not (CACHE/obj).is_file():rebuild.append((obj,ROOT/relative,compiler(obj)))
moc=protect(Path('C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64/bin/moc.exe'))
run([str(moc),str(ROOT/'tests/qml/QuickSmokeTests.cpp'),'-o',str(OUT/'QuickSmokeTests.moc')],'generate-quick-test-moc')

def job(row):
    obj,source,a=row;return obj,compile(source,obj.replace('/','-').replace('.','-'),a)
with ThreadPoolExecutor(max_workers=4) as pool:
    for obj,p in pool.map(job,rebuild):overrides[obj]=p
quickAutogen='CMakeFiles/veyra_qml_quick_tests.dir/veyra_qml_quick_tests_autogen/mocs_compilation.cpp.obj'
emptyMoc=OUT/'quick-mocs.cpp';emptyMoc.write_text('// Setup moc is included explicitly by QuickSmokeTests.cpp.\n',encoding='utf8')
overrides[quickAutogen]=compile(emptyMoc,'quick-mocs',compiler(quickAutogen))
for obj in objects:
    if obj not in overrides:protect(obj)
# Unchanged objects added by PR31, not present in the older cached CMake graph.
playlist=[protect(BASE/'build/pr31-playlist-20261009'/n) for n in ('PlaylistModel.obj','playlist-moc.obj')]
for relative in ('include/veyra/ui/QmlPlayerBridge.h','include/veyra/ui/PlaylistModel.h','src/ui/PlaylistModel.cpp'):
    original=next(p for p in previous['sources'] if p.replace('\\','/').endswith('/'+relative));assert sha(original)==previous['sources'][original]
    assert Path(original).read_bytes().replace(b'\r\n',b'\n')==(ROOT/relative).read_bytes().replace(b'\r\n',b'\n');sources[str(ROOT/relative)]=sha(ROOT/relative)
sourceCompiler=compiler('CMakeFiles/veyra_sources.dir/src/source/CaptureCardSource.cpp.obj')
generatedSdk=OUT/'decklink-generated';generatedSdk.mkdir(exist_ok=True)
for file in SDK.rglob('*'):
    if file.is_file():protect(file)
midl=protect(Path(r'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/midl.exe'))
run([str(midl),'/nologo','/env','x64','/I',str(SDK),'/out',str(generatedSdk),'/h','DeckLinkAPI_h.h',
     '/iid','DeckLinkAPI_i.c','/tlb','DeckLinkAPI.tlb','/client','none','/server','none',str(SDK/'DeckLinkAPI.idl')],'generate-decklink-abi')
nativeCore=compile(ROOT/'src/source/DeckLinkCapture.cpp','DeckLinkCapture',sourceCompiler)
nativeDriver=compile(ROOT/'src/source/DeckLinkDriver.cpp','DeckLinkDriver',sourceCompiler,['/DVEYRA_HAS_DECKLINK=1','/I'+str(generatedSdk)],sha(generatedSdk/'DeckLinkAPI_h.h'))
stubDriver=compile(ROOT/'src/source/DeckLinkDriver.cpp','DeckLinkDriver-stub',sourceCompiler,['/DVEYRA_HAS_DECKLINK=0'])
native=[nativeCore,nativeDriver]
uiOnly=sys.argv[1:]==['ui-only']
if uiOnly:
    for name in ('veyra_capture_tests','veyra_capture_color_tests','veyra_capture_format_selection_tests',
                 'veyra_hdr_color_tests','veyra_capture_audio_tests','decklink-tests','decklink-stub-tests'):
        protect(OUT/(name+'.exe'))
for file in SDK.rglob('*'):
    if file.is_file():protect(file)

shaderOut=OUT/'shaders';shaderOut.mkdir(exist_ok=True)
for source in (CACHE/'shaders').rglob('*'):
    if not source.is_file() or source.name=='PackedCaptureToLinear.dxil':continue
    protect(source);target=shaderOut/source.relative_to(CACHE/'shaders');target.parent.mkdir(parents=True,exist_ok=True)
    if not target.exists():os.link(source,target)
dxc=protect(Path(r'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe'))
run([str(dxc),'-T','cs_6_0','-E','main','-O3','-Qstrip_debug','-Qstrip_reflect','-Fo',str(shaderOut/'PackedCaptureToLinear.dxil'),str(ROOT/'shaders/PackedCaptureToLinear.hlsl')],'shader-packed')
resource='CMakeFiles/veyra_qml_ui.dir/.qt/rcc/qrc_veyra_i18n.cpp.obj'
rcc=protect(Path('C:/veyra-deps/qt-veyra/6.8.3/msvc2022_64/bin/rcc.exe'))
qrc=OUT/'i18n.qrc';qrc.write_text('<RCC><qresource prefix="/i18n"><file alias="catalog.json">'+(ROOT/'i18n/catalog.json').as_posix()+'</file></qresource></RCC>',encoding='utf8')
generated=OUT/'qrc_veyra_i18n.cpp';run([str(rcc),'--name','veyra_i18n','--no-zstd','-o',str(generated),str(qrc)],'generate-i18n')
overrides[resource]=compile(generated,'qrc_veyra_i18n',compiler(resource));sources[str(ROOT/'i18n/catalog.json')]=sha(ROOT/'i18n/catalog.json')

cmake=r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
link=r'C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Tools\MSVC\1444~1.352\bin\Hostx64\x64\link.exe';executables=[]
def linkTarget(name,objs,var):
    rsp=OUT/(name+'.rsp');rsp.write_text('\n'.join(subprocess.list2cmdline([str(p)]) for p in objs)+'\n'+unescape(var.get('LINK_PATH',''))+'\n'+unescape(var['LINK_LIBRARIES']),encoding='utf8')
    directory=OUT/('manifest-'+name);directory.mkdir(exist_ok=True)
    flags=[('/MANIFESTINPUT:'+str(CACHE/'veyra-qml.manifest')) if a.startswith('/MANIFESTINPUT:') else a for a in args(unescape(var.get('LINK_FLAGS','')))]
    command=[cmake,'-E','vs_link_exe','--msvc-ver=1944','--intdir='+str(directory),r'--rc=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\rc.exe',r'--mt=C:\PROGRA~2\WI3CF2~1\10\bin\100261~1.0\x64\mt.exe','--manifests','--',link,'/nologo','@'+str(rsp),'/out:'+str(OUT/(name+'.exe')),'/implib:'+str(OUT/(name+'.lib')),'/pdb:'+str(OUT/(name+'.pdb')),'/version:0.0']+flags
    run(command,'link-'+name,CACHE);executables.append(OUT/(name+'.exe'))
for name,(objs,var) in targets.items():
    if uiOnly and name!='veyra_qml_ui':continue
    selected=[overrides.get(obj,CACHE/obj) for obj in objs]
    linked=libraries(var)
    selected+=[p for obj,p in overrides.items() if obj not in objs and any(obj.startswith('CMakeFiles/'+group+'.dir/') for group in linked) and
               (name!='veyra_capture_audio_tests' or obj.endswith('/src/sink/CaptureAudioSession.cpp.obj'))]
    if name=='veyra_qml_ui':selected+=playlist
    # Static-library closure flattened by CMake into the immutable GUI graph.
    if 'veyra_sources' in linked:selected+=native
    linkTarget(name,selected,var)
if not uiOnly:
    objects,var=targets['veyra_live_presentation_tests']
    linked=libraries(var)
    modules=[p for obj,p in overrides.items() if any(obj.startswith('CMakeFiles/'+group+'.dir/') for group in linked)]
    for name,source in (('veyra_settings_transaction_tests','tests/integration/SettingsTransactionTests.cpp'),('veyra_sr_quality_tests','tests/integration/SrQualityTests.cpp')):
        test=compile(ROOT/source,name,compiler(objects[0]))
        linkTarget(name,[test]+modules+native,var)
if not uiOnly:
    test=compile(ROOT/'tests/unit/DeckLinkCaptureTests.cpp','DeckLinkCaptureTests',sourceCompiler)
    var=targets['veyra_capture_tests'][1]
    baseObj=protect(CACHE/'CMakeFiles/veyra_base.dir/src/base/Log.cpp.obj')
    linkTarget('decklink-tests',[test,nativeCore,nativeDriver,baseObj],var)
    linkTarget('decklink-stub-tests',[test,nativeCore,stubDriver,baseObj],var)
for p,d in inputs.items():assert sha(p)==d,p
for folder in ('include','src','shaders','qml','tests'):
    for file in (ROOT/folder).rglob('*'):
        if file.is_file() and file.suffix in ('.h','.hlsl','.hlsli','.qml','.cpp'):sources[str(file)]=sha(file)
receipt={'commands':commands,'sources':sources,'overrides':{n:{'path':str(p),'sha256':sha(p)} for n,p in overrides.items()},'inputsUnchanged':inputs,'seconds':time.monotonic()-start,'executables':{p.name:sha(p) for p in executables},'recompiledDependencyClosure':len(rebuild)}
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
(BASE/'reports'/TASK/'build-current.json').write_text(json.dumps({'receipt':str(LOG/'receipt.json')},indent=2),encoding='utf8')
print('Built current product and native/legacy tests; immutable inputs unchanged;',LOG,flush=True)

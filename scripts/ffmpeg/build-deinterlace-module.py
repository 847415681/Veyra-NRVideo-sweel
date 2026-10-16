from pathlib import Path
import hashlib,json,os,subprocess,time,urllib.request,zipfile

import argparse
parser=argparse.ArgumentParser();parser.add_argument('--task',default='issues41-45-20261016');parser.add_argument('--compile-only',action='store_true');args=parser.parse_args()
base=Path('E:/项目/Veyra');task=args.task
import re
assert re.fullmatch(r'[a-z0-9][a-z0-9.-]{0,79}',task)
root=Path(__file__).resolve().parents[2]
work=base/'build'/task/'ffmpeg-filter';logs=base/'logs'/task/'ffmpeg-filter'
tmp=base/'tmp'/task/'filter-build';prefix=base/'deps'/('ffmpeg-filter-'+task)
for path in [work,logs,tmp,prefix]:path.mkdir(parents=True,exist_ok=True)

def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

archive=base/'downloads'/'FFmpeg-n9.0.1-issues41-45-20261016.zip'
if not archive.exists():
    with urllib.request.urlopen('https://codeload.github.com/FFmpeg/FFmpeg/zip/refs/tags/n9.0.1',timeout=30) as response:
        archive.write_bytes(response.read())
src=work/'FFmpeg-n9.0.1'
if not src.exists():
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            assert (work/item.filename).resolve().is_relative_to(work.resolve())
        source.extractall(work)
tag=json.loads(urllib.request.urlopen('https://api.github.com/repos/FFmpeg/FFmpeg/git/ref/tags/n9.0.1',timeout=20).read())
obj=tag['object']
if obj['type']=='tag':obj=json.loads(urllib.request.urlopen(obj['url'],timeout=20).read())['object']
assert obj['type']=='commit' and obj['sha']=='bf1b838f2ab88b4f8fd83443325c782ea0e0f7fa'
assert sha(archive)=='d16837ddbb0963753aa2739971a09b07fe14fe855a6e84f253f851127f99c746'
patch=root/'scripts/ffmpeg/deinterlace-d3d12-async.patch'
check=subprocess.run(['git','apply','--check',str(patch)],cwd=src,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
if check.returncode==0:subprocess.run(['git','apply',str(patch)],cwd=src,check=True)
else:subprocess.run(['git','apply','--reverse','--check',str(patch)],cwd=src,check=True)
print('source',obj['sha'],'archive',sha(archive),flush=True)
configure_source=src/'configure'
configuration=configure_source.read_text(encoding='utf8')
# This host has the Chinese-only MSVC banner. Detection must not require
# 'Microsoft' at column 0; retain the actual compiler, flags and ABI.
configuration=configuration.replace('grep -q ^Microsoft','grep -q Microsoft').replace('grep ^Microsoft','grep Microsoft')
if configuration!=configure_source.read_text(encoding='utf8'):configure_source.write_text(configuration,encoding='utf8')
env={key:value for key,value in os.environ.items() if not key.upper().startswith(('VEYRA_','QT_','QML_','QSG_'))}
env.update(TEMP=str(tmp),TMP=str(tmp),VSLANG='1033',PYTHONDONTWRITEBYTECODE='1')
setup=subprocess.run(['cmd.exe','/u','/d','/c',r'call C:\PROGRA~2\MICROS~3\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && set'],env=env,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,check=True)
for line in setup.stdout.decode('utf-16-le').splitlines():
    if '=' in line:
        key,value=line.split('=',1)
        if key and not key.startswith('='):env[key]=value
msys=Path('C:/veyra-deps/vcpkg/downloads/tools/msys2/0fcfdbb60b602f3c')
env={key.upper():value for key,value in env.items()}
env['VSLANG']='1033'
bash=msys/'usr/bin/bash.exe';make=msys/'usr/bin/make.exe'
assert bash.exists() and make.exists()
env['PATH']=str(Path(env['VCTOOLSINSTALLDIR'])/'bin/Hostx64/x64')+';'+str(msys/'usr/bin')+';'+env['PATH']
env.update(MSYS2_PATH_TYPE='inherit',MSYS_NO_PATHCONV='1',TEMP=str(tmp),TMP=str(tmp))
flags=['--toolchain=msvc','--arch=x86_64','--target-os=win32','--disable-doc','--disable-programs',
       '--disable-everything','--disable-autodetect','--disable-avcodec','--disable-avformat','--disable-avdevice',
       '--disable-swscale','--disable-swresample','--enable-avfilter','--enable-filter=bwdif,deinterlace_d3d12',
       '--enable-d3d12va','--enable-d3d11va','--enable-shared','--disable-static','--disable-x86asm',
       '--enable-w32threads','--cc=cl.exe','--cxx=cl.exe','--ld=link.exe',
       '--ar='+str(msys/'usr/share/automake-1.18/ar-lib')+' lib.exe','--ranlib=:',
       '--extra-cflags=-MD','--extra-cxxflags=-MD','--prefix='+str(prefix)]
commands=[('configure',[str(bash),'./configure',*flags]),('compile',[str(make),'-j8','libavfilter/avfilter-12.dll'])]
if args.compile_only:commands=commands[1:]
start=time.monotonic()
for name,command in commands:
    if (logs/(name+'.log')).exists():
        import shutil
        shutil.copy2(logs/(name+'.log'),logs/(name+'-previous-'+str(time.time_ns())+'.log'))
    if name=='configure' and (src/'ffbuild/config.log').exists():
        import shutil
        shutil.copy2(src/'ffbuild/config.log',logs/('config-previous-'+str(time.time_ns())+'.log'))
    with (logs/(name+'.log')).open('wb') as stream:
        result=subprocess.run(command,cwd=src,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=max(1,880-(time.monotonic()-start)))
    print(name,result.returncode,str(logs/(name+'.log')),flush=True)
    if result.returncode:
        print((logs/(name+'.log')).read_text(encoding='utf8',errors='replace')[-6500:]);raise SystemExit(result.returncode)
    if name=='configure':
        import re
        generated=src/'config.h';contents=generated.read_text(encoding='utf8')
        contents=re.sub(r'^#define CC_IDENT .*$', '#define CC_IDENT "Microsoft MSVC 19.44 (localized host)"',contents,flags=re.M)
        generated.write_text(contents,encoding='utf8')
dll=src/'libavfilter/avfilter-12.dll';assert dll.exists()
# Add one LGPL filter DLL and its public headers to a new dependency prefix.
# Existing approved codec/demux/util DLLs are reused byte-for-byte, never replaced.
old=base/'deps/ffmpeg-subtitle-formats-20261009/installed'
for path in old.rglob('*'):
    if path.is_file():
        dest=prefix/path.relative_to(old);dest.parent.mkdir(parents=True,exist_ok=True)
        if not dest.exists():os.link(path,dest)
(prefix/'bin').mkdir(exist_ok=True)
import shutil
shutil.copy2(dll,prefix/'bin/avfilter-12.dll')
headers=prefix/'include/libavfilter';headers.mkdir(parents=True,exist_ok=True)
for path in (src/'libavfilter').glob('*.h'):shutil.copy2(path,headers/path.name)
receipt=dict(passed=True,tag='n9.0.1',commit=obj['sha'],archive=str(archive),archiveSha256=sha(archive),
             source=str(src),patch=str(patch),patchSha256=sha(patch),commands=commands,filterDll=str(prefix/'bin/avfilter-12.dll'),filterSha256=sha(dll),
             existingRuntimeFilesPreserved=all(sha(path)==sha(prefix/path.relative_to(old)) for path in old.rglob('*') if path.is_file()),
             seconds=time.monotonic()-start,license='LGPL-2.1-or-later')
(logs/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
assert receipt['existingRuntimeFilesPreserved']
# Preserve complete upstream source plus the actual local patch/build recipe.
archive_out=base/'archives'/task/'FFmpeg-9.0.1-deinterlace-module-source.zip'
archive_out.parent.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(archive) as upstream,zipfile.ZipFile(archive_out,'w',zipfile.ZIP_DEFLATED,6) as result:
    for entry in upstream.infolist():
        if entry.is_dir():continue
        rel=Path(*Path(entry.filename).parts[1:])
        content=(src/rel).read_bytes() if rel.as_posix() in ('configure','libavfilter/vf_deinterlace_d3d12.c') else upstream.read(entry)
        result.writestr(entry.filename,content)
    result.writestr('veyra-build/build-deinterlace-module.py',Path(__file__).read_bytes())
    result.writestr('veyra-build/deinterlace-d3d12-async.patch',patch.read_bytes())
    result.writestr('veyra-build/configuration.json',json.dumps(dict(upstreamCommit=obj['sha'],configuration=flags,receipt=receipt),ensure_ascii=False,indent=2))
with zipfile.ZipFile(archive_out) as result:assert result.testzip() is None
share=prefix/'share/ffmpeg';share.mkdir(parents=True,exist_ok=True)
for name in ('COPYING.LGPLv2.1','LICENSE.md'):shutil.copy2(src/name,share/('filter-'+name))
metadata=dict(schema=1,sourceTag='n9.0.1',sourceCommit=obj['sha'],license='LGPL-2.1-or-later',configuration=flags,
              patchSha256=sha(patch),correspondingSource=dict(archive=str(archive_out),sha256=sha(archive_out)),
              files=[dict(name='avfilter-12.dll',sha256=sha(dll),bytes=dll.stat().st_size)],existingRuntimeFilesPreserved=True)
(share/'veyra-filter-build.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf8')
receipt['filterManifest']=str(share/'veyra-filter-build.json');receipt['correspondingSource']=metadata['correspondingSource']
(logs/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
print('PASS',json.dumps(receipt,ensure_ascii=False),flush=True)

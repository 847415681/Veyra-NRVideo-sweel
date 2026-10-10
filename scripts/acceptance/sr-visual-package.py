"""Local fixes candidate: immutable donor whitelist, current code, sealed audit."""
from pathlib import Path
import hashlib, importlib.util, json, os, re, shutil, subprocess, sys, time, zipfile
import psutil

ROOT=Path(__file__).resolve().parents[2]
BASE=Path('E:/项目/Veyra');TASK='sr-visual-quality-20261010';LABEL='2.0.6-fixes-20261010'
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
BUILD=BASE/'build'/TASK;OUT=BASE/'test-packages/fixes-20261010'
STAGE=OUT/('Veyra-'+LABEL+'-NVIDIA-win64-portable')
FFMPEG=BASE/'deps/ffmpeg-subtitle-formats-20261009/installed'
FFSOURCE=BASE/'test-packages/subtitle-formats-20261009/FFmpeg-9.0.1-slices256-dav1d-zlib-source.zip'
FFSHA='49db22049f072bd5448faa664aa736074dccfe6e72b4a933f673d8ae954b0bb6'
SOURCE=OUT/('Veyra-'+LABEL+'-source.zip')
REPORT=BASE/'reports'/TASK
spec=importlib.util.spec_from_file_location('qml_package',ROOT/'scripts/package-qml-release.py')
pkg=importlib.util.module_from_spec(spec);spec.loader.exec_module(pkg)
donor=json.loads((DONOR/'package-manifest.json').read_text(encoding='utf8'))

def sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def verify_donor():
    for row in donor['files']:
        f=DONOR/row['path'];assert f.stat().st_size==row['size'] and sha(f)==row['sha256'],row['path']

def atomic(source,target):
    target.parent.mkdir(parents=True,exist_ok=True)
    swap=target.with_name(target.name+'.swap');shutil.copyfile(source,swap);os.replace(swap,target)

def write(target,data):
    target.parent.mkdir(parents=True,exist_ok=True)
    swap=target.with_name(target.name+'.swap');swap.write_bytes(data);os.replace(swap,target)

def document(target,data):write(target,(json.dumps(data,ensure_ascii=False,indent=2)+'\n').encode('utf8'))

def payload():
    records=[]
    for f in sorted(STAGE.rglob('*')):
        if not f.is_file() or f.name=='package-manifest.json':continue
        name=f.relative_to(STAGE).as_posix();pkg.validate_payload(name)
        assert f.name not in ('SrMotion.dxil','SrStaticDetect.dxil'),name
        records.append(dict(path=name,size=f.stat().st_size,sha256=sha(f)))
    assert not pkg.REQUIRED-{r['path'] for r in records}
    return records

def runtime_audit():
    runtime=json.loads((STAGE/'release-runtime-manifest.json').read_text(encoding='utf8'))
    assert runtime['enforcedAtRuntime'] is False
    for row in runtime['files']:
        f=STAGE/row['path']
        assert f.stat().st_size==row['size'] and sha(f)==row['sha256'].lower(),row['path']
    assert (STAGE/'release-runtime-manifest.json').read_bytes()==(STAGE/'runtime/experimental/release-runtime-manifest.json').read_bytes()
    assert '--enable-zlib' in json.loads((STAGE/'licenses/FFMPEG-VEYRA-BUILD.json').read_text(encoding='utf8'))['configuration']
    assert sha(STAGE/'veyra_qml_ui.exe')==sha(BUILD/'veyra_qml_ui.exe')
    return len(runtime['files'])

def refresh():
    atomic(BUILD/'veyra_qml_ui.exe',STAGE/'veyra_qml_ui.exe')
    for f in (ROOT/'qml').rglob('*'):
        if f.is_file() and (f.suffix in ('.qml','.js') or f.name=='qmldir'):
            atomic(f,STAGE/'qml'/f.relative_to(ROOT/'qml'))
    for f in (BUILD/'shaders').glob('*.dxil'):atomic(f,STAGE/'shaders'/f.name)
    for name in ('README.md','README.en.md','LICENSE','THIRD_PARTY_NOTICES.md'):
        if (ROOT/name).is_file():atomic(ROOT/name,STAGE/name)
    docs={r['path'] for r in donor['files'] if r['path'].startswith('docs/')}
    docs|={'docs/'+name for name in ('SR_VISUAL_QUALITY_REPORT_2026-10-10.md','SR_VISUAL_QUALITY_PLAN_2026-10-10.md',
        'SETTINGS_SR_UI_REPORT_2026-10-10.md','INACTIVE_EFFECTS_REPORT_2026-10-10.md','DECKLINK_NATIVE_REPORT_2026-10-09.md',
        'SUBTITLE_FORMATS_REPORT_2026-10-09.md','PR31_PLAYLIST_REPORT_2026-10-09.md','HDR_FG_COMPAT_TOGGLE_REPORT_2026-10-09.md')}
    for name in docs:
        if (ROOT/name).is_file():atomic(ROOT/name,STAGE/name)
    ff=pkg.audit_ffmpeg(FFMPEG)
    for row in ff['files']:atomic(FFMPEG/'bin'/row['name'],STAGE/row['name'])
    atomic(FFMPEG/'share/ffmpeg/veyra-local-build.json',STAGE/'licenses/FFMPEG-VEYRA-BUILD.json')
    atomic(FFMPEG/'share/zlib/copyright',STAGE/'licenses/ZLIB-COPYRIGHT.txt')
    atomic(FFMPEG/'share/zlib/vcpkg.spdx.json',STAGE/'licenses/ZLIB-SPDX.json')
    runtime=json.loads((DONOR/'release-runtime-manifest.json').read_text(encoding='utf8'))
    runtime.update(package='2.0.6',candidate=LABEL,localOnly=True)
    for name in ('release-runtime-manifest.json','runtime/experimental/release-runtime-manifest.json'):
        document(STAGE/name,runtime)
    notes='''# Veyra 2.0.6-fixes-20261010 本地测试版（NVIDIA）

完整解压到新目录，运行 veyra_qml_ui.exe；不要只复制 EXE 覆盖旧版。
本包为本地测试候选，未发布正式新版。优先复测：播放中切换设置后的软件错误、手动输入数字后快捷键、关闭效果器时编辑参数的卡顿、长文字菜单和窄窗口布局。

包含已完成的播放列表/文件夹导入、PGS/SUP 等字幕、Blackmagic 原生采集开关和 HDR 补帧兼容开关。两个兼容开关默认关闭；Blackmagic 开关在采集设置，美乐威开关旁。BMD 原生驱动由 Desktop Video 提供，包内不安装驱动。显存持续增长的问题仍未根治。

超分保留此前实现：原尺寸 RTX Video SR 的执行门控及 FSR 首帧修复仍在。此次画质实验全部撤回，没有新增锐化、局部静态检测、整帧检测或额外 SR GPU pass，不宣称全面画质提升。实际开启/切换效果时仍有必要的初始化。

本机 RTX 5070 回归通过；这不是 AMD、BMD 实卡或所有用户环境通过的证明。
已批准的 NVIDIA/Intel/AMD 运行组件原样沿用，社区 NR 的 HashMismatch/NotSigned 身份保留在 release-runtime-manifest.json，软件不以清单锁定用户自行替换的 DLL。

应用源码：同目录上一级 Veyra-2.0.6-fixes-20261010-source.zip。
本次 FFmpeg/dav1d/zlib 对应源码：同目录上一级 FFmpeg-9.0.1-slices256-dav1d-zlib-source.zip；保留 PS5 H.264 slices256 补丁。
其余未变依赖源码： https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.0.6/Veyra-2.0.6-dependency-source.zip
构建方法见 docs/BUILD_2.0.6.md；本测试版增加 -DVEYRA_DISPLAY_VERSION=2.0.6-fixes-20261010，并明确配置 DeckLink SDK。源码包不含 SDK、运行库、模型、凭据或测试视频。
'''
    write(STAGE/'README-TEST.zh-CN.md',notes.encode('utf8'))

def manifest(source=None):
    data={k:v for k,v in donor.items() if k!='files'}
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    data.update(candidate=LABEL,displayVersion=LABEL,releaseReady=False,localOnly=True,
                baseCommit='c664a675c7b7a5c6cb54ed79c07b8a72127f4dfb',sourceArchiveCommit=head,
                validationRecord='docs/SR_VISUAL_QUALITY_REPORT_2026-10-10.md',
                srExperimentsWithdrawn=True)
    data['knownIssues']=[*donor['knownIssues'],
        'No general SR quality gain from this round; all SR production experiments withdrawn',
        'Blackmagic native and AMD field results require affected hardware/user testing']
    data['correspondingSource'].update(application='../'+SOURCE.name,
        applicationSha256=sha(SOURCE) if source else None,
        ffmpeg='../'+FFSOURCE.name,ffmpegSha256=FFSHA,
        displayVersionOverride='-DVEYRA_DISPLAY_VERSION='+LABEL,
        deckLinkBuildOption='-DVEYRA_DECKLINK_SDK_DIR=<external SDK12.0 directory>')
    data['files']=payload();document(STAGE/'package-manifest.json',data)
    return data

def stage():
    assert not STAGE.exists(),STAGE
    verify_donor();STAGE.mkdir(parents=True)
    for row in donor['files']:
        pkg.validate_payload(row['path']);target=STAGE/row['path'];target.parent.mkdir(parents=True,exist_ok=True)
        os.link(DONOR/row['path'],target)
    refresh();manifest();runtime_audit();verify_donor()
    print('Staged',STAGE,flush=True)

def smoke():
    for p in psutil.process_iter(['name']):assert not (p.info['name'] or '').lower().startswith(('veyra','fg-presentation','flash-pixel')),p.info
    before=payload();results=[]
    for mode in ('gpu','software'):
        label='package-'+time.strftime('%Y%m%d-%H%M%S')+'-'+mode
        profile=BASE/'tests'/TASK/label;tmp=BASE/'tmp'/TASK/label;logs=BASE/'logs'/TASK/label
        for p in (profile,tmp,logs):p.mkdir(parents=True,exist_ok=False)
        document(profile/'qml-preferences.v1.json',{'language':'zh-CN','autoResume':False,'rememberPosition':False,
            'prewarmEnhancement':False,'gpuPriority':'normal','screenshotDir':str(logs/'screenshots'),'sentinel':'preserved'})
        env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QML_','QT_','QSG_'))}
        env.update(PATH=os.environ['WINDIR']+'/System32;'+os.environ['WINDIR'],TEMP=str(tmp),TMP=str(tmp),
            CUDA_CACHE_PATH=str(tmp/'cuda'),QML_DISK_CACHE_PATH=str(tmp/'qml-cache'),
            VEYRA_LOG_FILE=str(logs/'engine.log'),QT_FORCE_STDERR_LOGGING='1')
        if mode=='software':env['QT_QUICK_BACKEND']='software'
        command=[str(STAGE/'veyra_qml_ui.exe'),'--data-dir',str(profile),'--page','pro','--size','1280x900',
                 '--exit-after','16000','E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4']
        with (logs/'console.log').open('wb') as f:
            run=subprocess.run(command,cwd=STAGE,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=40)
        console=(logs/'console.log').read_text(encoding='utf8',errors='replace')
        engine=(logs/'engine.log').read_text(encoding='utf8',errors='replace')
        errors=[line for line in (console+'\n'+engine).splitlines() if re.search(
            r'ReferenceError:|TypeError:|\[ERROR\s*\]|\[FATAL\s*\]|D3D12 ERROR|D3D12 CORRUPTION|Cannot assign|Binding loop',line)]
        assert run.returncode==0 and not errors,(run.returncode,errors)
        assert 'GTAVI_An_Extended_Look_4K_Native.mp4' in engine,engine[-3000:]
        presented=[int(v) for v in re.findall(r'\brealPresented=(\d+)',engine)]
        assert presented and max(presented)>=100,engine[-3000:]
        assert json.loads((profile/'qml-preferences.v1.json').read_text(encoding='utf8'))['sentinel']=='preserved'
        assert before==payload(),'Sealed app changed or generated unlisted data'
        results.append(dict(mode=mode,exit=run.returncode,passed=True,command=command,logs=str(logs),
                            profile=str(profile),realPresented=max(presented),exeSha256=sha(STAGE/'veyra_qml_ui.exe')))
        print('Sealed package cold playback PASS',mode,flush=True)
    runtime_audit();verify_donor()
    document(OUT/'package-smoke.json',dict(passed=True,results=results,files=len(before)))

def source_archive():
    assert not subprocess.check_output(['git','status','--porcelain'],cwd=ROOT).strip(),'Commit source first'
    assert not SOURCE.exists(),SOURCE
    names=subprocess.check_output(['git','ls-files','-z'],cwd=ROOT).decode('utf8').split('\0')
    records=[];skipped=[];prefix=SOURCE.stem+'/'
    with zipfile.ZipFile(SOURCE,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name in sorted(filter(None,names)):
            pkg.relative(name)
            if '__pycache__' in name or name.endswith('.pyc'):skipped.append(name);continue
            assert not re.search(r'\.(dll|exe|lib|pdb|obj|onnx|pth|zip|7z|ptx|addon64)$|(^|/)(runtime_local|third_party_local|\.git)/',name,re.I),name
            f=ROOT/name;assert not f.is_symlink(),name
            data=f.read_bytes();z.writestr(prefix+name,data)
            records.append(dict(path=name,size=len(data),sha256=hashlib.sha256(data).hexdigest()))
        metadata=dict(schema=1,commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
            displayVersion=LABEL,exeSha256=sha(BUILD/'veyra_qml_ui.exe'),files=records,excludedTrackedCache=skipped,
            dependencySource=donor['correspondingSource']['dependencies'],
            dependencySha256=donor['correspondingSource']['dependencySha256'],ffmpegSource=FFSOURCE.name,ffmpegSha256=FFSHA)
        z.writestr(prefix+'source-snapshot-manifest.json',json.dumps(metadata,ensure_ascii=False,indent=2))
    with zipfile.ZipFile(SOURCE) as z:
        assert z.testzip() is None
        for r in records:assert hashlib.sha256(z.read(prefix+r['path'])).hexdigest()==r['sha256'],r['path']
    return dict(path=str(SOURCE),sha256=sha(SOURCE),files=len(records),excludedTrackedCache=skipped)

def seal():
    smoke=json.loads((OUT/'package-smoke.json').read_text(encoding='utf8'));assert smoke['passed']
    assert all(r['exeSha256']==sha(BUILD/'veyra_qml_ui.exe') for r in smoke['results'])
    verify_donor();source=source_archive();refresh();data=manifest(source);count=runtime_audit()
    assert sha(FFSOURCE)==FFSHA
    with zipfile.ZipFile(FFSOURCE) as z:assert z.testzip() is None
    linked=OUT/FFSOURCE.name
    if not linked.exists():os.link(FFSOURCE,linked)
    assert sha(linked)==FFSHA
    archive=Path(str(STAGE)+'.zip');assert not archive.exists(),archive
    with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for row in data['files']:z.write(STAGE/row['path'],STAGE.name+'/'+row['path'])
        z.write(STAGE/'package-manifest.json',STAGE.name+'/package-manifest.json')
    verified=pkg.verify_zip(archive)
    with zipfile.ZipFile(archive) as z:assert z.testzip() is None
    verify_donor();assert data['files']==payload()
    for p in (archive,SOURCE,linked):
        write(Path(str(p)+'.sha256'),(sha(p)+'  '+p.name+'\n').encode('ascii'))
    receipt=dict(passed=True,archive=verified,source=source,ffmpegSource=dict(path=str(linked),sha256=FFSHA),
        displayVersion=LABEL,sourceCommit=data['sourceArchiveCommit'],exeSha256=sha(STAGE/'veyra_qml_ui.exe'),
        runtimeRecordsVerified=count,donorUnchanged=len(donor['files']),srProductionChanges=0,
        byteSize=archive.stat().st_size,smoke=str(OUT/'package-smoke.json'))
    document(OUT/'package.json',receipt);document(REPORT/'package-current.json',receipt)
    print(json.dumps(receipt,ensure_ascii=False,indent=2),flush=True)

assert sys.argv[1:] in (['stage'],['smoke'],['seal'])
{'stage':stage,'smoke':smoke,'seal':seal}[sys.argv[1]]()

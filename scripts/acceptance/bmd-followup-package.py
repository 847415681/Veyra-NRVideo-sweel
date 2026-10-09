"""One shared private app and a small overlay; keep runtimes unchanged."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,sys,zipfile
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='blackmagic-followup-20261009'
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
OUT=BASE/'test-packages'/TASK;APP=BASE/'tests'/TASK/'app';BUILD=BASE/'build'/TASK
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def build():return json.loads(Path(json.loads((BASE/'reports'/TASK/'build-current.json').read_text(encoding='utf8'))['receipt']).read_text(encoding='utf8'))
def auditDonor():
 manifest=json.loads((DONOR/'package-manifest.json').read_text(encoding='utf-8-sig'))
 for row in manifest['files']:assert sha(DONOR/row['path'])==row['sha256'].lower(),row['path']
 return manifest
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/bmd-followup-control.py')],check=True)
receipt=build()
for n,d in receipt['sources'].items():assert sha(ROOT/n)==d,n
for n,d in receipt['executables'].items():assert sha(Path(n))==d['sha256'],n
manifest=auditDonor();OUT.mkdir(parents=True,exist_ok=True)
if sys.argv[1]=='prepare':
 assert not APP.exists();APP.parent.mkdir(parents=True,exist_ok=True)
 def linked(a,b):
  if Path(a).relative_to(DONOR).as_posix() in ('veyra_qml_ui.exe','qml/Veyra/Main.qml','THIRD_PARTY_NOTICES.md','package-manifest.json'):return shutil.copy2(a,b)
  os.link(a,b);return b
 shutil.copytree(DONOR,APP,copy_function=linked)
 shutil.copy2(BUILD/'veyra-bmd-followup.exe',APP/'veyra_qml_ui.exe');shutil.copy2(ROOT/'THIRD_PARTY_NOTICES.md',APP/'THIRD_PARTY_NOTICES.md')
 (BASE/'reports'/TASK/'app.json').write_text(json.dumps({'path':str(APP),'exeSha256':sha(APP/'veyra_qml_ui.exe'),'donorFilesVerified':len(manifest['files']),'sharedDependenciesUnmodified':True},ensure_ascii=False,indent=2),encoding='utf8')
 print('Prepared one private app; verified donor files',len(manifest['files']));raise SystemExit(0)
assert sys.argv[1]=='package'
assert sha(APP/'veyra_qml_ui.exe')==sha(BUILD/'veyra-bmd-followup.exe')
payload={'veyra_qml_ui.exe':BUILD/'veyra-bmd-followup.exe','THIRD_PARTY_NOTICES.md':ROOT/'THIRD_PARTY_NOTICES.md','Start-BMD-Capture-Test.ps1':ROOT/'scripts/diagnostics/Start-BMD-Capture-Test.ps1','licenses/capture/LIBDSHOWCAPTURE_LGPL.txt':ROOT/'licenses/capture/LIBDSHOWCAPTURE_LGPL.txt'}
# Use the complete matching QML/shader set; no new DLL/model/SDK is distributed.
for folder in ('qml/Veyra','shaders'):
 for p in (DONOR/folder).rglob('*'):
  if p.is_file():payload[p.relative_to(DONOR).as_posix()]=p
readme='''BMD / Blackmagic / DeckLink 2.0.6 兼容测试补丁（非正式发布）

1. 保留原包，另复制/解压一份完整 Veyra 2.0.6 便携包；把本补丁解压覆盖到副本根目录。AMD/NVIDIA 继续用各自原包的运行库。
2. 关闭 OBS 和其它采集程序，运行 veyra_qml_ui.exe。选原来的 BMD 入口，使用 OBS 能出画时的完全相同分辨率、帧率、像素格式；59.94 与 60、1080p30 与 1080i60 不互换。先关闭增强确认基础画面，再开平时的增强。两个名称不证明物理上有两张卡。
3. 新兼容模式默认自动应用于 BMD。请测试两个入口、断开后重连。不要用任意模式代替实际输入制式；软件没有声称自动检测 BMD 信号。
4. 如需收集对照日志，在本目录打开 PowerShell，运行：
   powershell -NoProfile -ExecutionPolicy Bypass -File .\\Start-BMD-Capture-Test.ps1
   选择设备/格式，3分钟后自动退出并在 logs 中生成 zip。脚本使用私有配置，不改原设置。
   旧协商对照：同一命令末尾加 -Variant Legacy。两次请选完全相同的设备/模式/增强设置；不要同时开。
5. 如仍黑屏，把两次 zip 与 OBS 来源的属性（实际输入制式、卡型号、HDMI/SDI）一起交给开发者。日志中的黑像素统计不是“无信号”判定，正常黑场不会触发切换/丢帧。

本轮修复的是接收端与驱动的分配器/连接格式协商，保留驱动 DIB 方向；同时拒绝静默 subtype/隔行合同变更。已做本机 COM、GPU 像素、普通 USB 采集与 GUI 验证；没有本机 BMD 卡，实卡是否恢复出画仍待反馈。运行库/模型均沿用原包，本补丁不更换驱动。
基于当前 main dbfceab（包含已合入的 VFG 优化），此包仅为 BMD 本地候选，不是新版本发布。软件内构建字符串沿用缓存构建的 2.0.6-vfg-next，以 bmd-compat-manifest.json 中 EXE SHA 为本候选身份。
'''
files={n:{'sha256':sha(p),'bytes':p.stat().st_size} for n,p in payload.items()}
meta={'schema':1,'version':'2.0.6','candidate':'bmd-followup-20261009','localOnly':True,'releaseReady':False,'baseMain':receipt['baseMain'],'displayVersion':'2.0.6-vfg-next','realBmdHardwareValidated':False,'runtimeChanges':False,'appliesTo':'Blackmagic/DeckLink native raw video; v210 keeps the existing system conversion path','correspondingSource':'Veyra-2.0.6-BMD-compat-source.zip','compiledSources':receipt['sources'],'files':files}
target=OUT/'Veyra-2.0.6-BMD-compat-test.zip';assert not target.exists()
with zipfile.ZipFile(target,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
 for name,p in sorted(payload.items()):z.write(p,name)
 z.writestr('使用说明.txt',readme.encode('utf-8-sig'));z.writestr('bmd-compat-manifest.json',json.dumps(meta,ensure_ascii=False,indent=2).encode('utf8'))
with zipfile.ZipFile(target) as z:
 assert z.testzip() is None
 for name,row in files.items():assert hashlib.sha256(z.read(name)).hexdigest()==row['sha256'],name
 assert not any(Path(n).suffix.lower() in ('.dll','.onnx','.pdb','.lib','.hipfb') for n in z.namelist())
auditDonor()
result={'path':str(target),'bytes':target.stat().st_size,'sha256':sha(target),'files':len(files)+2,'crcAndEveryPayloadShaVerified':True,'exeSha256':sha(BUILD/'veyra-bmd-followup.exe'),'donorFilesVerified':len(manifest['files'])}
(BASE/'reports'/TASK/'package.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8');print(json.dumps(result,ensure_ascii=False))

"""Local overlay with exact corresponding source; no runtime/SDK/model upload."""
from pathlib import Path
import hashlib,io,json,os,shutil,subprocess,zipfile
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='fg-flash-fix-20261009'
OUT=BASE/'test-packages'/TASK;OUT.mkdir(parents=True,exist_ok=True)
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert not subprocess.check_output(['git','status','--porcelain'],cwd=ROOT)
commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,encoding='ascii').strip()
exe=BASE/'build'/TASK/'veyra-hdr-flash-fix.exe'
receipt=json.loads((BASE/'logs'/TASK/'build/receipt.json').read_text(encoding='utf8'))
assert sha(exe)==receipt['executables'][str(exe)]['sha256']
productSources={rel.replace('\\','/'):digest for rel,digest in receipt['sources'].items() if rel.replace('\\','/').startswith(('src/','include/'))}
assert set(productSources)=={'include/veyra/engine/VideoPresenter.h','src/engine/VideoPresenter.cpp','src/engine/EngineController.cpp','src/engine/EngineControllerImage.cpp'}
for rel,digest in productSources.items():assert sha(ROOT/rel)==digest,rel
source=OUT/'Veyra-2.0.6-HDR-flash-fix-source.zip';assert not source.exists()
subprocess.run(['git','archive','--format=zip','--prefix=Veyra-hdr-flash-fix/','--output='+str(source),'HEAD'],cwd=ROOT,check=True)
with zipfile.ZipFile(source) as z:
 assert z.testzip() is None
 for n in z.namelist():assert Path(n).suffix.lower() not in ('.dll','.lib','.exe','.pdb','.onnx','.cubin','.hipfb'),n
 for rel,digest in productSources.items():
  assert z.read('Veyra-hdr-flash-fix/'+rel).replace(b'\r\n',b'\n')==(ROOT/rel).read_bytes().replace(b'\r\n',b'\n'),rel
stage=OUT/'overlay';stage.mkdir(exist_ok=False)
shutil.copy2(exe,stage/'veyra_qml_ui.exe')
shutil.copy2(ROOT/'scripts/diagnostics/Start-HDR-Flash-Fix.ps1',stage/'Start-HDR-Flash-Fix.ps1')
os.link(source,stage/source.name)
for name in ('LICENSE','LICENSE.md','LICENSE.txt','THIRD_PARTY_NOTICES.md'):
 if (ROOT/name).is_file():shutil.copy2(ROOT/name,stage/name)
for name,variant in [('01-Test-Compatible.cmd','Compatible'),('02-Test-Direct-Control.cmd','Direct')]:
 (stage/name).write_text('@echo off\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-HDR-Flash-Fix.ps1" -AppDirectory "%~dp0." -Variant '+variant+'\npause\n',encoding='ascii')
text='''Veyra 2.0.6 · HDR补帧闪黑兼容候选（2026-10-09）

这是可回退测试候选，本机验证通过；用户RTX5090/采集卡上的闪黑是否消失仍待复测，不能认定已经根治。

使用：
1. 关闭所有Veyra。先把原目录的veyra_qml_ui.exe复制为veyra_qml_ui.original.exe.bak保留，再将本包覆盖到完整2.0.6 NVIDIA便携目录。不要改EXE名字，不要放到缺少Qt/运行库的空目录直接运行。
2. 原版提前保存好同一输入与画质/补帧设置。双击01-Test-Compatible.cmd，使用原来会闪的4K HDR采集、源时间戳和补帧设置。默认不会定时退出，测完手动关闭即可。
3. 双击02-Test-Direct-Control.cmd，用同一输入/设置再测。它运行同一个EXE、同一路径，只关闭新复制路径。两个启动器都从原设置复制到私有目录，测试内修改不会写回原设置；不要在两次之间改驱动配置。
4. 日志与回执自动打包在logs/hdr-flash-*.zip，只包含日志，不含私有配置/凭据。把两次ZIP与是否闪的结果一起反馈。Compatible不闪、Direct闪才支持呈现路径假说；两者都不闪仍不能认定根治。

改变：NVIDIA HDR10 + DLSS/VFG/应用侧FSR补帧，先GPU合成到独立纹理再复制到交换链。默认SDR、无补帧、AMD、XeSS保持直接呈现。没有像素采样、额外CPU等待或自动降倍率。4K额外纹理固定约31.9MiB。
本机GTA6 4K输入/1080p原版NR/DLSS2X测试：两路径播放速率1.000、跳源0；1280×720合成GPU窗口P95中位数0.034→0.039ms。该数据不能外推5090或面板扫描。

基于当前main，包含此前已经合入的VFG优化。Direct只代表当前main原呈现路径，不能完全替代旧2.0.6整包对照。
没有更换运行库/模型，没有修改NVIDIA App/RTSS或驱动设置。若只有改EXE名字就不闪，需要另查程序级配置，不能归功于本候选。

回退：关闭Veyra，把备份EXE复制回veyra_qml_ui.exe即可；DLL和原用户设置保持。标准双击EXE会按正常产品行为使用原设置，两个测试启动器则使用私有副本。
精确完整源码：同包Veyra-2.0.6-HDR-flash-fix-source.zip；源码和构建身份见hdr-flash-fix-manifest.json。完整2.0.6依赖/许可证沿用原包。
'''
(stage/'使用说明.txt').write_text(text,encoding='utf-8-sig')
shutil.copy2(ROOT/'docs/FG_FLASH_FIX_REPORT_2026-10-09.md',stage/'排查与测试报告.md')
manifest={'kind':'local-hdr-flash-compatibility-candidate','version':'2.0.6','sourceCommit':commit,'baseMain':'dbfceabee804cd81bac4a4781f0b7b4cf272902e','baseProduct':'71723e0249a17764696796fd204a6eb69d6da276','executableSha256':sha(exe),'sourceArchive':source.name,'sourceSha256':sha(source),'runtimeIncluded':False,'pixelProbeIncluded':False,'field5090Validated':False}
(stage/'hdr-flash-fix-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf8')
files={p.name:sha(p) for p in stage.iterdir() if p.is_file()}
target=OUT/'Veyra-2.0.6-HDR-flash-fix-test.zip';assert not target.exists()
with zipfile.ZipFile(target,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
 for name in sorted(files):z.write(stage/name,name,compress_type=zipfile.ZIP_STORED if name.endswith('.zip') else zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(target) as z:
 assert z.testzip() is None
 for name,digest in files.items():assert hashlib.sha256(z.read(name)).hexdigest()==digest,name
 assert z.namelist().count('veyra_qml_ui.exe')==1
 assert json.loads(z.read('hdr-flash-fix-manifest.json'))==manifest
assert sha(stage/'veyra_qml_ui.exe')==sha(BASE/'tests'/TASK/'app/veyra_qml_ui.exe')
result={'sourceCommit':commit,'package':{'path':str(target),'bytes':target.stat().st_size,'sha256':sha(target),'files':files},'source':{'path':str(source),'bytes':source.stat().st_size,'sha256':sha(source)},'passed':True}
(BASE/'reports'/TASK/'package-check.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps({k:v for k,v in result.items() if k!='package'},ensure_ascii=False));print('Package:',target.stat().st_size,sha(target))

"""Stage a complete local VFG candidate, preserving approved 2.0.6 payloads."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,sys,zipfile
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='vfg-next-20261007'
PACKAGE=BASE/'releases/release-2.0.6-20261007/packages/Veyra-2.0.6-NVIDIA-win64-portable'
OUT=BASE/'test-packages'/TASK;APP=OUT/'Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
VERIFY=BASE/'verify'/TASK
def sha(p):
 with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT,encoding='utf8').strip()
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/vfg-next-control.py')],cwd=ROOT,check=True)
assert not git('status','--porcelain=v1'),'Commit the reviewed source and test helpers first'
build=json.loads((BASE/'logs'/TASK/'candidate-final.json').read_text(encoding='utf8'));assert build['exit']==0
head=build['sourceHead'];assert head==git('rev-parse','HEAD')
for path,digest in build['sourceFiles'].items():assert sha(ROOT/path)==digest,path
measure=json.loads((VERIFY/'measurements.json').read_text(encoding='utf8'))
assert measure['passed'] and measure['exeSha256']==sha(BASE/'build'/TASK/'veyra_qml_ui.exe')
old=json.loads((PACKAGE/'package-manifest.json').read_text(encoding='utf-8-sig'))
for item in old['files']:assert sha(PACKAGE/item['path'])==item['sha256'].lower(),item['path']
assert not APP.exists();OUT.mkdir(parents=True,exist_ok=True);VERIFY.mkdir(parents=True,exist_ok=True)
shutil.copytree(PACKAGE,APP,copy_function=shutil.copy2)
shutil.copy2(BASE/'build'/TASK/'veyra_qml_ui.exe',APP/'veyra_qml_ui.exe')
notes=ROOT/'docs/VFG_NEXT_REPORT_2026-10-07.md';assert notes.is_file()
assert '状态：本机VFG定向验收通过' in notes.read_text(encoding='utf8')
shutil.copy2(notes,APP/'VFG-NEXT-NOTES.zh-CN.md')
source=OUT/'Veyra-2.0.6-VFG-next-source.zip';assert not source.exists()
subprocess.run(['git','archive','--format=zip','--prefix=Veyra-vfg-next/','--output='+str(source),head],cwd=ROOT,check=True,timeout=90)
manifest={'schema':3,'version':'2.0.6','candidate':'2.0.6-vfg-next','displayVersion':'2.0.6-vfg-next','releaseReady':False,'localOnly':True,'gpuPackage':'NVIDIA',
 'baseCommit':head,'sourceArchiveCommit':head,'publishedBaseVersion':'2.0.6','publishedBaseExeSha256':sha(PACKAGE/'veyra_qml_ui.exe'),
 'correspondingSource':{'application':'../'+source.name,'applicationSha256':sha(source),'dependencies':old['correspondingSource']['dependencies'],'dependencySha256':old['correspondingSource']['dependencySha256'],'instructions':'docs/BUILD_2.0.6.md','displayVersionOverride':'-DVEYRA_DISPLAY_VERSION=2.0.6-vfg-next'},
 'validationRecord':'docs/VFG_NEXT_REPORT_2026-10-07.md',
 'knownIssues':['Native 4K High quality above 2X remains over budget on RTX5070; requested 8X can deliberately fall back to stable 2X','Submission FPS is not physical scanout or measured end-to-end latency','NVIDIA VRAM growth remains unresolved; only normal bounded VFG checks were run'],
 'files':[]}
for file in sorted(APP.rglob('*')):
 if not file.is_file() or file.name=='package-manifest.json':continue
 rel=file.relative_to(APP).as_posix();original=PACKAGE/rel
 if original.is_file():
  assert not os.path.samefile(file,original),'Candidate must not share writable hardlinks'
  if rel!='veyra_qml_ui.exe':assert sha(file)==sha(original),rel
 manifest['files'].append({'path':rel,'size':file.stat().st_size,'sha256':sha(file)})
(APP/'package-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf8')
archive=OUT/(APP.name+'.zip');assert not archive.exists()
with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
 for file in sorted(APP.rglob('*')):
  if file.is_file():z.write(file,file.relative_to(OUT).as_posix())
with zipfile.ZipFile(archive) as z:assert z.testzip() is None
receipt={'app':str(APP),'exeSha256':sha(APP/'veyra_qml_ui.exe'),'source':str(source),'sourceSha256':sha(source),'files':len(manifest['files']),'runtimeAndQmlUnchanged':True,'independentCopies':True,'buildSource':head,'archive':str(archive),'archiveSha256':sha(archive),'archiveBytes':archive.stat().st_size,'packageManifestSha256':sha(APP/'package-manifest.json')}
(VERIFY/'candidate-package.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
(OUT/'SHA256SUMS.txt').write_text(sha(archive)+'  '+archive.name+'\n'+sha(source)+'  '+source.name+'\n',encoding='ascii')
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/vfg-next-control.py')],cwd=ROOT,check=True)
print(json.dumps(receipt,ensure_ascii=False),flush=True)

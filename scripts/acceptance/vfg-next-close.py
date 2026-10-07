"""Close the local candidate with source/ZIP evidence and safe task-only cleanup."""
from pathlib import Path
import ast,hashlib,json,re,shutil,subprocess,sys,zipfile,psutil
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='vfg-next-20261007';VERIFY=BASE/'verify'/TASK;LOG=BASE/'logs'/TASK
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
 with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT,encoding='utf8').strip()
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/vfg-next-control.py')],cwd=ROOT,check=True)
assert not any(p.info['name'].lower() in ('veyra.exe','veyra_qml_ui.exe','veyra_vfg_gpu_tests.exe','veyra_vfg_export_probe.exe') for p in psutil.process_iter(['name']))
subprocess.run(['git','diff','--check'],cwd=ROOT,check=True);assert not git('status','--porcelain=v1')
measure=read(VERIFY/'measurements.json');package=read(VERIFY/'candidate-package.json');cold=read(VERIFY/'candidate-cold.json');build=read(LOG/'candidate-final.json')
assert measure['passed'] and cold['passed'] and build['exit']==0
assert measure['exeSha256']==package['exeSha256']==cold['exeSha256']==sha(BASE/'build'/TASK/'veyra_qml_ui.exe')
assert package['buildSource']==build['sourceHead']==git('rev-parse','HEAD')
for path,digest in build['sourceFiles'].items():assert sha(ROOT/path)==digest,path
assert not git('diff','--name-only',measure['productSourceCommit'],'HEAD','--','src','include','shaders','qml','CMakeLists.txt','tests')
app=Path(package['app']);manifest=read(app/'package-manifest.json')
assert manifest['localOnly'] and not manifest['releaseReady'] and manifest['displayVersion']=='2.0.6-vfg-next'
assert sha(app/'package-manifest.json')==package['packageManifestSha256']
for row in manifest['files']:assert sha(app/row['path'])==row['sha256'],row['path']
assert {p.relative_to(app).as_posix() for p in app.rglob('*') if p.is_file()}=={r['path'] for r in manifest['files']}|{'package-manifest.json'}
assert sha(ROOT/'docs/VFG_NEXT_REPORT_2026-10-07.md')==sha(app/'VFG-NEXT-NOTES.zh-CN.md')
assert sha(Path(package['archive']))==package['archiveSha256']==cold['archiveSha256']
with zipfile.ZipFile(package['archive']) as z:
 assert z.testzip() is None
 assert {i.filename for i in z.infolist()}=={app.name+'/'+r['path'] for r in manifest['files']}|{app.name+'/package-manifest.json'}
 for row in manifest['files']:
  assert z.getinfo(app.name+'/'+row['path']).file_size==row['size'],row['path']
  with z.open(app.name+'/'+row['path']) as stream:assert hashlib.file_digest(stream,'sha256').hexdigest()==row['sha256'],row['path']
source=Path(package['source']);assert sha(source)==package['sourceSha256']
paths=list(build['sourceFiles']);assert all('\n' not in p for p in paths)
objects=subprocess.run(['git','cat-file','--batch'],input=''.join(package['buildSource']+':'+p+'\n' for p in paths).encode('utf8'),cwd=ROOT,stdout=subprocess.PIPE,check=True).stdout
committed={};offset=0
for path in paths:
 end=objects.index(b'\n',offset);header=objects[offset:end].split();assert len(header)==3 and header[1]==b'blob',path
 size=int(header[2]);offset=end+1;committed[path]=objects[offset:offset+size];offset+=size+1
assert offset==len(objects)
with zipfile.ZipFile(source) as z:
 assert z.testzip() is None
 for path in paths:
  archived=z.read('Veyra-vfg-next/'+path);blob=committed[path]
  assert archived==blob or (b'\0' not in blob and archived.replace(b'\r\n',b'\n')==blob.replace(b'\r\n',b'\n')),path
 for item in z.infolist():assert not re.search(r'\.(dll|lib|exe|pdb|onnx|f16|hsaco|cubin)$',item.filename,re.I),item.filename
pythonFiles=list((ROOT/'scripts/acceptance').glob('vfg-next-*.py'))
for path in pythonFiles:ast.parse(path.read_text(encoding='utf8'),filename=str(path))
targets=[BASE/'tests'/TASK/'app',VERIFY/'candidate-extracted',BASE/'tmp'/TASK]
cleanup=[]
for path in targets:
 # Validate absolute resolved targets before any recursive removal. No user
 # data, published directories, build, source, test evidence or package is removed.
 resolved=path.resolve();assert path.is_absolute() and resolved.is_relative_to(BASE.resolve()) and TASK in resolved.parts
 assert resolved==path and not path.is_symlink(),str(path)
 if path.exists():
  files=[p for p in path.rglob('*') if p.is_file()];size=sum(p.stat().st_size for p in files)
  cleanup.append({'path':str(path),'files':len(files),'bytes':size});shutil.rmtree(path)
result={'passed':True,'head':git('rev-parse','HEAD'),'branch':git('branch','--show-current'),'productSourceCommit':measure['productSourceCommit'],
 'exeSha256':package['exeSha256'],'sourceSha256':package['sourceSha256'],'archiveSha256':package['archiveSha256'],'candidate':str(app),'archive':package['archive'],
 'compiledInputsVerified':len(build['sourceFiles']),'pythonSyntaxFiles':len(pythonFiles),'matrixCases':len(measure['matrix']),'native':measure['native'],'exportCases':measure['exportCases'],'lifecycle':measure['lifecycle'],
 'cold':cold,'unchangedApprovedRuntimeAndQml':True,'publishedAndOther34WorktreesPreserved':True,'noSdkOrRuntimeAddedToGit':True,'cleanup':cleanup,'removedBytes':sum(x['bytes'] for x in cleanup),
 'measurementReceiptSha256':sha(VERIFY/'measurements.json'),'packageManifestSha256':sha(app/'package-manifest.json'),'finalZipAllFilesAndCrcVerified':True,'sourceArchiveCompiledInputsVerified':True}
(LOG/'cleanup.json').write_text(json.dumps(cleanup,ensure_ascii=False,indent=2),encoding='utf8')
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/vfg-next-control.py')],cwd=ROOT,check=True)
(VERIFY/'final-check.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8')
print('VFG LOCAL FINAL AUDIT PASS',result['head'],len(manifest['files']),'payload files;',result['removedBytes'],'temporary bytes removed',flush=True)

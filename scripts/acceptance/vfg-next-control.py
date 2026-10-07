"""Independent VFG continuation guard; previous baselines are immutable."""
from pathlib import Path
import hashlib,json,subprocess
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='vfg-next-20261007'
START=BASE/'archives'/TASK/'start.json'
ALLOWED={'AGENTS.md','docs/WORKLOG.md','src/pipeline/VfgBackend.cpp','tests/integration/VfgGpuTests.cpp'}
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def git(*args,cwd=ROOT):return subprocess.check_output(['git',*args],cwd=cwd)
def others():
 out=[]
 for line in git('worktree','list','--porcelain').decode('utf8').splitlines():
  if not line.startswith('worktree '):continue
  p=Path(line[9:])
  if p.resolve()==ROOT.resolve():continue
  names=set(git('ls-files','-m','-o','--exclude-standard','-z',cwd=p).decode('utf8').split('\0'))-{''}
  out.append({'path':str(p),'head':git('rev-parse','HEAD',cwd=p).decode().strip(),'statusHex':git('status','--porcelain=v1','-z','--untracked-files=all',cwd=p).hex(),'changed':{n:sha(p/n) if (p/n).is_file() else None for n in sorted(names)}})
 return out
assert sha(START)=='13b729312a19c06f41642d4451dd4f5d042a795937e934a3f70b715b9995091a','Immutable baseline changed'
start=json.loads(START.read_text(encoding='utf8'))
changed=[n for n,d in start['files'].items() if not (ROOT/n).is_file() or sha(ROOT/n)!=d]
current=set(git('ls-files','-c','-o','--exclude-standard','-z').decode('utf8').split('\0'))-{''}
changed+=sorted(current-set(start['files']))
assert all(n in ALLOWED or n.startswith(('docs/VFG_NEXT_','scripts/acceptance/vfg-next-')) for n in changed),changed
assert others()==start['otherWorktrees'],'Another worktree changed'
for n,d in start['immutable'].items():assert sha(Path(n))==d,n
print('VFG continuation guard passed:',len(set(changed)),'scope files;',len(start['otherWorktrees']),'other worktrees;',len(start['immutable']),'immutable files')

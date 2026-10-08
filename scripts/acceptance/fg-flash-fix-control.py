"""Read-only preservation/scope checks against this task's immutable start."""
from pathlib import Path
import hashlib,json,subprocess
ROOT=Path(__file__).resolve().parents[2];REPORT=Path('E:/项目/Veyra/reports/fg-flash-fix-20261009')
def git(*args,cwd=ROOT):return subprocess.check_output(['git',*args],cwd=cwd)
before=json.loads((REPORT/'pre-document-state.json').read_text(encoding='utf8'))
errors=[]
for name,expected in before.items():
 p=Path(name)
 names=sorted(set(v.decode('utf8') for v in git('ls-files','-m','-d','--others','--exclude-standard','-z',cwd=p).split(b'\0') if v))
 actual={'head':git('rev-parse','HEAD',cwd=p).decode().strip(),'statusSha256':hashlib.sha256(git('status','--porcelain=v1','-z',cwd=p)).hexdigest(),
         'dirtyFiles':{n:hashlib.sha256((p/n).read_bytes()).hexdigest() if (p/n).is_file() else None for n in names}}
 if actual!=expected:errors.append(name)
allowed={'AGENTS.md','docs/WORKLOG.md','docs/FG_FLASH_FIX_PLAN_2026-10-09.md','docs/FG_FLASH_FIX_REPORT_2026-10-09.md',
 'include/veyra/engine/VideoPresenter.h','src/engine/VideoPresenter.cpp','tests/integration/FgPresentationTests.cpp','tests/integration/HdrCopyPresentationFixture.h',
 'scripts/acceptance/fg-flash-fix-build.py','scripts/acceptance/fg-flash-fix-test.py','scripts/acceptance/fg-flash-fix-control.py','scripts/acceptance/fg-flash-fix-package.py','scripts/diagnostics/Start-HDR-Flash-Fix.ps1'}
changed=set(git('diff','dbfceabee804cd81bac4a4781f0b7b4cf272902e','--name-only').decode('utf8').splitlines())
changed.update(git('ls-files','--others','--exclude-standard').decode('utf8').splitlines())
unexpected=sorted(changed-allowed)
assert not unexpected,unexpected
for n in changed:assert Path(n).suffix.lower() not in ('.dll','.lib','.exe','.pdb','.onnx','.cubin','.hipfb','.zip'),n
receipt={'originalWorktrees':len(before),'originalDirtyFiles':sum(len(v['dirtyFiles']) for v in before.values()),'changedOriginals':errors,'changedScope':sorted(changed),'passed':not errors}
(REPORT/'control.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps(receipt,ensure_ascii=False));assert not errors

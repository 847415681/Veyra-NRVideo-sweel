"""Read-only scope and preservation gate for the BMD follow-up."""
from pathlib import Path
import hashlib,json,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='blackmagic-followup-20261009'
subprocess.run([sys.executable,'-B',str(BASE/'reports'/TASK/'state.py'),'verify'],check=True)
allowed={'AGENTS.md','THIRD_PARTY_NOTICES.md','docs/WORKLOG.md','docs/BLACKMAGIC_FOLLOWUP_PLAN_2026-10-09.md','docs/BLACKMAGIC_FOLLOWUP_REPORT_2026-10-09.md',
 'include/veyra/source/NativeCaptureSink.h','src/source/NativeCaptureSink.cpp','src/source/CaptureCardSource.cpp',
 'tests/unit/CaptureColorContractTests.cpp','tests/unit/BmdAllocatorCases.h',
 'scripts/acceptance/bmd-followup-control.py','scripts/acceptance/bmd-followup-build.py','scripts/acceptance/bmd-followup-test.py','scripts/acceptance/bmd-followup-package.py','scripts/diagnostics/Start-BMD-Capture-Test.ps1'}
def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT).decode('utf8')
changed=set(git('diff','dbfceabee804cd81bac4a4781f0b7b4cf272902e','--name-only').splitlines())
changed.update(git('ls-files','--others','--exclude-standard').splitlines())
assert changed<=allowed,sorted(changed-allowed)
for n in changed:assert Path(n).suffix.lower() not in ('.exe','.dll','.pdb','.lib','.onnx','.zip','.hipfb','.cubin'),n
subprocess.run(['git','diff','--check'],cwd=ROOT,check=True,capture_output=True)
(BASE/'reports'/TASK/'control.json').write_text(json.dumps({'passed':True,'scope':sorted(changed),'baseMain':'dbfceabee804cd81bac4a4781f0b7b4cf272902e'},ensure_ascii=False,indent=2),encoding='utf8')
print('BMD preservation/scope passed:',len(changed),'files')

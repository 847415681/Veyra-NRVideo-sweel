"""Audit immutable pre-task worktrees and this task's narrow source scope."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE = Path('E:/项目/Veyra')
TASK = 'inactive-effects-20261010'
start = json.loads((BASE/'archives'/TASK/'start.json').read_text(encoding='utf8'))

def git(*args, cwd=ROOT):
    return subprocess.check_output(['git', *args], cwd=cwd)

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

for name, row in start['worktrees'].items():
    path = Path(name)
    assert git('rev-parse', 'HEAD', cwd=path).decode().strip() == row['head'], name
    assert hashlib.sha256(git('status', '--porcelain=v1', '-z', cwd=path)).hexdigest() == row['statusSha256'], name
    for relative, digest in row['dirtyFiles'].items():
        file = path/relative
        assert (sha(file) if file.is_file() else None) == digest, str(file)
assert git('rev-parse', 'main').decode().strip() == start['main']
assert git('branch', '--show-current').decode().strip() == 'codex/'+TASK
allowed = {'AGENTS.md','docs/WORKLOG.md','docs/INACTIVE_EFFECTS_PLAN_2026-10-10.md','docs/INACTIVE_EFFECTS_REPORT_2026-10-10.md',
'include/veyra/engine/EnhancementSettings.h','src/engine/EngineController.cpp','src/engine/EffectChain.cpp','src/pipeline/EnhanceGraph.cpp',
'tests/unit/EffectChainTests.cpp','tests/unit/RepairContractTests.cpp','tests/integration/LivePresentationTests.cpp',
'scripts/acceptance/inactive-effects-control.py','scripts/acceptance/inactive-effects-build.py','scripts/acceptance/inactive-effects-test.py'}
allowed.update({'src/ui/QmlPlayerBridge.cpp','scripts/acceptance/inactive-effects-gui.py','scripts/acceptance/inactive-effects.qml'})

dirty = {p.decode('utf8') for p in git('ls-files', '-m', '-d', '--others', '--exclude-standard', '-z').split(b'\0') if p}


changed = {p.decode('utf8') for p in git('diff', '--name-only', start['base']).splitlines() if p}
assert (changed | dirty) <= allowed, (changed | dirty) - allowed
git('diff', '--check')
git('diff', '--cached', '--check')
label = sys.argv[1] if len(sys.argv) > 1 else 'audit'
assert re.fullmatch('[a-z0-9-]+', label)
report = {'baselineWorktreesUnchanged': len(start['worktrees']),
          'dirtyFilesUnchanged': sum(len(v['dirtyFiles']) for v in start['worktrees'].values()),
          'scopePass': True, 'changed': sorted(changed | dirty), 'mainHead': start['main'], 'base': start['base']}
(BASE/'reports'/TASK/(label+'.json')).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
print(json.dumps(report, ensure_ascii=False), flush=True)

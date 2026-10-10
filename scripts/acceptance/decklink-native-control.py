"""Audit immutable pre-task worktrees and this task's narrow source scope."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE = Path('E:/项目/Veyra')
TASK = 'decklink-native-20261009'
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
allowed = {'THIRD_PARTY_NOTICES.md', 'src/pipeline/EnhanceGraph.cpp', 'src/source/DeckLinkDriver.cpp', 'scripts/acceptance/decklink-native-ui.qml', 'qml/Veyra/DialogHost.qml', 'scripts/acceptance/decklink-native-control.py', 'src/engine/EngineController.cpp', 'include/veyra/source/DeckLinkCapture.h', 'scripts/acceptance/decklink-native-test.py', 'tests/unit/DeckLinkCaptureTests.cpp', 'AGENTS.md', 'shaders/PackedCaptureToLinear.hlsl', 'include/veyra/pipeline/EnhanceGraph.h', 'i18n/catalog.json', 'src/source/DeckLinkCapture.cpp', 'docs/DECKLINK_NATIVE_REPORT_2026-10-09.md', 'include/veyra/source/CaptureCardSource.h', 'include/veyra/pipeline/ColorMetadata.h', 'tests/integration/HdrColorTests.cpp', 'include/veyra/source/CapturePixelFormat.h', 'docs/DECKLINK_NATIVE_PLAN_2026-10-09.md', 'include/veyra/pipeline/FramePacket.h', 'src/ui/QmlPlayerBridge.cpp', 'scripts/acceptance/decklink-native-build.py', 'docs/WORKLOG.md', 'scripts/acceptance/decklink-native-gui-test.py', 'CMakeLists.txt', 'include/veyra/pipeline/CaptureUploadFrame.h', 'src/source/CaptureCardSource.cpp', 'tests/integration/DeckLinkGpuCases.h'}
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

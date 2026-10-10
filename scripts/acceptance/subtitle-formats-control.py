"""Verify the immutable pre-task worktrees and the explicit subtitle scope."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='subtitle-formats-20261009'
start=json.loads((BASE/'archives'/TASK/'start.json').read_text(encoding='utf8'))
def git(*args,cwd=ROOT):return subprocess.check_output(['git',*args],cwd=cwd)
def sha(p):
    with p.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
for name,record in start['worktrees'].items():
    path=Path(name)
    assert git('rev-parse','HEAD',cwd=path).decode().strip()==record['head'],name
    assert hashlib.sha256(git('status','--porcelain=v1','-z',cwd=path)).hexdigest()==record['statusSha256'],name
    for relative,digest in record['dirtyFiles'].items():
        file=path/relative
        assert (sha(file) if file.is_file() else None)==digest,str(file)
assert git('branch','--show-current').decode().strip()=='codex/'+TASK
allowed={'AGENTS.md','CMakeLists.txt','THIRD_PARTY_NOTICES.md','docs/WORKLOG.md',
         'docs/SUBTITLE_FORMATS_PLAN_2026-10-09.md','docs/SUBTITLE_FORMATS_REPORT_2026-10-09.md',
         'i18n/catalog.json','include/veyra/engine/Subtitles.h','qml/Veyra/DialogHost.qml',
         'scripts/ffmpeg/README.md','scripts/ffmpeg/prepare-slice-build.py','scripts/package-ffmpeg-source.py','scripts/package-qml-release.py',
         'src/engine/Subtitles.cpp','src/ui/QmlPlayerBridge.cpp','tests/integration/SubtitleFormatTests.cpp',
         'tests/integration/SubtitleLoadingTests.cpp','tests/integration/SubtitleOverlayTests.cpp',
         'scripts/tests/make-subtitle-format-fixtures.py',
         'scripts/acceptance/subtitle-formats-build.py','scripts/acceptance/subtitle-formats-test.py',
         'scripts/acceptance/subtitle-formats-gui-test.py','scripts/acceptance/subtitle-formats-control.py'}
dirty={name.decode('utf8') for name in git('ls-files','-m','-d','--others','--exclude-standard','-z').split(b'\0') if name}
assert dirty<=allowed,dirty-allowed
changed={name.decode('utf8') for name in git('diff','--name-only',start['mainHead']).splitlines() if name}
assert changed<=allowed,changed-allowed
git('diff','--check');git('diff','--cached','--check')
label=sys.argv[1] if len(sys.argv)>1 else 'audit';assert re.fullmatch('[a-z0-9-]+',label)
report={'baselineWorktreesUnchanged':len(start['worktrees']),'dirtyFilesUnchanged':sum(len(row['dirtyFiles']) for row in start['worktrees'].values()),'scopePass':True,'changed':sorted(changed|dirty),'mainHead':start['mainHead']}
(BASE/'reports'/TASK/(label+'.json')).write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps(report,ensure_ascii=False),flush=True)

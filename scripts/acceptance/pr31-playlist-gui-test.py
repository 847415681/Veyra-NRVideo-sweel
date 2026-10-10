"""Actual player/bridge playlist checks, with a private app and profile only."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import psutil

if len(sys.argv) == 2 and sys.argv[1] == 'all':
    outcomes = [subprocess.run([sys.executable, '-B', __file__, case], timeout=120).returncode
                for case in ('functional', 'next', 'english', 'empty')]
    raise SystemExit(any(outcomes))

ROOT = Path(__file__).resolve().parents[2]
BASE = Path('E:/项目/Veyra'); TASK = 'pr31-playlist-20261009'
DONOR = BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
APP = BASE/'tests'/TASK/'app'; BUILD = BASE/'build'/TASK
FFMPEG = BASE/'deps/ffmpeg-subtitle-formats-20261009/installed/bin'
case = sys.argv[1]; assert case in ('functional', 'next', 'english', 'empty')
stamp = time.strftime('%Y%m%d-%H%M%S')
LOG = BASE/'logs'/TASK/('gui-'+case+'-'+stamp)
TMP = BASE/'tmp'/TASK/LOG.name
for path in (LOG, TMP): path.mkdir(parents=True, exist_ok=False)

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
manifest = json.loads((DONOR/'package-manifest.json').read_text(encoding='utf8'))

def verify():
    for row in manifest['files']: assert sha(DONOR/row['path']) == row['sha256'], row['path']

verify()
for proc in psutil.process_iter(['pid', 'name']):
    name = (proc.info['name'] or '').lower()
    assert not name.startswith(('veyra', 'fg-presentation', 'flash-pixel')), f'Player already running: {name} {proc.pid}'
if not APP.exists():
    APP.mkdir(parents=True)
    for row in manifest['files']:
        relative = Path(row['path'])
        if relative.parts[0] in ('runtime_local', 'logs') or relative.name == 'veyra_qml_ui.exe': continue
        target = APP/relative; target.parent.mkdir(parents=True, exist_ok=True)
        os.link(DONOR/relative, target)
    (APP/'runtime_local').mkdir(exist_ok=True)

def replace(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    swap = target.with_name(target.name+'.swap'); shutil.copy2(source, swap); os.replace(swap, target)

replace(BUILD/'veyra_qml_ui.exe', APP/'veyra_qml_ui.exe')
for source in (ROOT/'qml').rglob('*'):
    if source.is_file() and (source.suffix in ('.qml', '.js') or source.name == 'qmldir'):
        replace(source, APP/'qml'/source.relative_to(ROOT/'qml'))
for source in FFMPEG.glob('*.dll'): replace(source, APP/source.name)
profile = BASE/'tests'/TASK/'profiles'/LOG.name; profile.mkdir(parents=True, exist_ok=False)
(profile/'qml-preferences.v1.json').write_text(json.dumps({'language': 'en' if case == 'english' else 'zh-CN',
    'gpuPriority': 'normal', 'autoResume': False, 'rememberPosition': False, 'prewarmEnhancement': False,
    'subtitleEnabled': False, 'sentinel': 'preserved'}), encoding='utf8')
clips = BASE/'tests'/TASK/'playback-clips'; clips.mkdir(exist_ok=True)
other = clips/'other'; other.mkdir(exist_ok=True)
original = BASE/'tests/playback-smoothness-20261004/fixtures/golden.mkv'
for target in (clips/'Movie1.mkv', other/'Movie2.mkv', other/'Movie10.mkv'):
    if not target.exists(): os.link(original, target)

env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('VEYRA_', 'QT_', 'QSG_'))}
env.update(PATH=str(APP)+os.pathsep+env['PATH'], TEMP=str(TMP), TMP=str(TMP),
           CUDA_CACHE_PATH=str(TMP/'cuda-cache'), QML_DISK_CACHE_PATH=str(TMP/'qml-cache'),
           QT_FORCE_STDERR_LOGGING='1', VEYRA_LOG_FILE=str(LOG/'engine.log'))
timer = r'''
    property int playlistTestStep: 0
    property int playlistTestTicks: 0
    property int playlistTestAt: 0
    property real playlistTestPosition: 0
    function playlistTestFail(text) { console.error("PLAYGUI_FAIL", text); playlistTestTimer.stop() }
    function playlistTestCheck(ok, text) {
        if (!ok) { playlistTestFail(text); return false }
        console.log("PLAYGUI_CHECK", text); return true
    }
    function playlistTestFind(item, name) {
        if (item.objectName === name) return item
        if (item.children) for (let i=0;i<item.children.length;++i) {
            const found=playlistTestFind(item.children[i],name); if(found)return found
        }
        return null
    }
    function playlistTestDone() {
        playlistWindow.contentItem.grabToImage(result=>result.saveToFile(SCREENSHOT))
        console.log("PLAYGUI_PASS", MODE); playlistTestTimer.stop()
        playlistTestClose.start()
    }
    Timer { id: playlistTestClose; interval: 500; onTriggered: veyra.quit() }
    Timer {
        id: playlistTestTimer; interval: 150; repeat: true; running: true
        onTriggered: {
            ++root.playlistTestTicks
            if(root.playlistTestTicks>150) { root.playlistTestFail("timeout step="+root.playlistTestStep); return }
            const p=veyra.playlist
            if(MODE==="empty") {
                if(root.playlistTestStep===0) {
                    if(!root.playlistTestCheck(!veyra.running && p.count===0,"starts without a source or fabricated entries"))return
                    veyra.showPlaylist(); root.playlistTestStep=1; return
                }
                if(root.playlistTestStep===1) {
                    if(!root.playlistTestCheck(playlistWindow.visible,"home playlist opens before a video"))return
                    veyra.openDroppedUrls([FOLDER]); root.playlistTestStep=2; return
                }
                if(p.busy)return
                if(!root.playlistTestCheck(p.count===2 && !veyra.running,"folder drop imports without starting playback"))return
                root.playlistTestDone(); return
            }
            if(root.playlistTestStep===0) {
                if(!veyra.running || p.busy || !p.count || veyra.duration<2)return
                if(!root.playlistTestCheck(p.count===1 && p.currentIndex===0,"actual open is in the playlist"))return
                if(!root.playlistTestCheck(veyra.preferences.hdrFgCompatibility!==true,"HDR compatibility remains default-off"))return
                p.autoAdvance=false
                if(!veyra.paused)veyra.togglePlayPause()
                root.playlistTestStep=1; return
            }
            if(root.playlistTestStep===1) {
                if(!veyra.paused)return
                root.playlistTestPosition=veyra.position
                const button=root.playlistTestFind(fullBar.contentItem,"cine-playlist")
                if(!root.playlistTestCheck(button!==null,"minimal bottom bar contains playlist button"))return
                button.clicked()
                veyra.openDroppedUrls([FOLDER])
                root.playlistTestStep=2; return
            }
            if(root.playlistTestStep===2) {
                if(p.busy)return
                if(!root.playlistTestCheck(playlistWindow.visible && p.count===3,"folder appends and owned floating playlist opens"))return
                if(!root.playlistTestCheck(veyra.sourceName==="Movie1.mkv" && veyra.paused && Math.abs(veyra.position-root.playlistTestPosition)<0.2,"append does not change source/position/pause state"))return
                p.sortBy(1)
                if(MODE==="next") {
                    p.autoAdvance=true; veyra.seekTo(veyra.duration-0.6)
                    root.playlistTestStep=20; return
                }
                p.move(0,2); root.playlistTestStep=3; return
            }
            if(root.playlistTestStep===3) {
                const badge=root.playlistTestFind(playlistWindow.contentItem,"playlist-state-2")
                if(!root.playlistTestCheck(p.currentIndex===2 && badge && badge.text===(MODE==="english"?"Paused":"已暂停"),"reorder keeps actual paused file marker"))return
                p.remove(0)
                if(!root.playlistTestCheck(p.currentIndex===1 && veyra.sourceName==="Movie1.mkv","removing another row keeps playback"))return
                p.remove(1)
                if(!root.playlistTestCheck(p.currentIndex===-1 && veyra.paused && veyra.sourceName==="Movie1.mkv","removing active row does not stop or mislabel playback"))return
                p.play(0); root.playlistTestStep=4; return
            }
            if(root.playlistTestStep===4) {
                if(!veyra.running || veyra.paused || veyra.openingSource)return
                if(!root.playlistTestCheck(veyra.sourceName==="Movie10.mkv" && p.currentIndex===0,"playlist selection opens the chosen real video"))return
                const badge=root.playlistTestFind(playlistWindow.contentItem,"playlist-state-0")
                if(!root.playlistTestCheck(badge && badge.text===(MODE==="english"?"Playing":"正在播放"),"playing marker follows confirmed engine state"))return
                playlistWindow.width=620; playlistWindow.height=480; playlistWindow.hide(); playlistWindow.open()
                if(!root.playlistTestCheck(playlistWindow.width===620 && playlistWindow.height===480,"playlist reopen preserves resized geometry"))return
                p.autoAdvance=true; veyra.stopPlayback(); root.playlistTestAt=root.playlistTestTicks
                root.playlistTestStep=5; return
            }
            if(root.playlistTestStep===5) {
                if(root.playlistTestTicks-root.playlistTestAt<5)return
                if(!root.playlistTestCheck(!veyra.running && veyra.sourceName==="Movie10.mkv","manual stop does not trigger another file"))return
                if(!root.playlistTestCheck(p.save(),"actual playlist persisted"))return
                root.playlistTestDone(); return
            }
            if(root.playlistTestStep===20) {
                if(veyra.position<veyra.duration-1)return
                veyra.togglePlayPause(); root.playlistTestStep=21; return
            }
            if(root.playlistTestStep===21) {
                if(veyra.sourceName!=="Movie2.mkv" || !veyra.running || veyra.openingSource)return
                if(!root.playlistTestCheck(p.currentIndex===1,"actual end advances once to the next sorted file"))return
                if(!veyra.paused)veyra.togglePlayPause()
                root.playlistTestAt=root.playlistTestTicks; root.playlistTestStep=22; return
            }
            if(root.playlistTestStep===22) {
                if(root.playlistTestTicks-root.playlistTestAt<4)return
                if(!root.playlistTestCheck(veyra.paused && veyra.sourceName==="Movie2.mkv" && p.currentIndex===1,"stale Ended snapshots do not skip another file"))return
                root.playlistTestDone(); return
            }
        }
    }
'''
for token, value in {'MODE': case, 'FOLDER': other.as_uri(), 'SCREENSHOT': str(LOG/'playlist.png')}.items():
    timer = timer.replace(token, json.dumps(value.replace('\\', '/'), ensure_ascii=False))
main = APP/'qml/Veyra/Main.qml'; saved = main.read_bytes()
assert saved.decode('utf8').count('    id: root') == 1
swap = main.with_suffix('.swap'); swap.write_text(saved.decode('utf8').replace('    id: root', '    id: root'+timer, 1), encoding='utf8'); os.replace(swap, main)
command = [str(APP/'veyra_qml_ui.exe'), '--data-dir', str(profile), '--page', 'home' if case == 'empty' else 'min',
           '--size', '1280x800', '--exit-after', '24000']
if case != 'empty': command.append(str(clips/'Movie1.mkv'))
start = time.monotonic()
try:
    with (LOG/'console.log').open('wb') as stream:
        run = subprocess.run(command, cwd=APP, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=45)
finally:
    swap = main.with_suffix('.swap'); swap.write_bytes(saved); os.replace(swap, main)
verify()
text = (LOG/'console.log').read_text(encoding='utf8', errors='replace')
errors = [line for line in text.splitlines() if re.search(r'PLAYGUI_FAIL|ReferenceError:|TypeError:|\[ERROR\s*\]|\[FATAL\s*\]|D3D12 ERROR|D3D12 CORRUPTION|Cannot assign|Binding loop', line)]
receipt = {'case': case, 'command': command, 'exit': run.returncode, 'seconds': time.monotonic()-start,
           'errors': errors, 'passed': 'PLAYGUI_PASS' in text and not errors and run.returncode == 0,
           'events': [line for line in text.splitlines() if 'PLAYGUI_' in line],
           'exeSha256': sha(APP/'veyra_qml_ui.exe'), 'donorUnchanged': len(manifest['files']), 'profile': str(profile)}
(LOG/'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf8')
(BASE/'reports'/TASK/('gui-'+case+'-current.json')).write_text(json.dumps({'receipt': str(LOG/'receipt.json')}, indent=2), encoding='utf8')
print(json.dumps(receipt, ensure_ascii=False, indent=2), flush=True)
raise SystemExit(not receipt['passed'])

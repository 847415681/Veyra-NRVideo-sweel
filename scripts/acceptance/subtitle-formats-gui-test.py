"""Real QML bridge checks in one private, hardlinked app and private profiles."""
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

ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='subtitle-formats-20261009'
DONOR=BASE/'test-packages/vfg-next-20261007/Veyra-2.0.6-VFG-next-NVIDIA-win64-portable'
APP=BASE/'tests'/TASK/'app';BUILD=BASE/'build'/TASK;FIX=BASE/'tests'/TASK/'formats'
FFMPEG=BASE/'deps'/('ffmpeg-'+TASK)/'installed/bin'
case=sys.argv[1];assert case in ('embedded','external','sidecar','english')
LOG=BASE/'logs'/TASK/('gui-'+case+'-'+time.strftime('%Y%m%d-%H%M%S'));LOG.mkdir(parents=True,exist_ok=False)
TMP=BASE/'tmp'/TASK/LOG.name;TMP.mkdir(parents=True,exist_ok=False)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
manifest=json.loads((DONOR/'package-manifest.json').read_text(encoding='utf8'))
def verify():
    for row in manifest['files']:assert sha(DONOR/row['path'])==row['sha256'],row['path']
verify()
for proc in psutil.process_iter(['pid','name']):
    name=(proc.info['name'] or '').lower()
    assert not name.startswith(('veyra','fg-presentation','flash-pixel')),f'Player already running: {name} {proc.pid}'
if not APP.exists():
    APP.mkdir(parents=True)
    for row in manifest['files']:
        rel=Path(row['path'])
        if rel.parts[0] in ('runtime_local','logs') or rel.name=='veyra_qml_ui.exe':continue
        target=APP/rel;target.parent.mkdir(parents=True,exist_ok=True)
        if rel.as_posix()=='qml/Veyra/Main.qml':shutil.copy2(DONOR/rel,target)
        else:os.link(DONOR/rel,target)
    (APP/'runtime_local').mkdir(exist_ok=True)
def replace(source,target):
    target.parent.mkdir(parents=True,exist_ok=True)
    swap=target.with_name(target.name+'.swap');shutil.copy2(source,swap);os.replace(swap,target)
for rel in ('veyra_qml_ui.exe','qml/Veyra/SettingsPage.qml','qml/Veyra/DialogHost.qml'):
    replace(BUILD/rel if rel.endswith('.exe') else ROOT/rel,APP/rel)
for source in FFMPEG.glob('*.dll'):replace(source,APP/source.name)
for source,relative in (
    (FFMPEG.parent/'share/ffmpeg/veyra-local-build.json','licenses/FFMPEG-VEYRA-BUILD.json'),
    (FFMPEG.parent/'share/zlib/copyright','licenses/ZLIB-COPYRIGHT.txt'),
    (FFMPEG.parent/'share/zlib/vcpkg.spdx.json','licenses/ZLIB-SPDX.json'),
    (ROOT/'THIRD_PARTY_NOTICES.md','THIRD_PARTY_NOTICES.md'),
):replace(source,APP/relative)
profile=BASE/'tests'/TASK/'profiles'/LOG.name;profile.mkdir(parents=True,exist_ok=False)
(profile/'qml-preferences.v1.json').write_text(json.dumps({'language':'en' if case=='english' else 'zh-CN','gpuPriority':'normal','autoResume':False,'prewarmEnhancement':False,'subtitleEnabled':True,'sentinel':'preserved'}),encoding='utf8')
media=BASE/'downloads'/TASK/'pgs-real.mkv'
if case=='sidecar':
    folder=BASE/'tests'/TASK/'sidecar';folder.mkdir(exist_ok=True)
    media=folder/'movie.mkv'
    if not media.exists():os.link(BASE/'tests/5060-presets-subtitles-20260918/bitmap.mkv',media)
    replace(FIX/'bitmap.sup',folder/'movie.sup')
env={k:v for k,v in os.environ.items() if not k.upper().startswith(('VEYRA_','QT_','QSG_'))}
env.update(PATH=str(APP)+os.pathsep+env['PATH'],TEMP=str(TMP),TMP=str(TMP),CUDA_CACHE_PATH=str(TMP/'cuda-cache'),QML_DISK_CACHE_PATH=str(TMP/'qml-cache'),QT_FORCE_STDERR_LOGGING='1',VEYRA_LOG_FILE=str(LOG/'engine.log'))
timer=r'''
    property int subTestStep: 0
    property int subTestTicks: 0
    property int subTestBase: 0
    function subTestFail(message) { console.error("SUBGUI_FAIL", message); subTestTimer.stop() }
    function subTestCheck(ok, message) { if (!ok) { subTestFail(message); return false }; console.log("SUBGUI_CHECK", message); return true }
    function subTestFind(item, name) {
        if(item.objectName === name) return item
        if(item.children) for(let k=0;k<item.children.length;++k) { const found=subTestFind(item.children[k],name); if(found)return found }
        return null
    }
    Timer {
        id: subTestTimer; interval: 200; repeat: true; running: true
        onTriggered: {
            ++root.subTestTicks
            if(root.subTestTicks>85) { root.subTestFail("timeout step="+root.subTestStep); return }
            const mode=MODE
            const tracks=veyra.subtitleTracks
            if(root.subTestStep===0) {
                if(!veyra.running || !tracks.length || !tracks.every(t=>t.usable)) return
                if(!root.subTestCheck(!veyra.preferences.hdrFgCompatibility,"HDR compatibility stays off by default"))return
                if(!root.subTestCheck(tracks.every(t=>t.note.indexOf("不可用")<0),"decoded PGS is selectable"))return
                console.log("SUBGUI_TRACKS",JSON.stringify(tracks))
                if(mode==="sidecar") {
                    if(!root.subTestCheck(tracks.length===2 && !tracks[0].embedded && tracks[1].embedded,"same-name bitmap file is loaded beside embedded PGS"))return
                } else if(!root.subTestCheck(tracks.length===1 && tracks[0].embedded,"compressed PGS is available in the actual player"))return
                if(!veyra.paused)veyra.togglePlayPause()
                veyra.seekTo(2)
                root.subTestBase=tracks.length
                if(mode==="external") {
                    const start=Date.now()
                    if(!root.subTestCheck(veyra.loadSubtitleFile(VOBSUB),"external load accepted"))return
                    if(!root.subTestCheck(Date.now()-start<100,"external load returns without blocking QML"))return
                    root.subTestStep=1;return
                }
                if(mode==="english") {
                    veyra.loadSubtitleFile(BROKEN);root.subTestStep=7;return
                }
                veyra.subtitlePrimary=0;veyra.subtitleOffsetMs=250
                if(!root.subTestCheck(veyra.subtitleTracks[0].offsetMs===250,"PGS offset applies to selected track"))return
                root.subTestStep=9
            } else if(root.subTestStep===1) {
                if(tracks.length!==root.subTestBase+2)return
                if(!root.subTestCheck(!tracks[0].embedded&&!tracks[1].embedded&&tracks[0].usable&&tracks[1].usable,"both VobSub languages enter the list"))return
                veyra.subtitlePrimary=1;veyra.subtitleOffsetMs=375
                veyra.subtitlePrimary=0;veyra.subtitleOffsetMs=-250;veyra.subtitleSecondary=1
                if(!root.subTestCheck(veyra.subtitlePrimary===0&&veyra.subtitleSecondary===1&&veyra.subtitleTracks[0].offsetMs===-250&&veyra.subtitleTracks[1].offsetMs===375,"primary secondary and per-track offsets remain independent"))return
                veyra.loadSubtitleFile(BROKEN);root.subTestStep=2
            } else if(root.subTestStep===2) {
                if(veyra.subtitleStatus.indexOf("加载失败")<0)return
                if(!root.subTestCheck(tracks.length===root.subTestBase+2&&veyra.subtitlePrimary===0,"bad file preserves current subtitles"))return
                veyra.loadSubtitleFile(UNICODE);root.subTestStep=3
            } else if(root.subTestStep===3) {
                if(tracks.length!==root.subTestBase+3)return
                if(!root.subTestCheck(tracks[0].usable&&!tracks[0].embedded&&tracks[0].label.indexOf("中文")>=0,"manual Unicode text file loads asynchronously"))return
                veyra.loadSubtitleFile(SUP)
                veyra.openPath(MEDIA)
                root.subTestStep=4
            } else if(root.subTestStep===4) {
                if(!veyra.running || tracks.length!==1 || !tracks[0].embedded || !tracks[0].usable)return
                if(!root.subTestCheck(tracks.every(t=>t.embedded),"source change cancels pending external addition"))return
                root.subTestStep=9
            } else if(root.subTestStep===7) {
                if(veyra.subtitleStatus.indexOf("loading failed")<0)return
                if(!root.subTestCheck(veyra.subtitleStatus.indexOf("Subtitle decoding failed")>=0&&tracks.length===1,"final failure detail uses English catalog"))return
                root.subTestStep=9
            }
            if(root.subTestStep===9) {
                dialogs.open("subtitle");root.subTestStep=10;return
            }
            if(root.subTestStep===10) {
                const dialog=root.subTestFind(dialogs,"subtitle-dialog")
                if(!root.subTestCheck(dialog&&dialog.trackOptions.length===veyra.subtitleTracks.length+1&&dialog.trackOptions.slice(1).every(t=>!t.disabled),"actual subtitle dialog exposes usable tracks"))return
                root.contentItem.grabToImage(result=>result.saveToFile(SCREENSHOT))
                console.log("SUBGUI_PASS",mode);subTestTimer.stop()
            }
        }
    }
'''
replacements={'MODE':case,'VOBSUB':str(FIX/'vobsub.idx'),'BROKEN':str(FIX/'broken.sup'),'UNICODE':str(FIX/'中文 路径.srt'),'SUP':str(FIX/'bitmap.sup'),'MEDIA':str(media),'SCREENSHOT':str(LOG/'subtitle-dialog.png')}
for token,value in replacements.items():timer=timer.replace(token,json.dumps(value.replace('\\','/'),ensure_ascii=False))
main=APP/'qml/Veyra/Main.qml';original=main.read_bytes()
assert original.decode('utf8').count('    id: root')==1
swap=main.with_suffix('.swap');swap.write_text(original.decode('utf8').replace('    id: root','    id: root'+timer,1),encoding='utf8');os.replace(swap,main)
command=[str(APP/'veyra_qml_ui.exe'),'--data-dir',str(profile),'--page','pro','--size','1280x800','--exit-after','20000',str(media)]
start=time.monotonic()
try:
    with (LOG/'console.log').open('wb') as stream:run=subprocess.run(command,cwd=APP,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=40)
finally:
    swap=main.with_suffix('.swap');swap.write_bytes(original);os.replace(swap,main)
verify()
text=(LOG/'console.log').read_text(encoding='utf8',errors='replace')
errors=[l for l in text.splitlines() if re.search(r'SUBGUI_FAIL|ReferenceError:|TypeError:|\[ERROR\s*\]|\[FATAL\s*\]|D3D12 ERROR|D3D12 CORRUPTION',l)]
receipt={'case':case,'command':command,'exit':run.returncode,'seconds':time.monotonic()-start,'errors':errors,'passed':'SUBGUI_PASS' in text and not errors and run.returncode==0,'events':[l for l in text.splitlines() if 'SUBGUI_' in l],'exeSha256':sha(APP/'veyra_qml_ui.exe'),'donorUnchanged':True}
(LOG/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps(receipt,ensure_ascii=False,indent=2),flush=True)
raise SystemExit(not receipt['passed'])

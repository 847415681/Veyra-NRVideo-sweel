// Product integration probe, injected only into an isolated test copy of Main.qml.
Connections {
    target: veyra.bilibili
    function onPlaybackReady(url, title) { controlsSmoke.opens++; controlsSmoke.lastReady = Date.now() }
}
Timer {
    id: controlsSmoke
    interval: 500; repeat: true; running: true
    property int phase: 0
    property int ticks: 0
    property int opens: 0
    property int expectedOpens: 0
    property double entered: Date.now()
    property double lastReady: 0
    property double before: 0
    property double pausePosition: 0
    property int firstQuality: 0
    property int otherQuality: 0
    property int otherAudio: 0
    function log(text) { veyra.logUi("bilibili-controls-test", text) }
    function next(p) { phase = p; entered = Date.now(); log("phase=" + p) }
    function fail(text) { log("CONTROLS_FAIL " + text); running = false; veyra.stopPlayback(); root.close() }
    function pass(text) { log("CONTROLS_PASS " + text); next(99) }
    function loaded() { return opens >= expectedOpens && !veyra.bilibili.busy && veyra.running && !veyra.openingSource && Date.now() - lastReady > 2500 }
    function switchQuality(q) { before = veyra.position; expectedOpens = opens + 1; veyra.bilibili.changeQuality(q) }
    onTriggered: {
        ++ticks
        if (ticks > 350) { fail("timeout phase=" + phase); return }
        if (veyra.failed && Date.now() - entered > 10000) { fail("engine " + veyra.statusText); return }
        const age = Date.now() - entered
        const model = veyra.bilibili
        if (phase === 0 && ticks >= 3) {
            veyra.openBilibiliDialog(); model.inspect("@LINK@"); next(1)
        } else if (phase === 1 && !model.busy) {
            if (model.parts.length === 0) { fail("metadata " + model.status); return }
            log("METADATA parts=" + model.parts.length + " qualities=" + JSON.stringify(model.qualities))
            model.play(); next(2)
        } else if (phase === 2 && veyra.running && age > 12000 && veyra.submitFpsKnown && veyra.submitFps > 0) {
            if (veyra.audioTracks.length === 0) { fail("missing audio"); return }
            firstQuality = model.playingQuality
            const alt = model.playbackQualities.find(q => q.id > 0 && q.id !== firstQuality)
            otherQuality = alt ? alt.id : firstQuality
            log("PLAYING " + veyra.sourceWidth + "x" + veyra.sourceHeight + " q=" + firstQuality + " audio=" + JSON.stringify(model.playbackAudioTracks))
            if (@LIVE@) {
                if (!model.timeshiftAvailable || veyra.duration < 120) { fail("timeshift unavailable"); return }
                expectedOpens = opens + 1; model.changeQuality(otherQuality); next(20)
            } else {
                if (!alt) { fail("sample has no alternate quality"); return }
                veyra.playbackRate = 1.25; next(3)
            }
        } else if (phase === 3 && age > 2500) {
            switchQuality(otherQuality); next(4)
        } else if (phase === 4 && loaded() && age > 6000) {
            if (model.playingQuality !== otherQuality || veyra.position < before - 1 || veyra.paused || Math.abs(veyra.playbackRate - 1.25) > 0.01) { fail("playing switch position/rate/state"); return }
            log("QUALITY_PLAYING before=" + before + " after=" + veyra.position + " q=" + model.playingQuality + " rate=" + veyra.playbackRate)
            veyra.togglePlayPause(); next(5)
        } else if (phase === 5 && veyra.paused && age > 1500) {
            pausePosition = veyra.position; switchQuality(firstQuality); next(6)
        } else if (phase === 6 && loaded() && age > 6000) {
            if (!veyra.paused || Math.abs(veyra.position - pausePosition) > 1.5 || Math.abs(veyra.playbackRate - 1.25) > 0.01) { fail("paused switch before=" + pausePosition + " after=" + veyra.position + " paused=" + veyra.paused); return }
            log("QUALITY_PAUSED before=" + pausePosition + " after=" + veyra.position)
            const altAudio = model.playbackAudioTracks.find(a => a.id !== model.playingAudio)
            if (altAudio) {
                otherAudio = altAudio.id; expectedOpens = opens + 1; model.changeAudio(otherAudio); next(7)
            } else { log("AUDIO_SINGLE_TRACK"); next(8) }
        } else if (phase === 7 && loaded() && age > 6000) {
            if (model.playingAudio !== otherAudio || !veyra.paused || Math.abs(veyra.position - pausePosition) > 1.5) { fail("audio switch"); return }
            log("AUDIO_PAUSED id=" + model.playingAudio + " position=" + veyra.position); next(8)
        } else if (phase === 8 && age > 1500) {
            if (Math.abs(veyra.position - pausePosition) > 1.5) { fail("paused position drift"); return }
            veyra.togglePlayPause(); next(9)
        } else if (phase === 9 && veyra.position > pausePosition + 5 && veyra.submitFpsKnown && veyra.submitFps > 0) {
            if (@PGC@) { expectedOpens = opens + 1; model.changePart(1); next(10) }
            else pass("video quality/paused/audio/rate position=" + veyra.position)
        } else if (phase === 10 && loaded()) {
            if (model.playingPart !== 1 || veyra.position > 15 || veyra.paused) { fail("part switch position=" + veyra.position); return }
            next(11)
        } else if (phase === 11 && age > 7000 && veyra.submitFpsKnown && veyra.submitFps > 0) {
            pass("pgc part=" + model.playingPart + " position=" + veyra.position)
        } else if (phase === 20 && loaded() && age > 7000 && veyra.submitFps > 0) {
            log("LIVE_QUALITY actual=" + model.playingQuality + " window=" + veyra.duration)
            expectedOpens = opens + 1; veyra.seekTo(veyra.duration - 120); next(21)
        } else if (phase === 21 && loaded() && age > 12000 && veyra.submitFps > 0) {
            const lag = veyra.duration - veyra.position
            if (!model.replaying || lag < 105 || lag > 145 || veyra.audioTracks.length === 0) { fail("replay lag=" + lag + " replay=" + model.replaying); return }
            log("REPLAY lag=" + lag + " size=" + veyra.sourceWidth + "x" + veyra.sourceHeight + " fps=" + veyra.submitFps)
            expectedOpens = opens + 1; veyra.seekTo(veyra.duration - 300); next(22)
        } else if (phase === 22 && !model.busy && veyra.running && age > 9000 && veyra.submitFps > 0) {
            const lag = veyra.duration - veyra.position
            if (opens < expectedOpens) {
                if (!model.replaying || !model.status.includes("已保留当前播放") || lag < 100 || lag > 160) { fail("missing replay did not preserve previous session"); return }
                log("REPLAY_UNAVAILABLE_KEPT lag=" + lag)
            } else {
                if (lag < 285 || lag > 325) { fail("second seek lag=" + lag); return }
                log("REPLAY_SECOND lag=" + lag)
            }
            expectedOpens = opens + 1; veyra.seekTo(veyra.duration - 90); next(24)
        } else if (phase === 24 && loaded() && age > 9000 && veyra.submitFps > 0) {
            const lag = veyra.duration - veyra.position
            if (lag < 75 || lag > 115) { fail("90 second replay lag=" + lag); return }
            log("REPLAY_90 lag=" + lag)
            expectedOpens = opens + 1; model.returnToLive(); next(23)
        } else if (phase === 23 && loaded() && age > 10000 && veyra.submitFps > 0) {
            if (model.replaying || veyra.duration - veyra.position > 3) { fail("return live"); return }
            pass("live quality/timeshift120/timeshift300-or-preserve/timeshift90/return audio=" + veyra.audioTracks.length)
        } else if (phase === 99) { running = false; veyra.stopPlayback(); root.close() }
    }
}

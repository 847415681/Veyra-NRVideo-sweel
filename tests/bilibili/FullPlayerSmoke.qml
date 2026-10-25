// Inserted only into a disposable copy of Main.qml by full_player_smoke.py.
// Drives the real bridge, network resolver, FFmpeg engine and native presenter.
Timer {
    id: bilibiliSmoke
    interval: 500
    repeat: true
    running: true
    property int phase: 0
    property int ticks: 0
    property int startedAt: 0
    property double playbackStartPosition: -1
    function log(text) { veyra.logUi("bilibili-smoke", text) }
    function fail(text) {
        log("BILIBILI_FULL_FAIL " + text)
        running = false
        veyra.stopPlayback()
        root.close()
    }
    onTriggered: {
        ++ticks
        if (ticks > 200) { fail("timeout phase=" + phase); return }
        if (veyra.failed) { fail("engine " + veyra.statusText); return }
        // Live FLV timestamps may start days into the timeline.
        if (phase === 2 && veyra.running && playbackStartPosition < 0)
            playbackStartPosition = veyra.position
        if (phase === 0 && ticks >= 3) {
            veyra.openBilibiliDialog()
            veyra.bilibili.inspect("@LINK@")
            phase = 1
        } else if (phase === 1 && !veyra.bilibili.busy && veyra.bilibili.parts.length > 0) {
            log("METADATA parts=" + veyra.bilibili.parts.length + " qualities=" + veyra.bilibili.qualities.length)
            veyra.bilibili.play()
            phase = 2
        } else if (phase === 2 && veyra.running && veyra.position - playbackStartPosition > 15 && veyra.submitFpsKnown && veyra.submitFps > 0) {
            if (veyra.sourceWidth <= 0 || veyra.sourceHeight <= 0 || veyra.audioTracks.length === 0) {
                fail("missing video/audio"); return
            }
            log("PLAYING " + veyra.sourceWidth + "x" + veyra.sourceHeight
                + " submitFps=" + veyra.submitFps + " audioTracks=" + veyra.audioTracks.length
                + " duration=" + veyra.duration + " position=" + veyra.position)
            startedAt = ticks
            phase = 3
        } else if (phase === 3 && ticks - startedAt >= 10 && veyra.position - playbackStartPosition > 20 && veyra.submitFps > 0) {
            if (@LIVE@) {
                log("BILIBILI_FULL_PASS live seconds=" + (veyra.position - playbackStartPosition) + " submitFps=" + veyra.submitFps)
                phase = 5
            } else {
                veyra.seekTo(30)
                phase = 4
            }
        } else if (phase === 4 && veyra.position >= 42 && veyra.position < 60 && veyra.submitFpsKnown && veyra.submitFps > 0) {
            log("BILIBILI_FULL_PASS seek position=" + veyra.position + " submitFps=" + veyra.submitFps)
            phase = 5
        } else if (phase === 5) {
            veyra.stopPlayback()
            phase = 6
        } else if (phase === 6) {
            running = false
            root.close()
        }
    }
}

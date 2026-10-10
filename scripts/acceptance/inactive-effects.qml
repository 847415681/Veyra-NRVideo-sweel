import QtQuick
Item {
    id: test
    property string phase: "edit"
    property int step: 0
    property int ticks: 0
    property int nrIndex: -1
    property real beginPosition: 0
    function require(ok, message) {
        if (!ok) throw new Error(message)
        console.log("INACTIVE_UI_CHECK", message)
    }
    function closed() {
        require(!veyra.nrEnabled && !veyra.srEnabled && !veyra.fgEnabled && !veyra.colorEnabled && !veyra.videoHdr, "effects stay disabled")
    }
    function values() {
        const nr = veyra.nrLayers[0]
        require(nr.style === 2 && nr.sizePolicy === 3 && nr.temporal, "disabled NR values retained")
        require(veyra.videoSrQuality === 4 && veyra.srTargetIndex === 1, "disabled SR values retained")
        require(veyra.vfgQuality === 2 && veyra.videoHdrParams.peakNits === 800, "disabled VFG and Video HDR values retained")
        require(Math.abs(veyra.colorExposure - .4) < .001, "disabled colour value retained")
        require(veyra.nrHoldStrength === 0 && Math.abs(veyra.nrHoldTolerance - .12) < .001, "disabled stabiliser tolerance retained")
    }
    Timer {
        interval: 500; repeat: true; running: true
        onTriggered: { try {
            if (++test.ticks > 100) throw new Error("deadline step=" + test.step)
            if (test.step === 0) {
                if (test.ticks < 4) return
                test.closed()
                if (test.phase === "invalid") {
                    test.require(veyra.nrHoldStrength === 0 && Math.abs(veyra.nrHoldTolerance - .02) < .001, "invalid stabiliser preferences keep defaults")
                    console.log("INACTIVE_UI_PASS invalid"); Qt.quit(); return
                }
                test.require(veyra.nrLayers.length > 0, "real list NR instance exists")
                test.nrIndex = veyra.nrLayers[0].index
                if (test.phase === "restore") { test.values(); test.require(!veyra.running, "restoring private settings does not auto-play"); console.log("INACTIVE_UI_PASS restore"); Qt.quit(); return }
                test.require(veyra.nrHoldStrength === 0 && Math.abs(veyra.nrHoldTolerance - .02) < .001, "old profile keeps stabiliser disabled by default")
                veyra.volume = 0
                veyra.openPath("E:/Ai/知识/小七姐/GTAVI_An_Extended_Look_4K_Native.mp4")
                test.step = 1; return
            }
            test.require(!veyra.failed, "real player has no error")
            if (test.step === 1) {
                if (!veyra.running || veyra.position < 1 || veyra.applying) return
                test.beginPosition = veyra.position
                veyra.logUi("inactive-effects-test", "BEGIN inactive UI edits")
                test.step = 2; return
            }
            test.closed()
            if (test.step === 2) test.require(veyra.setNrLayerParameter(test.nrIndex, "style", 2), "edit disabled NR style")
            else if (test.step === 3) test.require(veyra.setNrLayerParameter(test.nrIndex, "sizePolicy", 3), "edit disabled NR resolution")
            else if (test.step === 4) test.require(veyra.setNrLayerParameter(test.nrIndex, "temporal", 1), "edit disabled NR history option")
            else if (test.step === 5) veyra.videoSrQuality = 4
            else if (test.step === 6) veyra.srTargetIndex = 1
            else if (test.step === 7) veyra.vfgQuality = 2
            else if (test.step === 8) test.require(veyra.setVideoHdrParameter("peakNits", 800), "edit disabled Video HDR")
            else if (test.step === 9) veyra.colorExposure = .4
            else if (test.step === 10) veyra.nrHoldTolerance = .12
            else {
                if (veyra.applying) return
                test.values()
                test.require(veyra.position > test.beginPosition + 3, "actual media time advances throughout GUI edits")
                veyra.logUi("inactive-effects-test", "END inactive UI edits")
                console.log("INACTIVE_UI_PASS edit")
                Qt.quit(); return
            }
            ++test.step
        } catch(e) { console.log("INACTIVE_UI_FAIL", String(e)); Qt.quit() } }
    }
}

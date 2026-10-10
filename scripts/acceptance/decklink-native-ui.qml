import QtQuick
Item {
    id: test
    property var dialogHost: null
    property var appWindow: Window.window
    property string evidence: ""
    property string phase: "fresh"
    property int step: 0
    property int ticks: 0
    property bool grabbed: false
    property var before: []
    function require(ok, why) {
        if (!ok) throw new Error(why)
        console.log("NATIVE_UI_CHECK", why)
    }
    function find(item, name) {
        if (item.objectName === name) return item
        for (const child of item.children || []) {
            const hit = find(child, name)
            if (hit) return hit
        }
        return null
    }
    function reveal(row) {
        let parent = row.parent
        while (parent) {
            if (parent.contentItem !== undefined && parent.contentY !== undefined && parent.contentHeight > parent.height) {
                const y = row.mapToItem(parent.contentItem, 0, 0).y
                parent.contentY = Math.min(parent.contentHeight - parent.height, Math.max(0, y - 140))
                return
            }
            parent = parent.parent
        }
    }
    Timer {
        interval: 250; running: true; repeat: true
        onTriggered: { try {
            ++test.ticks
            if (test.ticks > 110) throw new Error("deadline step=" + test.step)
            if (test.step === 0) {
                if (test.ticks < 6 || !test.dialogHost) return
                test.require(!veyra.running && !veyra.nrEnabled && !veyra.fgEnabled, "no source or enhancement started")
                test.dialogHost.open("capture"); test.step = 1; return
            }
            if (veyra.captureQueryBusy) return
            const row = test.find(test.appWindow.contentItem, "capture-blackmagic-native-row")
            const toggle = test.find(test.appWindow.contentItem, "capture-blackmagic-native")
            const magewell = test.find(test.appWindow.contentItem, "capture-magewell-row")
            if (!row || !toggle || !magewell) throw new Error("production switches missing")
            if (test.step === 1) {
                test.require(row.parent === magewell.parent && row.y < magewell.y, "native switch is beside Magewell in the capture panel")
                if (test.phase === "restore") {
                    test.require(veyra.preferences.blackmagicNativeCapture === true && toggle.checked && toggle.enabled, "enabled preference restored and can be switched off without a BMD driver")
                    toggle.toggled(false); test.step = 3; return
                }
                test.require(veyra.preferences.blackmagicNativeCapture !== true && !toggle.checked, "fresh profile defaults off")
                test.require(!veyra.captureDevices.some(x => x.id.startsWith("decklink-native:")), "default-off list contains only DirectShow endpoints")
                const usb = veyra.captureDevices.find(x => x.label === "KUHAIMI 27P")
                test.require(usb !== undefined, "real local USB regression fixture is present")
                if (veyra.captureDeviceId !== usb.id) { veyra.captureDeviceId = usb.id; return }
                test.require(!veyra.captureBlackmagicDevice && !toggle.enabled, "ordinary USB cannot enable the BMD-only switch")
                test.require(row.label === (test.phase === "english" ? "Blackmagic native capture" : "Blackmagic 原生采集"), "switch label uses the selected language")
                test.before = veyra.captureDevices.map(x => x.id)
                // Exercise the actual saved-preference API; this PC has no BMD
                // endpoint on which a user could enable the disabled switch.
                veyra.setPreference("blackmagicNativeCapture", true)
                test.step = 2; return
            }
            if (test.step === 2) {
                test.require(toggle.checked && toggle.enabled, "saved setting updates the production switch")
                const ids = veyra.captureDevices.map(x => x.id)
                test.require(test.before.every(x => ids.includes(x)), "enabling native mode retains ordinary USB and virtual devices")
                test.require(!veyra.captureDeviceId.length && veyra.captureStatus.length > 0, "missing native driver/device produces an explicit status without guessing another card")
                test.require(!veyra.running, "switching the setting does not start a capture")
                test.reveal(row); test.step = 4; return
            }
            if (test.step === 3) {
                test.require(veyra.preferences.blackmagicNativeCapture === false && !toggle.checked, "production toggle handler saves off")
                test.require(!veyra.captureDevices.some(x => x.id.startsWith("decklink-native:")), "off restores the DirectShow device list")
                test.require(!veyra.running, "switching off leaves idle playback idle")
                test.reveal(row); test.step = 4; return
            }
            if (test.step === 4) {
                test.grabbed = false
                test.appWindow.contentItem.grabToImage(result => {
                    test.grabbed = result.saveToFile(test.evidence + "/capture-switch.png")
                })
                test.step = 5; return
            }
            if (test.step === 5 && test.grabbed) {
                console.log("NATIVE_UI_PASS", test.phase)
                Qt.quit()
            }
        } catch(e) { console.log("NATIVE_UI_FAIL", String(e)); Qt.quit() } }
    }
}

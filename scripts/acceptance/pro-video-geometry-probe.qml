// Test-only driver loaded into a private copy of Main.qml by the companion script.
import QtQuick
Item {
    id: probe
    property var appWindow: Window.window
    property var cases: []
    property int index: -1
    property int ticks: 0
    property bool recorded: false
    property string media: ""
    function find(item, name) {
        if (!item) return null
        if (item.objectName === name) return item
        for (const child of (item.children || [])) {
            const hit = find(child, name)
            if (hit) return hit
        }
        return null
    }
    function rect(item) {
        const p = item.mapToItem(appWindow.contentItem, 0, 0)
        return { x: p.x, y: p.y, w: item.width, h: item.height }
    }
    function apply(c) {
        if (appWindow.fullscreen) appWindow.toggleFullscreen()
        appWindow.page = "pro"
        appWindow.width = c.width
        appWindow.height = c.height
        veyra.setPreference("language", c.language || "zh-CN")
        veyra.setPreference("listInspectorWidth", c.inspector || 376)
        veyra.contentRate = c.options ? 1 : 0
        veyra.displaySync = c.options ? 1 : 0
        veyra.outputCustomFps = 30
        veyra.outputRateMode = c.options ? 2 : 0
        veyra.playbackRate = c.options ? 0.5 : 1
        if (c.page === "min") appWindow.page = "min"
        if (c.fullscreen) appWindow.toggleFullscreen()
    }
    function record(c) {
        const content = appWindow.contentItem
        const host = find(content, "videoHost")
        const area = find(content, "videoArea")
        const bar = find(content, "pro-transport")
        const head = find(content, "pro-header")
        const options = find(content, "run-active-options")
        if (!host || !veyra.hasSource || veyra.failed) throw new Error("source/host unavailable")
        const row = { name: c.name, index: index, page: appWindow.page, qtDpr: Screen.devicePixelRatio,
            fullscreen: appWindow.fullscreen, window: {w: appWindow.width,h: appWindow.height},
            host: rect(host), options: veyra.runOptions, status: veyra.runStatus,
            submitFps: veyra.submitFps, applying: veyra.applying }
        if (appWindow.page === "pro" && !appWindow.fullscreen) {
            if (!area || !bar || !head) throw new Error("professional layout missing")
            row.area = rect(area); row.bar = rect(bar); row.header = rect(head)
            if (options) row.optionText = {rect: rect(options),text: options.text}
        }
        console.log("PRO_GEOMETRY " + JSON.stringify(row))
    }
    Timer {
        interval: 100; running: true; repeat: true
        onTriggered: {
            try {
                ++probe.ticks
                if (probe.index < 0) {
                    if (probe.ticks < 3) return
                    veyra.setPreference("reducedMotion", true)
                    veyra.nodeMode = 0; veyra.muted = true
                    veyra.nrEnabled = false; veyra.srEnabled = false
                    veyra.fgEnabled = false; veyra.videoHdr = false; veyra.colorEnabled = false
                    probe.index = 0; probe.apply(probe.cases[0])
                    // Open on the same callback as the page/size change, before a render.
                    veyra.openPath(probe.media)
                    probe.ticks = 0; return
                }
                if (probe.ticks > 180) throw new Error("timeout at " + probe.index)
                if (!probe.recorded) {
                    if (probe.ticks < 12 || !veyra.running || veyra.applying) return
                    probe.record(probe.cases[probe.index]); probe.recorded = true; probe.ticks = 0
                } else if (probe.ticks >= 5) {
                    ++probe.index
                    if (probe.index >= probe.cases.length) {
                        console.log("PRO_GEOMETRY_DONE")
                        veyra.stopPlayback(); Qt.quit(); return
                    }
                    probe.apply(probe.cases[probe.index]); probe.recorded = false; probe.ticks = 0
                }
            } catch(e) {
                console.error("PRO_GEOMETRY_FAIL " + e.message); Qt.exit(3)
            }
        }
    }
}

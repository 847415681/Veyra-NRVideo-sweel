import QtQuick
Item {
    id: test
    property var appRoot
    property var dialogHost
    property var nodePage
    property var proPage
    property string evidence
    property int step: 0
    property int ticks: 0
    property var menu
    property bool saved: false
    property int settleTick: 0
    Item { id: popupAnchor; parent: test.appRoot ? test.appRoot.contentItem : null; x: parent ? parent.width - 12 : 0; y: parent ? parent.height - 12 : 0; width: 1; height: 1 }
    function require(ok, why) {
        if (!ok) throw new Error(why)
        console.log("LAYOUT_UI_CHECK", why)
    }
    function walk(item, output, seen) {
        if (!item || seen.indexOf(item) >= 0) return output
        seen.push(item); output.push(item)
        for (const child of item.children || []) walk(child, output, seen)
        for (const child of item.data || []) walk(child, output, seen)
        return output
    }
    function find(root, name) { return walk(root, [], []).find(x => x.objectName === name) }
    function checkMenu() {
        require(menu && menu.opened, "production menu opened")
        require(menu.width <= appRoot.width - 16 && menu.x >= 7 && menu.x + menu.width <= appRoot.width - 7, "menu stays inside window width")
        require(menu.height <= appRoot.height - 16 && menu.y >= 7 && menu.y + menu.height <= appRoot.height - 7, "menu stays inside window height")
        const labels = walk(menu.contentItem, [], []).filter(x => x.objectName === "vmenu-option-label")
        require(labels.length > 0, "production choices contain text")
        require(labels.every(x => !x.truncated && x.contentWidth <= x.width + 1 && x.contentHeight <= x.height + 1), "complete choice labels fit and wrap")
        console.log("LAYOUT_UI_GEOMETRY", JSON.stringify({width:menu.width,height:menu.height,x:menu.x,y:menu.y,labels:labels.map(x => x.text)}))
    }
    function grab(name) {
        saved = false
        appRoot.contentItem.grabToImage(result => { saved = result.saveToFile(evidence + "/" + name + ".png") })
    }
    Timer {
        interval: 250; running: true; repeat: true
        onTriggered: { try {
            ++test.ticks
            if (test.ticks > 150) throw new Error("deadline step=" + test.step)
            if (test.step === 0) {
                if (test.ticks < 6 || !test.dialogHost) return
                test.require(!veyra.running, "layout check uses a private idle profile")
                test.dialogHost.open("capture"); test.step = 1; return
            }
            if (test.step === 1) {
                if (veyra.captureQueryBusy) return
                const device = veyra.captureDevices.find(x => x.label === "KUHAIMI 27P")
                if (device && veyra.captureDeviceId !== device.id) { veyra.captureDeviceId = device.id; return }
                const select = test.find(test.appRoot.contentItem, "capture-format")
                test.require(select && select.options.length > 0, "real capture format list available")
                test.menu = test.walk(select, [], []).find(x => typeof x.openAt === "function")
                test.menu.openAt(select, "down"); test.step = 2; return
            }
            if (test.step === 2) { if (!test.menu.opened) return; test.checkMenu(); test.grab("capture-format"); test.step = 3; return }
            if (test.step === 3) {
                if (!test.saved) return
                test.menu.close(); test.dialogHost.close(); test.appRoot.page = "node"; test.step = 4; return
            }
            if (test.step === 4) {
                if (veyra.nodeMode !== 1) return
                if (!veyra.chain.length) { test.require(veyra.addEffect("nr") >= 0, "add a node through the real editor API"); return }
                test.menu = test.find(test.nodePage, "node-context-menu")
                const node = veyra.chain.find(x => x.type === "nr") || veyra.chain[0]
                test.require(test.menu && node, "real node context menu and node available")
                test.menu.targetId = node.id
                test.menu.openAt(popupAnchor, "at"); test.step = 5; return
            }
            if (test.step === 5) {
                if (!test.menu.opened) return
                test.checkMenu()
                const transport = test.find(test.nodePage, "node-transport-layout")
                test.require(transport && transport.childrenRect.width <= transport.width + 1, "node transport controls stay within the bar")
                const tip = test.find(test.nodePage, "node-canvas-tip")
                test.require(tip && !tip.truncated && tip.contentWidth <= tip.width + 1, "complete node instructions wrap within the canvas")
                test.grab("node-context"); test.step = 6; return
            }
            if (test.step === 6) {
                if (!test.saved) return
                test.menu.close(); veyra.nodeMode = 0; test.appRoot.page = "pro"; test.settleTick = test.ticks; test.step = 7; return
            }
            if (test.step === 7) {
                if (test.ticks < test.settleTick + 6 || test.appRoot.shownPage !== "pro" || test.appRoot.leavingPage !== "") return
                test.require(test.proPage.current && test.proPage.visible && test.proPage.opacity === 1, "production inspector transition has completed")
                const rows = test.walk(test.proPage, [], []).filter(x => x.stacked !== undefined && x.implicitHeight !== undefined && x.width > 0)
                test.require(rows.length > 10, "production inspector rows present")
                console.log("LAYOUT_UI_ROWS", JSON.stringify(rows.filter(x => x.childrenRect.width > x.width + 1).map(x => ({label:x.label,width:x.width,children:x.childrenRect.width}))))
                test.require(rows.every(x => x.childrenRect.width <= x.width + 1), "inspector rows stay within their panels")
                test.grab("inspector"); test.step = 8; return
            }
            if (test.step === 8 && test.saved) { console.log("LAYOUT_UI_PASS"); Qt.quit() }
        } catch(e) { console.log("LAYOUT_UI_FAIL", String(e)); Qt.quit() } }
    }
}

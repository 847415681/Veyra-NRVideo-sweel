import QtQuick
import QtTest
import Veyra

Item {
    width: 620; height: 250
    UpdateBanner { id: banner; anchors.centerIn: parent }
    SignalSpy { id: opened; target: banner; signalName: "openRequested" }
    TestCase {
        name: "UpdateBanner"
        when: windowShown
        function cleanup() { banner.dismiss(); banner.width = 500; opened.clear() }
        function test_defaultSilent() { compare(banner.visible, false) }
        function test_click() {
            banner.show("2.0.8")
            compare(banner.visible, true)
            mouseClick(banner, 30, banner.height / 2)
            compare(opened.count, 1)
            compare(banner.visible, false)
        }
        function test_close() {
            banner.show("2.0.8")
            const close = findChild(banner, "update-dismiss")
            mouseClick(close, close.width / 2, close.height / 2)
            compare(opened.count, 0)
            compare(banner.visible, false)
        }
        function test_timeoutAndNarrow() {
            banner.width = 230
            banner.show("2.0.100")
            verify(banner.height >= 54)
            wait(7500)
            compare(banner.visible, true)
            tryCompare(banner, "visible", false, 1200)
            compare(opened.count, 0)
        }
    }
}

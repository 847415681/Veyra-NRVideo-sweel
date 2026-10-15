import QtQuick
import QtQuick.Controls

Rectangle {
    id: banner
    // The native video host subtracts visible videoCover items from its region.
    objectName: "videoCover"
    property real coverRadius: 10
    property string version: ""
    property bool shown: false
    signal openRequested()
    width: 500
    height: Math.max(54, message.implicitHeight + 24)
    radius: 10
    color: "#17251E"
    border.color: "#426C51"
    visible: shown
    function show(value) { version = value; shown = true; dismissTimer.restart() }
    function dismiss() { shown = false; dismissTimer.stop() }
    // One-shot only: idle UI and playback render rates are unchanged.
    Timer { id: dismissTimer; interval: 8000; onTriggered: banner.shown = false }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: { banner.openRequested(); banner.dismiss() }
    }
    Text {
        id: message
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.right: closeButton.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: qsTr("发现新版本 %1，点击查看更新").arg(banner.version)
        color: "#D5F3DE"
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsBody
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
    }
    VButton {
        id: closeButton
        objectName: "update-dismiss"
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        icon: true
        iconName: "x"
        ghost: true
        tip: qsTr("关闭")
        onClicked: banner.dismiss()
    }
}

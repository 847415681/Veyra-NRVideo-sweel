import QtQuick
import QtQuick.Layouts

RowLayout {
    id: root
    required property var model
    property real menuBottomLimit: -1
    readonly property bool menuOpen: partsMenu.visible || qualityMenu.visible
    visible: model.active
    spacing: 2

    VButton {
        objectName: "bilibili-playing-parts"
        text: qsTr("选集")
        ghost: true
        implicitHeight: 30
        visible: !root.model.live
        enabled: !root.model.busy && root.model.playbackParts.length > 1
        tip: qsTr("切换分 P 或剧集")
        onClicked: partsMenu.openAt(this, "up")
    }
    VButton {
        objectName: "bilibili-playing-quality"
        text: root.model.playingQualityLabel || qsTr("清晰度")
        maxTextWidth: 88
        ghost: true
        implicitHeight: 30
        enabled: !root.model.busy && root.model.playbackQualities.length > 1
        tip: qsTr("清晰度 · %1").arg(root.model.playingQualityLabel)
        onClicked: qualityMenu.openAt(this, "up")
    }
    VButton {
        objectName: "bilibili-return-live"
        text: qsTr("回到直播")
        ghost: true
        implicitHeight: 30
        visible: root.model.replaying
        enabled: !root.model.busy
        onClicked: root.model.returnToLive()
    }
    VMenu {
        id: partsMenu
        objectName: "bilibili-playing-parts-menu"
        aboveLimit: root.menuBottomLimit
        title: qsTr("选集")
        items: root.model.playbackParts.map((p, i) => ({ label: p.label, part: i, checked: i === root.model.playingPart, disabled: root.model.busy }))
        onPicked: (i, o) => root.model.changePart(o.part)
    }
    VMenu {
        id: qualityMenu
        objectName: "bilibili-playing-quality-menu"
        aboveLimit: root.menuBottomLimit
        title: qsTr("清晰度")
        items: root.model.playbackQualities.map(q => ({ label: q.label, quality: q.id, checked: q.id === root.model.playingQuality, disabled: root.model.busy }))
        onPicked: (i, o) => root.model.changeQuality(o.quality)
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var model
    signal playRequested()
    spacing: 12
    RowLayout {
        Layout.fillWidth: true
        Basic.TextField {
            id: addressField
            objectName: "bilibili-address"
            Layout.fillWidth: true
            placeholderText: qsTr("粘贴 B 站链接或 BV / AV 号")
            color: Theme.t1
            placeholderTextColor: Theme.t3
            font.family: Theme.fontUi
            font.pixelSize: 13
            background: Rectangle { color: Theme.card; radius: 8; border.color: Theme.stroke2 }
            text: root.model.address
            enabled: !root.model.busy
            onAccepted: root.model.inspect(text)
        }
        VButton {
            objectName: "bilibili-inspect"
            text: root.model.busy ? qsTr("取消解析") : qsTr("解析")
            onClicked: root.model.busy ? root.model.cancel() : root.model.inspect(addressField.text)
        }
    }
    Text {
        Layout.fillWidth: true
        text: qsTr("支持普通视频、多 P、番剧和直播。会员及地区限制以当前账号权限为准。")
        color: Theme.t3; font.pixelSize: 12; wrapMode: Text.Wrap
    }
    Text {
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.model.title
        color: Theme.t1; font.pixelSize: 15; font.bold: true; wrapMode: Text.Wrap
    }
    VSelect {
        id: parts
        objectName: "bilibili-parts"
        Layout.fillWidth: true
        visible: root.model.parts.length > 0
        enabled: !root.model.busy
        title: qsTr("选集")
        options: root.model.parts.map((p, i) => ({ id: String(i), label: p.label }))
        value: options.length > root.model.selectedPart ? options[root.model.selectedPart].label : ""
        onPicked: id => root.model.selectPart(Number(id))
    }
    RowLayout {
        visible: root.model.parts.length > 0
        Layout.fillWidth: true
        VSelect {
            id: quality
            objectName: "bilibili-quality"
            Layout.fillWidth: true
            title: qsTr("清晰度")
            property int selectedQuality: 0
            options: root.model.qualities.map(q => ({ id: String(q.id), label: q.label }))
            value: {
                const choice = options.find(q => Number(q.id) === selectedQuality)
                return choice ? choice.label : options.length ? options[0].label : ""
            }
            onOptionsChanged: if (!options.some(q => Number(q.id) === selectedQuality)) selectedQuality = options.length ? Number(options[0].id) : 0
            onPicked: id => selectedQuality = Number(id)
            enabled: !root.model.busy
        }
        VButton {
            objectName: "bilibili-play"
            text: qsTr("播放")
            iconName: "play"
            primary: true
            enabled: !root.model.busy && root.model.parts.length > 0
            onClicked: { root.model.play(quality.selectedQuality); root.playRequested() }
        }
    }
    Text {
        objectName: "bilibili-status"
        Layout.fillWidth: true
        text: root.model.status
        color: Theme.t2; font.pixelSize: 12; wrapMode: Text.Wrap
    }
    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.stroke }
    RowLayout {
        Layout.fillWidth: true
        Text {
            Layout.fillWidth: true
            text: root.model.loginStatus
            color: Theme.t2; font.pixelSize: 12; wrapMode: Text.Wrap
        }
        VButton {
            objectName: "bilibili-login"
            text: root.model.account.length ? qsTr("退出登录") : (root.model.qrImage.length ? qsTr("刷新二维码") : qsTr("扫码登录"))
            onClicked: root.model.account.length ? root.model.logout() : root.model.beginLogin()
        }
    }
    Image {
        objectName: "bilibili-login-qr"
        Layout.alignment: Qt.AlignHCenter
        Layout.preferredWidth: 208
        Layout.preferredHeight: 208
        visible: root.model.qrImage.length > 0
        source: root.model.qrImage
        fillMode: Image.PreserveAspectFit
        smooth: false
        cache: false
    }
}

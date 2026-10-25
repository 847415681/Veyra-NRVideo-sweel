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
    Basic.ComboBox {
        id: parts
        objectName: "bilibili-parts"
        Layout.fillWidth: true
        visible: root.model.parts.length > 0
        enabled: !root.model.busy
        model: root.model.parts
        textRole: "label"
        palette.button: Theme.card
        palette.buttonText: Theme.t1
        palette.text: Theme.t1
        palette.base: Theme.card
        font.family: Theme.fontUi
        currentIndex: root.model.selectedPart
        onActivated: index => root.model.selectPart(index)
    }
    RowLayout {
        visible: root.model.parts.length > 0
        Layout.fillWidth: true
        Basic.ComboBox {
            id: quality
            objectName: "bilibili-quality"
            Layout.fillWidth: true
            model: root.model.qualities
            textRole: "label"
            valueRole: "id"
            palette.button: Theme.card
            palette.buttonText: Theme.t1
            palette.text: Theme.t1
            palette.base: Theme.card
            font.family: Theme.fontUi
            enabled: !root.model.busy
        }
        VButton {
            objectName: "bilibili-play"
            text: qsTr("播放")
            iconName: "play"
            primary: true
            enabled: !root.model.busy && root.model.parts.length > 0
            onClicked: { root.model.play(quality.currentValue || 0); root.playRequested() }
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

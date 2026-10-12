import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import Veyra

Item {
    id: root
    width: 900; height: 600
    Item { id: anchor; x: root.width - 24; y: root.height - 24; width: 1; height: 1 }
    VMenu { id: menu; title: "Long context menu" }
    VSelect {
        id: warningSelect; x: 12; y: 220; width: 250
        value: "MJPEG（不建议使用）"
        options: [{id:"nv12",label:"NV12"},{id:"jpeg",label:"MJPEG（不建议使用）",danger:true}]
    }
    VRow {
        id: row; x: 12; y: 12; width: 285
        label: "The capture device's actual supported pixel format"
        hint: "This information must stay readable even in a narrow panel."
        VSelect { implicitWidth: 220; value: "3840 × 2160 @ 59.94 fps · P010 · HDR · Native · Low latency" }
    }
    function labels(item, output) {
        if (item.objectName === "vmenu-option-label") output.push(item)
        for (const child of item.children || []) labels(child, output)
        return output
    }
    TestCase {
        name: "MenuAndRowLayout"
        when: windowShown
        function cleanup() { menu.close() }
        function test_mjpeg_warning() {
            compare(warningSelect.valueColor,Theme.err)
            warningSelect.value="NV12"
            compare(warningSelect.valueColor,Theme.t1)
            warningSelect.value="MJPEG（不建议使用）"
            menu.items=warningSelect.options
            menu.openAt(warningSelect,"at");tryCompare(menu,"opened",true)
            const items=labels(menu.contentItem,[])
            compare(items.length,2)
            compare(items[0].color,Theme.t1)
            compare(items[1].color,Theme.err)
            verify(!items[1].truncated)
        }
        function test_text_data() {
            return [{tag:"English",label:"Duplicate the selected enhancement node and reconnect its downstream output",width:900},
                    {tag:"Chinese",label:"3840 × 2160 @ 59.94 fps · P010 · 原生格式 · HDR · 低延迟 · 驱动默认输入",width:900},
                    {tag:"Japanese",label:"選択した画像補正ノードを複製して出力に再接続する",width:420},
                    {tag:"Narrow",label:"C:/A_very_long_folder_name/"+"unbroken".repeat(60),width:300}]
        }
        function test_text(data) {
            root.Window.window.width=data.width; root.Window.window.height=500
            root.width=data.width; root.height=500
            menu.items=[{label:data.label},{label:"Second choice"}]
            menu.openAt(anchor,"at")
            tryCompare(menu,"opened",true)
            wait(500)
            verify(menu.width<=data.width-16)
            verify(menu.x>=8 && menu.x+menu.width<=data.width-7, "x="+menu.x+" width="+menu.width+" window="+data.width)
            verify(menu.y>=8 && menu.y+menu.height<=500-7)
            const items=labels(menu.contentItem,[])
            compare(items.length,2)
            verify(!items[0].truncated)
            verify(items[0].contentWidth<=items[0].width+1)
            verify(items[0].contentHeight<=items[0].height+1)
            if(data.width===900) verify(menu.width>220)
        }
        function test_tall_menu_scrolls() {
            root.Window.window.width=600;root.Window.window.height=400;root.width=600;root.height=400
            let entries=[];for(let i=0;i<60;i++)entries.push({label:"Capture format "+i+" · 3840 × 2160 @ 59.94 fps · P010"})
            menu.items=entries;menu.openAt(anchor,"at");tryCompare(menu,"opened",true);wait(500)
            verify(menu.height<=384)
            const items=labels(menu.contentItem,[]);compare(items.length,60)
            verify(!items[59].truncated)
        }
        function test_narrow_row_reflows() {
            tryCompare(row,"stacked",true)
            verify(row.implicitHeight>60)
            verify(row.childrenRect.width<=row.width+1)
        }
    }
}

// A Lucide icon from IconData.js, drawn like the design's svg.i: 24x24 viewBox scaled
// to `size` (16 by default), no fill, stroke 1.7 in the current colour, round caps and
// joins. The stroke width is in viewBox units, as in the SVG, so it scales with size.
import QtQuick
import QtQuick.Window
import QtQuick.Shapes
import "IconData.js" as IconData

Item {
    id: icon
    property string name
    property color color: Theme.t1
    property real size: 16
    property real strokeWidth: 1.7
    // .play svg: fill:currentColor; stroke:none (the filled play/pause glyphs).
    property bool filled: false
    readonly property bool known: IconData.paths[name] !== undefined
    readonly property bool softwareRendering: (typeof vySoftwareUi !== "undefined" && vySoftwareUi)
        || GraphicsInfo.api === GraphicsInfo.Software
    // Bumped when the interface's graphics device was lost and recreated (GPU reset): Qt
    // 6.8.3's curve renderer then leaves the new nodes empty until the path changes.
    readonly property int deviceGeneration: (typeof veyra !== "undefined" && veyra) ? (veyra.uiDeviceGeneration || 0) : 0

    implicitWidth: size
    implicitHeight: size

    function softwareSvg() {
        const paint = "rgb(" + Math.round(color.r * 255) + "," + Math.round(color.g * 255) + "," + Math.round(color.b * 255) + ")"
        const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"'
            + ' fill="' + (filled ? paint : "none") + '" stroke="' + (filled ? "none" : paint)
            + '" opacity="' + color.a + '" stroke-width="' + strokeWidth + '" stroke-linecap="round" stroke-linejoin="round">'
            + '<path d="' + IconData.paths[name] + '"/></svg>'
        return "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg)
    }

    Image {
        objectName: "software-icon-image"
        width: icon.size; height: icon.size
        visible: icon.known && icon.softwareRendering
        source: visible ? icon.softwareSvg() : ""
        sourceSize.width: Math.max(1, Math.ceil(icon.size * Screen.devicePixelRatio))
        sourceSize.height: Math.max(1, Math.ceil(icon.size * Screen.devicePixelRatio))
        cache: true
        smooth: true
    }

    Shape {
        // Qt 6.8.3's software Shape render node still paints when its clip is
        // empty. Rasterize the same SVG path once: the normal image node then
        // respects ancestor clips and damage regions, including while scrolling.
        // The GPU path stays vector based. No timer or full-window repaint.
        width: 24
        height: 24
        scale: icon.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        visible: icon.known && !icon.softwareRendering
        ShapePath {
            fillColor: icon.filled ? icon.color : "transparent"
            strokeColor: icon.filled ? "transparent" : icon.color
            strokeWidth: icon.strokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: icon.known ? IconData.paths[icon.name] + (icon.deviceGeneration % 2 ? " " : "") : "" }
        }
    }
}

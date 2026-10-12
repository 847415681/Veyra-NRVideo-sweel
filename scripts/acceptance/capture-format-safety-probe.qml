import QtQuick
Item {
    id: probe
    property var appWindow: Window.window
    property var dialogHost
    property string mode: "fresh"
    property string outputDir: ""
    property var expected: ({})
    property int ticks: 0
    property int state: 0
    property var messages: []
    function require(ok,why) { if(!ok) throw new Error(why) }
    function find(item,name) {
        if(!item) return null
        if(item.objectName===name) return item
        for(const child of item.children||[]) {const hit=find(child,name);if(hit)return hit}
        return null
    }
    function field(name) {const f=find(appWindow.contentItem,name);require(!!f,name);return f}
    function pass() {console.log("CAPTURE_SAFETY_PASS",mode,JSON.stringify({resolution:veyra.captureResolutionId,rate:veyra.captureFrameRateId,format:veyra.captureFormatKey}));Qt.quit()}
    Connections {target:veyra;function onNotice(message,error){if(error)probe.messages.push(String(message))}}
    Timer {
        interval:400;running:true;repeat:true
        onTriggered:{try{
            require(++ticks<100,"deadline")
            if(ticks<3)return
            if(state===0){veyra.setPreference("language","zh-CN");dialogHost.open("capture");state=1;return}
            if(veyra.captureQueryBusy||ticks<6)return
            const all=veyra.captureFormats.slice()
            require(all.length>0,"physical device formats present")
            if(mode==="draft-restore"){
                require(veyra.captureResolutionId.length>0&&veyra.captureFrameRateId.length>0,"partial draft axes restored")
                require(veyra.captureFormatKey.length===0&&veyra.capturePixelId.length===0,"partial draft does not invent a pixel format")
                require(!veyra.startCaptureSession()&&!veyra.running,"partial draft cannot open another format")
                pass();return
            }
            require(veyra.captureResolutionId.length===0&&veyra.captureFrameRateId.length===0,"new device has no implicit resolution/rate")
            require(!field("capture-frame-rate").enabled&&!field("capture-format").enabled,"dependent selectors disabled before resolution")
            require(veyra.capturePixelOptions.length===0,"no pixels offered before resolution")
            veyra.capturePixelId=all[0].id
            require(messages.some(x=>x.indexOf("请先选择分辨率")>=0),"premature pixel reports resolution requirement")
            const reached={}
            for(const resolution of veyra.captureResolutionOptions.slice()){
                field("capture-resolution").picked(resolution.id)
                require(veyra.captureFormatKey.length===0,"resolution change requires explicit pixel confirmation")
                for(const rate of veyra.captureFrameRateOptions.slice()){
                    field("capture-frame-rate").picked(rate.id)
                    for(const pixel of veyra.capturePixelOptions.slice()){
                        require(all.some(f=>f.id===pixel.id),"pixel is an actual advertised media identity")
                        reached[pixel.id]=true
                        const format=all.find(f=>f.id===pixel.id)
                        if(format.pixel.toUpperCase()==="MJPEG"||format.pixel.toUpperCase()==="MJPG"){
                            require(pixel.danger&&pixel.label.indexOf("（不建议使用）")>=0,"MJPEG warning metadata")
                            field("capture-format").picked(pixel.id)
                            require(String(field("capture-format").valueColor)==="#ff5d5d","selected MJPEG remains red")
                        }
                    }
                }
            }
            require(Object.keys(reached).length===all.length,"all advertised formats remain reachable")
            const desired=all.find(f=>f.width===1920&&f.height===1080&&Math.abs(f.fps-30)<.005&&!f.interlaced&&f.pixel==="NV12")
            require(!!desired,"physical 1080p30 NV12 fixture")
            veyra.captureFormatKey=desired.id
            require(veyra.capturePixelId===desired.id,"full exact selection accepted")
            const resolution=veyra.captureResolutionOptions.find(r=>r.id!==veyra.captureResolutionId)
            require(!!resolution,"fixture offers multiple resolutions")
            veyra.captureResolutionId=resolution.id
            require(veyra.captureFormatKey.length===0,"stale media key cleared on size change")
            veyra.capturePixelId=desired.id
            require(veyra.capturePixelId.length===0&&!veyra.startCaptureSession()&&!veyra.running,"mismatched key cannot connect previous format")
            require(messages.some(x=>x.indexOf("此采集卡无此格式")>=0),"mismatched key emits visible format error")
            // Correcting the same formerly invalid identity must restore both axes.
            veyra.captureFormatKey=desired.id
            require(veyra.captureResolutionId==="1920x1080"&&veyra.capturePixelId===desired.id,"correction of invalid identity works")
            veyra.captureResolutionId=resolution.id
            console.log("CAPTURE_SAFETY_ENUM",all.length,Object.keys(reached).length)
            pass()
        }catch(e){console.error("CAPTURE_SAFETY_FAIL",e.message);Qt.exit(3)}}
    }
}

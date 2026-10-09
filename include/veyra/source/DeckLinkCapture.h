#pragma once
#include "veyra/pipeline/FramePacket.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::source::decklink {
// Public ingress contract contains no SDK declarations. The SDK adapter and
// the injected test driver both exercise Capture's actual state machine.
enum class Pixel { Uyvy8, Bgra8, V210, Rgb10xLE };
struct Mode {
    uint32_t id=0, width=0, height=0;
    int64_t duration=0, scale=0;
    bool interlaced=false;
    std::wstring name;
    pipeline::ColorDescription color;
};
struct Device { std::wstring name,path; bool autoDetection=false; };
struct Frame {
    const uint8_t* data=nullptr; size_t bytes=0;
    uint32_t width=0,height=0,stride=0;
    Pixel pixel=Pixel::Uyvy8;
    int64_t pts100ns=0,duration100ns=0;
    bool noInput=false,bottomUp=false;
    pipeline::ColorDescription color;
};
struct Signal { Mode mode; bool rgb=false; unsigned bits=0; };
size_t rowBytes(Pixel,uint32_t width);
bool sameColor(const pipeline::ColorDescription&,const pipeline::ColorDescription&);
bool isPath(std::wstring_view);
std::wstring modeKey(uint32_t);
std::vector<Device> devices();
std::vector<Mode> modes(std::wstring_view);

class DriverSink {
public:
    virtual ~DriverSink()=default;
    virtual void formatChanged(const Signal&)=0;
    virtual void video(const Frame&)=0;
    virtual void audio(const void*,size_t,int64_t pts100ns)=0;
    virtual void failure(std::wstring)=0;
};
class Driver {
public:
    virtual ~Driver()=default;
    virtual bool open(std::wstring_view,DriverSink&,std::wstring&)=0;
    virtual std::vector<Mode> modes()=0;
    virtual bool autoDetection()const=0;
    // No control method may run from a driver callback. stop() detaches the
    // callback and waits for all in-flight callbacks before returning.
    virtual bool enable(const Mode&,Pixel,bool automatic,bool audio,std::wstring&)=0;
    virtual bool start(std::wstring&)=0;
    virtual bool pause(std::wstring&)=0;
    virtual bool flush(std::wstring&)=0;
    virtual void stop()noexcept=0;
};
std::unique_ptr<Driver> makeDriver();

class Capture final:private DriverSink {
public:
    explicit Capture(std::unique_ptr<Driver> driver=makeDriver());
    ~Capture();
    // Probe only: bounded wait for an actual frame, no pixels/audio retained.
    bool configure(std::wstring_view path,std::wstring_view key,bool audio,
                   unsigned timeoutMs=2500);
    bool start(std::function<void(const Frame&)> video,
               std::function<void(const void*,size_t,int64_t)> audio={});
    void stop()noexcept;
    bool needsReopen()const;
    std::wstring error()const;
    Frame contract()const; // data is always null; metadata from valid probe frame
    Mode mode()const;
private:
    void formatChanged(const Signal&)override;
    void video(const Frame&)override;
    void audio(const void*,size_t,int64_t)override;
    void failure(std::wstring)override;
    struct Impl;std::unique_ptr<Impl> p_;
};
}

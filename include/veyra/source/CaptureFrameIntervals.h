#pragma once
#include <dshow.h>
#include <algorithm>
#include <vector>

namespace veyra::source {
// The driver owns this discrete list. A min/max interval range is not a list
// of supported standard frame rates; never expand it using a preset table.
inline std::vector<REFERENCE_TIME> captureFrameIntervals(IAMVideoControl* control, IPin* pin,
        int index, SIZE dimensions, REFERENCE_TIME advertised, HRESULT* queryResult=nullptr) {
    std::vector<REFERENCE_TIME> out{advertised};
    HRESULT hr=E_NOINTERFACE;
    if(control&&pin){
        long count=0;LONGLONG* rates=nullptr;
        hr=control->GetFrameRateList(pin,index,dimensions,&count,&rates);
        if(SUCCEEDED(hr)&&rates&&count>0&&count<=1000000)
            for(long i=0;i<count;++i)if(rates[i]>0&&std::find(out.begin(),out.end(),rates[i])==out.end())out.push_back(rates[i]);
        CoTaskMemFree(rates);
    }
    if(queryResult)*queryResult=hr;
    return out;
}
}

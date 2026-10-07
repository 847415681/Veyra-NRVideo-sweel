"""Query documented SDK selectors without loading models or changing binaries."""
from pathlib import Path
import ctypes as c,json,os,subprocess,sys
import psutil
ROOT=Path(__file__).resolve().parents[2];BASE=Path('E:/项目/Veyra');TASK='vfg-next-20261007'
subprocess.run([sys.executable,'-B',str(ROOT/'scripts/acceptance/vfg-next-control.py')],check=True)
assert not any(p.info['name'].lower() in ('veyra.exe','veyra_qml_ui.exe','veyra_vfg_gpu_tests.exe','veyra_vfg_export_probe.exe') for p in psutil.process_iter(['name']))
runtime=BASE/'tests'/TASK/'app/runtime/nvidia-vfg'
tmp=BASE/'tmp'/TASK/'capabilities-v2';tmp.mkdir(parents=True,exist_ok=False)
os.environ.update(TEMP=str(tmp),TMP=str(tmp),CUDA_CACHE_PATH=str(tmp/'cuda-cache'))
driver=c.CDLL(str(Path(os.environ['WINDIR'])/'System32/nvcuda.dll'),winmode=0x800)
def cu(name,args):
 f=getattr(driver,name);f.argtypes=args;f.restype=c.c_int;return f
assert cu('cuInit',[c.c_uint])(0)==0
context=c.c_void_p();assert cu('cuDevicePrimaryCtxRetain',[c.POINTER(c.c_void_p),c.c_int])(c.byref(context),0)==0
assert cu('cuCtxPushCurrent_v2',[c.c_void_p])(context)==0
with os.add_dll_directory(str(runtime)):
 loaded=[c.CDLL(str(runtime/name),winmode=0x100|0x800|0x400) for name in ('cudart64_12.dll','NVCVImage.dll','nvngxruntime.dll','NVVideoEffects.dll','nvVFXVideoFrameGeneration.dll')]
 sdk=c.CDLL(str(runtime/'NVVideoEffects.dll'),winmode=0x100|0x800|0x400)
 create=sdk.NvVFX_CreateEffect;create.argtypes=[c.c_char_p,c.POINTER(c.c_void_p)];create.restype=c.c_int
 destroy=sdk.NvVFX_DestroyEffect;destroy.argtypes=[c.c_void_p];destroy.restype=None
 get=sdk.NvVFX_GetU32;get.argtypes=[c.c_void_p,c.c_char_p,c.POINTER(c.c_uint)];get.restype=c.c_int
 setu=sdk.NvVFX_SetU32;setu.argtypes=[c.c_void_p,c.c_char_p,c.c_uint];setu.restype=c.c_int
 version=c.c_uint();sdk.NvVFX_GetVersion.argtypes=[c.POINTER(c.c_uint)];sdk.NvVFX_GetVersion.restype=c.c_int
 result={'runtime':str(runtime),'versionStatus':sdk.NvVFX_GetVersion(c.byref(version)),'version':hex(version.value),'selectors':[]}
 effect=c.c_void_p();result['createStatus']=create(b'VideoFrameGeneration',c.byref(effect));print('createStatus',result['createStatus'],flush=True);assert result['createStatus']==0
 try:
  for name,value in [('Mode',1),('CudaGraph',1),('BatchSize',1)]:
   before=c.c_uint();rc=get(effect,name.encode(),c.byref(before));setRc=setu(effect,name.encode(),value)
   after=c.c_uint();afterRc=get(effect,name.encode(),c.byref(after))
   result['selectors'].append({'name':name,'getStatus':rc,'before':before.value,'setStatus':setRc,'afterStatus':afterRc,'after':after.value})
 finally:destroy(effect)
previous=c.c_void_p();assert cu('cuCtxPopCurrent_v2',[c.POINTER(c.c_void_p)])(c.byref(previous))==0
assert cu('cuDevicePrimaryCtxRelease_v2',[c.c_int])(0)==0
out=BASE/'logs'/TASK/'sdk-capabilities.json';out.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps(result,ensure_ascii=False))

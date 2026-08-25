#!/usr/bin/env python
# Whole-network comparison, CPU vs NPU: one line per run --
# model, cpu/npu time, top-5 match, logit diff.  MOBILENET=<path> selects the model.
import os, sys, time
import numpy as np
try:
    import tflite_runtime.interpreter as tflite
except ImportError:
    from ai_edge_litert import interpreter as tflite
M=os.environ.get("MOBILENET","mobilenet_v1_1.0_224_quant.tflite")
def mk(mode):
    d=[tflite.load_delegate(os.environ.get("TEFLON_PATH","./mesa/build/src/gallium/targets/teflon/libteflon.so"))] if mode=="npu" else []
    ip=tflite.Interpreter(model_path=M,experimental_delegates=d)
    ip.allocate_tensors(); return ip
rng=np.random.default_rng(3)
res={}; ms={}; top={}
for mode in ["cpu","npu"]:
    ip=mk(mode)
    i0=ip.get_input_details()[0]; o0=ip.get_output_details()[0]
    if i0["dtype"]==np.uint8: x=rng.integers(0,256,i0["shape"],dtype=np.uint8)
    elif i0["dtype"]==np.int8: x=rng.integers(-128,128,i0["shape"],dtype=np.int8)
    else: x=rng.random(i0["shape"]).astype(i0["dtype"])
    rng=np.random.default_rng(3)
    ip.set_tensor(i0["index"],x)
    ip.invoke()                      # warm-up (first invoke includes allocation)
    t=time.time(); ip.invoke(); ms[mode]=(time.time()-t)*1000
    r=ip.get_tensor(o0["index"])[0].ravel()
    res[mode]=r.astype(np.float32) if np.issubdtype(r.dtype,np.floating) else r.astype(np.int32)
    top[mode]=np.argsort(res[mode])[-5:][::-1].tolist()
d=np.abs(res["cpu"]-res["npu"])
same=len(set(top["cpu"])&set(top["npu"]))
print(f'{os.path.basename(M)} cpu {ms["cpu"]:.0f}ms npu {ms["npu"]:.1f}ms top5 {same}/5'
      f'{" (order ok)" if top["cpu"]==top["npu"] else ""} logit diff max {d.max():g} mean {d.mean():.2f}'
      f' | npu top5 {top["npu"]}')

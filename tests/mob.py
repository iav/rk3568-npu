import os
import numpy as np
import tflite_runtime.interpreter as tflite
M=os.environ.get("MOBILENET","mobilenet_v1_1.0_224_quant.tflite")
def mk(mode):
    d=[tflite.load_delegate(os.environ.get("TEFLON_PATH","./mesa/build/src/gallium/targets/teflon/libteflon.so"))] if mode=="npu" else []
    ip=tflite.Interpreter(model_path=M,experimental_delegates=d)
    ip.allocate_tensors(); return ip
rng=np.random.default_rng(3)
x=rng.integers(0,256,(1,224,224,3),dtype=np.uint8)
res={}
import time
for mode in ["cpu","npu"]:
    ip=mk(mode)
    i0=ip.get_input_details()[0]; o0=ip.get_output_details()[0]
    ip.set_tensor(i0["index"],x)
    t=time.time(); ip.invoke(); dt=time.time()-t
    res[mode]=ip.get_tensor(o0["index"])[0].astype(np.int32)
    print(mode, f"{dt*1000:.0f}ms", "top5:", np.argsort(res[mode])[-5:][::-1].tolist())
d=np.abs(res["cpu"]-res["npu"])
print("дифф логитов: max",d.max(),"mean",round(float(d.mean()),2))

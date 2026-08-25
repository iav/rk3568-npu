import os
import numpy as np, sys
try:
    import tflite_runtime.interpreter as tflite
except ImportError:
    from ai_edge_litert import interpreter as tflite
model=sys.argv[1]
def mk(mode):
    d=[tflite.load_delegate(os.environ.get("TEFLON_PATH","./mesa/build/src/gallium/targets/teflon/libteflon.so"))] if mode=="npu" else []
    ip=tflite.Interpreter(model_path=model,experimental_delegates=d)
    ip.allocate_tensors(); return ip
cpu=mk("cpu"); npu=mk("npu")
i0=cpu.get_input_details()[0]
H,W,C=i0["shape"][1],i0["shape"][2],i0["shape"][3]
dt=i0["dtype"]
signed = dt==np.int8
off = -128 if signed else 0
ones=np.ones((1,H,W,C),np.int32)
tests={}
for f in [0,128,255]: tests[f"f{f}"]=ones*(f+off)
tests["colgrad"]=ones*(((np.arange(W)*2+60)%256)+off)[None,None,:,None]
tests["rowgrad"]=ones*(((np.arange(H)*2+60)%256)+off)[None,:,None,None]
tests["chan"]=ones*(((np.arange(C)*8)%256)+off)[None,None,None,:]
tests["mix"]=(((np.arange(H)[:,None,None]*3+np.arange(W)[None,:,None]*5+np.arange(C)[None,None,:]*17)%256)+off)[None]
out=[]
for name,x in tests.items():
    x=x.astype(dt)
    r={}
    for ip,tag in [(cpu,"cpu"),(npu,"npu")]:
        j=ip.get_input_details()[0]; o=ip.get_output_details()[0]
        ip.set_tensor(j["index"],x); ip.invoke()
        r[tag]=ip.get_tensor(o["index"])[0].astype(np.int32)
    d=np.abs(r["cpu"]-r["npu"])
    out.append(f"{name}:{d.max()}/{d.mean():.2f}")
print(model.split("/")[-1], " ".join(out))

# yolov8 tflite (float in NCHW? check) CPU vs NPU: run both, decode + NMS, compare boxes.
import os, sys, numpy as np, time
from ai_edge_litert import interpreter as tflite
from PIL import Image
M=sys.argv[1]; IMG=sys.argv[2]
def run(delegate):
    d=[tflite.load_delegate(os.environ.get("TEFLON_PATH","./mesa/build/src/gallium/targets/teflon/libteflon.so"))] if delegate else []
    ip=tflite.Interpreter(model_path=M,experimental_delegates=d); ip.allocate_tensors()
    i0=ip.get_input_details()[0]; o0=ip.get_output_details()[0]
    sh=i0['shape']; nchw = sh[1]==3
    s=sh[2] if nchw else sh[1]
    im=Image.open(IMG).convert("RGB").resize((s,s))
    x=np.asarray(im,np.float32)/255.0
    x=x.transpose(2,0,1)[None] if nchw else x[None]
    ip.set_tensor(i0['index'],x.astype(i0['dtype'])); ip.invoke()
    t=time.time(); ip.invoke(); dt=(time.time()-t)*1000
    return ip.get_tensor(o0['index'])[0], dt, s
def decode(out, s, conf=0.25, iou=0.5):
    # out: [84, N] -> boxes cxcywh (pixels) + 80 class scores
    boxes=out[:4].T; sc=out[4:].T
    cls=sc.argmax(1); score=sc.max(1); keep=score>conf
    boxes,cls,score=boxes[keep],cls[keep],score[keep]
    xy1=boxes[:,:2]-boxes[:,2:]/2; xy2=boxes[:,:2]+boxes[:,2:]/2
    b=np.concatenate([xy1,xy2],1); order=score.argsort()[::-1]; res=[]
    while len(order):
        i=order[0]; res.append(i)
        if len(order)==1: break
        r=order[1:]
        xx1=np.maximum(b[i,0],b[r,0]); yy1=np.maximum(b[i,1],b[r,1]); xx2=np.minimum(b[i,2],b[r,2]); yy2=np.minimum(b[i,3],b[r,3])
        inter=np.clip(xx2-xx1,0,None)*np.clip(yy2-yy1,0,None)
        a_i=(b[i,2]-b[i,0])*(b[i,3]-b[i,1]); a_r=(b[r,2]-b[r,0])*(b[r,3]-b[r,1])
        o=inter/(a_i+a_r-inter+1e-9)
        order=r[(o<iou)|(cls[r]!=cls[i])]
    return [(int(cls[i]),float(score[i]),[round(float(v),1) for v in b[i]]) for i in res]
names={0:"person",5:"bus",2:"car",7:"truck",11:"stop sign"}
for tag,dl in (("cpu",False),("npu",True)):
    out,dt,s=run(dl); det=decode(out,s)
    print("%s %.1fms %d dets:"%(tag,dt,len(det)))
    for c,sc,b in det[:8]: print("   %-10s %.2f %s"%(names.get(c,c),sc,b))
    if tag=="cpu": ref=out
    else:
        d=np.abs(out-ref); print("raw out diff: max %.3f mean %.4f (boxes rows max %.2f, scores rows max %.3f)"%(d.max(),d.mean(),d[:4].max(),d[4:].max()))

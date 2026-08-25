#!/usr/bin/env python3
"""Сравнить командный поток mesa с вендорским по всем задачам одной модели.

Вендорский поток берётся из .rknn (tools/rknn_regcmd.py), поток mesa — из
дампа `RKT_DUMP=1` (патч в emit_raw). Нарезка на задачи у сторон разная:
вендор и mesa режут слой на полосы по высоте по-своему, поэтому задачи
сопоставляются не по номеру, а по «подписи слоя» — геометрии, которая от
нарезки не зависит.

Расхождения делятся на три класса:
  ПОЛОСА   — регистр зависит от высоты полосы, разница ожидаема;
  АДРЕС    — буферы, сравнивать бессмысленно;
  РАСХОЖДЕНИЕ — всё остальное, то есть настоящий список работы.
"""
import argparse
import json
import os
import re
import sys
from collections import defaultdict, OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util

_spec = importlib.util.spec_from_file_location(
    "rknn_regcmd", os.path.join(os.path.dirname(os.path.abspath(__file__)), "rknn_regcmd.py"))
rr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rr)

# Регистры, значение которых зависит от высоты полосы, а не от слоя.
BAND_REGS = {
    0x1020,  # CNA_DATA_SIZE0   — DATAIN_HEIGHT
    0x102c,  # CNA_DATA_SIZE3   — DATAOUT_ATOMICS
    0x1084,  # CNA_FC_DATA_SIZE0
    0x3010,  # CORE DATAOUT_SIZE_0 (по факту) — высота выхода полосы
    0x4034,  # DPU_DATA_CUBE_HEIGHT
    0x405c,  # DPU_WDMA_SIZE_1
    0x5010,  # DPU_RDMA_DATA_CUBE_HEIGHT
    0x0010,  # PC_BASE_ADDRESS    — смещение следующей задачи
    0x0014,  # PC_REGISTER_AMOUNTS
}

ADDR_REGS = {0x1070, 0x1110, 0x4020, 0x5018, 0x5020, 0x502c, 0x5038}


def task_regs(task):
    out = OrderedDict()
    for word in task:
        if word == 0:
            continue
        target, reg, value = rr.decode(word)
        out[(target, reg)] = value
    return out


def layer_key(v):
    """Подпись слоя: то, что не зависит от нарезки на полосы."""
    def g(reg, lo, hi):
        for (t, r), val in v.items():
            if r == reg:
                return (val >> lo) & ((1 << (hi - lo + 1)) - 1)
        return None
    return (
        g(0x1038, 24, 28),  # WEIGHT_WIDTH
        g(0x1038, 16, 20),  # WEIGHT_HEIGHT
        g(0x1038, 0, 13),   # WEIGHT_KERNELS
        g(0x1014, 0, 5),    # шаги свёртки
        g(0x1020, 16, 26),  # DATAIN_WIDTH
        g(0x1024, 0, 15),   # DATAIN_CHANNEL
        g(0x1028, 0, 10),   # DATAOUT_WIDTH
    )


def load_mesa(path):
    words = []
    for line in open(path):
        m = re.search(r"rkt regcmd ([0-9a-f]{16})", line)
        if m:
            words.append(int(m.group(1), 16))
    return words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rknn")
    ap.add_argument("mesa_dump")
    ap.add_argument("--regnames", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "vendor-blob", "regnames.json"))
    ap.add_argument("--show-band", action="store_true", help="показать и полосовые тоже")
    args = ap.parse_args()

    regnames = json.load(open(args.regnames))
    data = open(args.rknn, "rb").read()
    vend, _ = rr.split_tasks(rr.words_from(data, rr.find_regcmd(data)))
    mesa, _ = rr.split_tasks(load_mesa(args.mesa_dump))

    vgroups, mgroups = defaultdict(list), defaultdict(list)
    for t in vend:
        v = task_regs(t)
        vgroups[layer_key(v)].append(v)
    for t in mesa:
        v = task_regs(t)
        mgroups[layer_key(v)].append(v)

    print("# вендор: %d задач, %d слоёв; mesa: %d задач, %d слоёв" % (
        len(vend), len(vgroups), len(mesa), len(mgroups)))
    common = [k for k in vgroups if k in mgroups]
    print("# слоёв, сопоставленных по подписи: %d" % len(common))
    only_v = [k for k in vgroups if k not in mgroups]
    only_m = [k for k in mgroups if k not in vgroups]
    for k in only_v:
        print("#   только у вендора: kw=%s kh=%s kern=%s Win=%s Cin=%s Wout=%s" % (
            k[0], k[1], k[2], k[4], k[5], k[6]))
    for k in only_m:
        print("#   только у mesa:    kw=%s kh=%s kern=%s Win=%s Cin=%s Wout=%s" % (
            k[0], k[1], k[2], k[4], k[5], k[6]))

    # (target, reg) -> список (подпись, вендорское, mesa)
    diffs = defaultdict(list)
    for k in common:
        a, b = vgroups[k][0], mgroups[k][0]
        for rk in sorted(set(a) | set(b)):
            va, vb = a.get(rk), b.get(rk)
            if va != vb:
                diffs[rk].append((k, va, vb))

    def name(target, reg):
        e = regnames.get(str(reg))
        return "%s %s" % (e[0][0], e[0][1]) if e else "%s %#06x" % (
            rr.TARGETS.get(target, hex(target)), reg)

    def fmt(x):
        return "—" if x is None else "%#010x" % x

    band, addr, real = [], [], []
    for rk, lst in sorted(diffs.items(), key=lambda kv: kv[0][1]):
        bucket = band if rk[1] in BAND_REGS else addr if rk[1] in ADDR_REGS else real
        bucket.append((rk, lst))

    print("\n=== РАСХОЖДЕНИЯ (%d регистров) ===" % len(real))
    for rk, lst in real:
        vals = {(va, vb) for _, va, vb in lst}
        print("  %-32s %#06x  слоёв=%d" % (name(*rk), rk[1], len(lst)))
        for va, vb in sorted(vals, key=lambda p: (p[0] is None, p[0] or 0))[:6]:
            print("        вендор=%-12s mesa=%s" % (fmt(va), fmt(vb)))
    print("\n=== зависят от нарезки на полосы (%d) ===" % len(band))
    print("   " + ", ".join(name(*rk) for rk, _ in band))
    print("=== адреса буферов (%d) ===" % len(addr))
    print("   " + ", ".join(name(*rk) for rk, _ in addr))


if __name__ == "__main__":
    main()

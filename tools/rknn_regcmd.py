#!/usr/bin/env python3
"""Извлечь и декодировать вендорский regcmd прямо из .rknn-файла.

Поток команд, который librknnrt заливает в NPU, целиком лежит в контейнере
модели: рантайм правит в нём только адреса буферов (веса, фичи, выход, bias).
Проверено на mobilenet_v1_rk3568.rknn — живой дамп из dmesg совпал с файлом
слово в слово, кроме четырёх адресных.

Слово 64 бита: [63:48]=target [47:16]=value [15:0]=reg.
"""
import argparse
import json
import os
import struct
import sys

# пролог задачи: S_POINTER = 0xe в CNA / CMAC / CORE
PROLOGUE = (0x02010000000E1004, 0x04010000000E2004, 0x08010000000E3004)

TARGETS = {0x0201: "CNA", 0x0401: "CMAC", 0x0801: "CORE", 0x1001: "DPU",
           0x2001: "DPU_RDMA", 0x0101: "PC"}


def load_words(path):
    data = open(path, "rb").read()
    return data


def find_regcmd(data):
    """Найти начало потока по прологу первой задачи."""
    pat = b"".join(w.to_bytes(8, "little") for w in PROLOGUE)
    off = data.find(pat)
    if off < 0:
        sys.exit("пролог задачи не найден — формат контейнера другой")
    return off


def words_from(data, off):
    n = (len(data) - off) // 8
    return [int.from_bytes(data[off + 8 * i:off + 8 * i + 8], "little") for i in range(n)]


def decode(word):
    reg = word & 0xFFFF
    value = (word >> 16) & 0xFFFFFFFF
    target = (word >> 48) & 0xFFFF
    return target, reg, value


def valid(word):
    target, reg, _ = decode(word)
    block = {0x0101: 0x0000, 0x0201: 0x1000, 0x0401: 0x2000,
             0x0801: 0x3000, 0x1001: 0x4000, 0x2001: 0x5000}.get(target)
    if block is None:
        return False
    return block <= reg < block + 0x1000


def split_tasks(words):
    """Порезать поток на задачи по прологу.

    Хвост задачи (записи PC + два слова с чужими target'ами + нулевое
    выравнивание) в поток входит; конец потока — первый прогон из восьми нулей
    после последнего пролога.
    """
    pros = [i for i in range(len(words) - 2) if tuple(words[i:i + 3]) == PROLOGUE]
    if not pros:
        sys.exit("пролог задачи не найден")
    end = len(words)
    zeros = 0
    for i in range(pros[-1], len(words)):
        zeros = zeros + 1 if words[i] == 0 else 0
        if zeros >= 8:
            end = i - 7
            break
    bounds = pros + [end]
    return [words[bounds[k]:bounds[k + 1]] for k in range(len(pros))], end


def fmt(regnames, target, reg, value):
    ent = regnames.get(str(reg))
    if not ent:
        return "%-8s %#06x = %#010x" % (TARGETS.get(target, hex(target)), reg, value)
    domain, name, fields = ent[0]
    parts = []
    for fname, lo, hi in fields:
        if fname.startswith("RESERVED"):
            continue
        fval = (value >> lo) & ((1 << (hi - lo + 1)) - 1)
        if fval:
            parts.append("%s=%d" % (fname, fval))
    tail = " ".join(parts)
    return "%-8s %-28s = %#010x  %s" % (domain, name, value, tail)



# (заголовок, домен, регистр, поле) — разрешаются в оффсеты по regnames.json
TABLE_FIELDS = [
    ("Win",     "CNA", "DATA_SIZE0", "DATAIN_WIDTH"),
    ("Hin",     "CNA", "DATA_SIZE0", "DATAIN_HEIGHT"),
    ("Cin",     "CNA", "DATA_SIZE1", "DATAIN_CHANNEL"),
    ("CinR",    "CNA", "DATA_SIZE1", "DATAIN_CHANNEL_REAL"),
    ("Wout",    "CNA", "DATA_SIZE2", "DATAOUT_WIDTH"),
    ("atomic",  "CNA", "DATA_SIZE3", "DATAOUT_ATOMICS"),
    ("grains",  "CNA", "CONV_CON2",  "FEATURE_GRAINS"),
    ("strX",    "CNA", "CONV_CON3",  "CONV_X_STRIDE"),
    ("strY",    "CNA", "CONV_CON3",  "CONV_Y_STRIDE"),
    ("kw",      "CNA", "WEIGHT_SIZE2", "WEIGHT_WIDTH"),
    ("kh",      "CNA", "WEIGHT_SIZE2", "WEIGHT_HEIGHT"),
    ("kern",    "CNA", "WEIGHT_SIZE2", "WEIGHT_KERNELS"),
    ("wbytes",  "CNA", "WEIGHT_SIZE0", "WEIGHT_BYTES"),
    ("wbank",   "CNA", "CBUF_CON0",  "WEIGHT_BANK"),
    ("dbank",   "CNA", "CBUF_CON0",  "DATA_BANK"),
    ("entries", "CNA", "CBUF_CON1",  "DATA_ENTRIES"),
    ("fetchpx", "CNA", "DMA_CON0",   "FETCH_PIXEL_LEN"),
    ("lstride", "CNA", "DMA_CON1",   "LINE_STRIDE"),
    ("sstride", "CNA", "DMA_CON2",   "SURF_STRIDE"),
    ("dpuW",    "DPU", "DATA_CUBE_WIDTH",   "WIDTH"),
    ("dpuH",    "DPU", "DATA_CUBE_HEIGHT",  "HEIGHT"),
    ("dpuC",    "DPU", "DATA_CUBE_CHANNEL", "CHANNEL"),
    ("dstsurf", "DPU", "DST_SURF_STRIDE",   "DST_SURF_STRIDE"),
]


def resolve_fields(regnames):
    """Найти (оффсет, low, high) для каждой строки TABLE_FIELDS."""
    index = {}
    for off, entries in regnames.items():
        for domain, name, fields in entries:
            for fname, lo, hi in fields:
                index[(domain, name, fname)] = (int(off), lo, hi)
    out = []
    for label, domain, reg, field in TABLE_FIELDS:
        key = (domain, reg, field)
        if key not in index:
            sys.exit("нет в regnames.json: %s" % (key,))
        off, lo, hi = index[key]
        out.append((label, off, lo, hi))
    return out


def task_table(tasks, regnames):
    fields = resolve_fields(regnames)
    hdr = ["task", "words"] + [f[0] for f in fields]
    print(" ".join("%7s" % h for h in hdr))
    for idx, t in enumerate(tasks):
        vals = {}
        for word in t:
            _, reg, value = decode(word)
            vals[reg] = value
        row = ["%d" % idx, "%d" % len(t)]
        for _, off, lo, hi in fields:
            v = vals.get(off)
            row.append("-" if v is None else "%d" % ((v >> lo) & ((1 << (hi - lo + 1)) - 1)))
        print(" ".join("%7s" % c for c in row))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rknn")
    ap.add_argument("--regnames", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "vendor-blob", "regnames.json"))
    ap.add_argument("--task", type=int, help="печатать только эту задачу")
    ap.add_argument("--summary", action="store_true", help="только сводка по задачам")
    ap.add_argument("--table", action="store_true", help="таблица геометрии по задачам")
    args = ap.parse_args()

    regnames = json.load(open(args.regnames))
    data = load_words(args.rknn)
    off = find_regcmd(data)
    words = words_from(data, off)
    tasks, used = split_tasks(words)

    print("# %s: regcmd @ offset %d, %d слов, %d задач" % (
        os.path.basename(args.rknn), off, used, len(tasks)))

    if args.table:
        task_table(tasks, regnames)
        return

    for idx, t in enumerate(tasks):
        if args.task is not None and idx != args.task:
            continue
        print("\n=== task %d: %d слов ===" % (idx, len(t)))
        if args.summary:
            continue
        for w in t:
            target, reg, value = decode(w)
            print("  " + fmt(regnames, target, reg, value))


if __name__ == "__main__":
    main()

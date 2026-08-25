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

# пролог обычной задачи: S_POINTER = 0xe в CNA / CMAC / CORE.  Только для
# справки — резка идёт по PC-хвосту, потому что LUT-загрузчики и DPU-only
# копии пролога не имеют (Test 73).
PROLOGUE = (0x02010000000E1004, 0x04010000000E2004, 0x08010000000E3004)

TARGETS = {0x0201: "CNA", 0x0401: "CMAC", 0x0801: "CORE", 0x1001: "DPU",
           0x2001: "DPU_RDMA", 0x4001: "PPU", 0x8001: "PPU_RDMA", 0x0101: "PC"}
BLOCKS = {0x0101: 0x0000, 0x0201: 0x1000, 0x0401: 0x2000, 0x0801: 0x3000,
          0x1001: 0x4000, 0x2001: 0x5000, 0x4001: 0x6000, 0x8001: 0x7000}
# два слова хвоста задачи: OP_ENABLE-строб (target 0x41) и широковещательный
# старт (0x81); значение у 0x81 — маска юнитов (0x1f conv, 0x60 PPU-пара).
TAIL_OPEN = 0x0041
TAIL_LAST = 0x0081


def load_words(path):
    data = open(path, "rb").read()
    return data


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
    if target in (TAIL_OPEN, TAIL_LAST):
        return True
    block = BLOCKS.get(target)
    if block is None:
        return False
    return block <= reg < block + 0x1000


def find_regcmd(data):
    """Выравнивание потока в контейнере: 0 или 4 байта — где больше валидных слов."""
    best = (0, -1)
    for off in (0, 4):
        n = sum(1 for w in words_from(data, off) if valid(w))
        if n > best[1]:
            best = (off, n)
    if best[1] < 8:
        sys.exit("командный поток не найден — формат контейнера другой")
    return best[0]


def regions(words):
    """Прогоны валидных слов; внутри прогона допускаются до 7 нулей подряд
    (выравнивание хвостов), 8+ нулей или невалидное слово — граница."""
    out, start, zeros = [], None, 0
    for i, w in enumerate(words):
        if w == 0:
            zeros += 1
            if zeros >= 8 and start is not None:
                out.append((start, i - 7))
                start = None
            continue
        if not valid(w):
            if start is not None:
                out.append((start, i))
                start = None
            zeros = 0
            continue
        zeros = 0
        if start is None:
            start = i
    if start is not None:
        out.append((start, len(words)))
    return out


def split_tasks(words, verbose=True):
    """Порезать поток на задачи по PC-хвосту.

    Поток = самый длинный прогон валидных слов в контейнере (остальные прогоны —
    таблицы адресных патчей рантайма и заготовки, они печатаются счётчиком).
    Задача = слова до широковещательного старта (target 0x81) включительно;
    нули-выравнивание пропускаются.  Так видны и задачи без CNA-пролога
    (LUT-загрузчики, DPU-only копии — Test 73).
    """
    regs = regions(words)
    if not regs:
        sys.exit("командный поток не найден")
    start, end = max(regs, key=lambda r: r[1] - r[0])
    if verbose and len(regs) > 1:
        others = [e - b for b, e in regs if (b, e) != (start, end)]
        print("# другие прогоны в контейнере (пропущены): %d, слов %s" % (
            len(others), others[:12]), file=sys.stderr)
    tasks, cur = [], []
    for w in words[start:end]:
        if w == 0:
            continue
        cur.append(w)
        if decode(w)[0] == TAIL_LAST:
            tasks.append(cur)
            cur = []
    if cur:
        tasks.append(cur)
    return tasks, end - start


BASE_ADDR_REGS = {0x4020, 0x5018, 0x5020, 0x502c, 0x5038,   # DPU DST, RDMA SRC/BS/BN/EW
                  0x6070, 0x701c, 0x1070, 0x1110}         # PPU DST, PPU_RDMA SRC, CNA feature, DCOMP_ADDR0


def task_kind(task):
    """conv / ppu / lut / dpu — по составу блоков; reloc / skeleton — не задачи.

    Хвост контейнера повторяет для каждой задачи её адресные слова (таблица
    патчей рантайма: S_POINTER + *_BASE_ADDR + PC) — валидны как слова, но не
    исполняются.  73-словные DPU-only задачи без CNA — настоящие (копии
    8px x 32ch, OUTPUT_MODE=4), их не трогаем.
    """
    words = [decode(w) for w in task]
    body = [(t, r, v) for t, r, v in words
            if t not in (0x0101, TAIL_OPEN, TAIL_LAST) and (r & 0xfff) != 0x004]
    if body and all(r in BASE_ADDR_REGS for _, r, _ in body):
        return "reloc"
    targets = set(t for t, _, _ in words)
    mask = [v >> 16 for t, _, v in words if t == TAIL_LAST]
    if 0x4001 in targets or (mask and mask[0] & 0x60):
        return "ppu"
    if 0x0201 in targets:
        return "conv"
    if any(r == 0x4100 and v for _, r, v in body):  # LUT_ACCESS_CFG armed
        return "lut"
    return "dpu"


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
    hdr = ["task", "kind", "words"] + [f[0] for f in fields]
    print(" ".join("%7s" % h for h in hdr))
    for idx, t in enumerate(tasks):
        vals = {}
        for word in t:
            _, reg, value = decode(word)
            vals[reg] = value
        row = ["%d" % idx, task_kind(t), "%d" % len(t)]
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
    ap.add_argument("--all", action="store_true", help="не отбрасывать reloc-записи (адресные патчи)")
    args = ap.parse_args()

    regnames = json.load(open(args.regnames))
    data = load_words(args.rknn)
    off = find_regcmd(data)
    words = words_from(data, off)
    tasks, used = split_tasks(words)
    if not args.all:
        dropped = [task_kind(t) for t in tasks if task_kind(t) == "reloc"]
        tasks = [t for t in tasks if task_kind(t) != "reloc"]
        if dropped:
            print("# отброшено записей: %d (%s); --all чтобы видеть" % (
                len(dropped), ", ".join(sorted(set(dropped)))), file=sys.stderr)

    print("# %s: regcmd @ offset %d, %d слов, %d задач" % (
        os.path.basename(args.rknn), off, used, len(tasks)))

    if args.table:
        task_table(tasks, regnames)
        return

    for idx, t in enumerate(tasks):
        if args.task is not None and idx != args.task:
            continue
        print("\n=== task %d (%s): %d слов ===" % (idx, task_kind(t), len(t)))
        if args.summary:
            continue
        for w in t:
            target, reg, value = decode(w)
            print("  " + fmt(regnames, target, reg, value))


if __name__ == "__main__":
    main()

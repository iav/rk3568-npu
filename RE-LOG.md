<!-- English translation of the lab log kept in Russian on the author's machine
     (RE-LOG.md, 2026-08-20 .. 2026-08-25).  Chronological, unedited: dead ends
     and wrong guesses are left in, later tests correct earlier ones.  "Test N"
     numbers are what README-rk3568.md and the Mesa commit messages refer to. -->

# RE-LOG: why the RK3568 NPU does not finish computing under mainline rocket

Reverse-engineering log, session 2026-08-20 (droid, agent).
Baseline: clean upstream submit/IRQ in rocket_job.c, symptom — PC starts,
CNA reads data, IRQ (GIC 183) = 0, drm_sched timeout 500 ms.

## Static analysis of vendor rknpu (BSP 6.1, drivers/rknpu)

The vendor per-submit path for RK3568 (`rknpu_job_subcore_commit[_pc]`,
num_irqs=1 → the branch with S_POINTER is NOT executed):

| # | write | value |
|---|--------|----------|
| 1 | 0x10 PC_DATA_ADDR | 0x1 ("switch to slave mode") |
| 2 | 0x10 PC_DATA_ADDR | regcmd_addr |
| 3 | 0x14 PC_DATA_AMOUNT | (regcfg_amount + 4 + scale-1)/scale − 1 (scale=1) |
| 4 | 0x20 INT_MASK | last_task->int_mask (from userspace) |
| 5 | 0x24 INT_CLEAR | first_task->int_mask |
| 6 | 0x30 TASK_CONTROL | ((0x6 \| pp)<<12) \| task_number → **0x6001** without ping-pong |
| 7 | 0x34 PC_DMA_BASE_ADDR | args->task_base_addr |
| 8 | 0x08 OP_EN | 1, then 0 (pulse) |

Vendor power_on: regulator + clk_bulk + genpd; `state_init=NULL`,
`cache_sgt_init=NULL` for rk356x ⇒ no hidden MMIO at power-on.
`bw_priority_base` (0xfe180008) is used ONLY from the
GET/SET_BW_PRIORITY ioctls (userspace initiative), not in the submit path.
soft_reset — only on timeout. probe — ioremap+irq (IRQF_SHARED), no MMIO magic.
`RKNPU_OFFSET_ENABLE_MASK 0xf008` is defined but never used.

Divergences rocket (upstream) ↔ rknpu (RK3568):

1. **TASK_CON: rocket writes 0x7001** (RESERVED_0(1)=bit14, TASK_COUNT_CLEAR(1)=bit13,
   TASK_PP_EN(1)=bit12, num=1), **vendor — 0x6001** (bit12=pp_en is NOT set for
   a regular task). The observed readback TASK_CON=0x1001 after a rocket submit
   means: bits 13/14 self-clear, while bit12 (PP_EN) STICKS=1.
   → candidate #1.
2. S_POINTER: rocket writes 0xe into CNA/CORE S_POINTER; the vendor on RK3568 does not
   touch it at all (the skip experiment was already done — "changes nothing", but not in combination
   with TASK_CON).
3. PC_DATA_AMOUNT: the vendor adds RKNPU_PC_DATA_EXTRA_AMOUNT=4, but its
   userspace passes the amount WITHOUT the trailing 4 entries, while mesa regcfg_amount
   INCLUDES them ⇒ semantics are equivalent (upstream works on RK3588).
   Low priority. (NB: in mesa rkt_ml.c the task-chain patching hardcodes
   scale=2: `regs_to_fetch/2` — on RK3568 this will under-fetch the SECOND task,
   but the first one does not complete anyway ⇒ not the primary gate, note it for mesa.)
4. TASK_DMA_BASE_ADDR: vendor writes args->task_base_addr, rocket 0x0
   (on RK3588 0 works). Low priority.
5. OP_EN pulse vs level — already checked, no effect.
6. INT_MASK/CLEAR: vendor — task masks from userspace, rocket — DPU_0|DPU_1;
   affects only IRQ delivery, not completion (task counter stays at 0).

## Experiments

### Test 1 (2026-08-20 ~19:5x): TASK_CON 0x6001 instead of 0x7001 (without TASK_PP_EN)
- Hypothesis: rocket always sets bit12 (TASK_PP_EN), the vendor does not; a stuck PP_EN
  prevents completion.
- Done: removed PC_TASK_CON_TASK_PP_EN(1) from rocket_job_hw_submit; build on m1,
  reload, run classification.py + npu-watch2.py (PC + CNA/CORE S_STATUS/S_POINTER).
- Observed: TASK_CON reads 0x0001 (the patch works, bits 13/14 self-clear).
  IRQ GIC 183 = 0, all tasks time out. BUT the watch gave something new:
  * BASE_ADDR "steps" by +0x1000 once every ~500 ms = these are the ADDRESSES OF NEW tasks after
    timeouts (regcmd of each next task), not fetch progress.
  * **CNA_S_STATUS = CORE_S_STATUS = 0x0 the whole way** — the sub-blocks do not even
    enter "operating" (midgy with his userspace gets 0x1).
  * **INT_RAW bit16 (0x10000) is raised on every task** and is cleared only
    by the reset on timeout. INT_MASK=0x300 (DPU_0|DPU_1) does not let it through.
  * AMOUNTS=0x81 (130 regcmd entries from mesa).
- Conclusion: the patch did not help, reverted. THE MAIN POINT: with mesa **main** the command list
  is RK3588-flavored (12 CBUF banks, broadcast OP_ENABLE target 0x81, missing
  CONV_CON4 etc., see MR !42134), so the sub-blocks on RK3568 are not
  configured/started at all. Testing kernel patches on mesa main is
  pointless: completion will not happen with any kernel.
  INT_RAW bit16 looks not like garbage but like a status "PC finished fetch/dispatch"
  (or a broadcast error) — it is raised strictly with every task.

### Decision: reproduce midgy's baseline
mesa MR !42134 (midgy971/mesa, branch rk3568-draft, sha b79779ac) contains
userspace fixes for RK3568 (CBUF 8 banks, CONV_CON4, FETCH_PIXEL_LEN,
individual op_en, DCOMP layout, 5×S_POINTER wake). With it + his kernel
RFC: CNA starts (S_STATUS=1), interrupts arrive, but DPU write=0, no completion,
S_POINTER bit16 "executer engage" does not appear. Our target is the kernel gate;
for that we need his userspace. Fetch the branch into /media/nvme/mesa → mr42134-rk3568.
(midgy's kernel RFC could not be found on lore — lore is behind Anubis; the RFC content
is almost certainly = our variant edits: dma 32, scale 1, compatible rk3568.)

### Test 2: mesa MR !42134 (branch mr42134-rk3568) + upstream rocket
- Build of midgy's branch in /media/nvme/mesa (1 commit, 3 rocket files). Run.
- Result: THE SAME: S_STATUS CNA/CORE = 0, INT_RAW bit16 on every task,
  IRQ=0. Plus something new in dmesg: `rk_iommu: Enable stall request timed out, status:
  0x000009`, `Disable paging request timed out` in the reset path.
- Conclusion: even with the RK3568-corrected cmdstream NOTHING starts ⇒ the gate is below
  the level of the command list contents. (midgy's symptom "CNA operating" did not
  reproduce for us — he had his own kernel RFC.)

### Finding: the kiln project (github.com/gahingwoo/kiln, author Jiaxing Hu)
"Run LLMs and vision models on RK35xx NPU on mainline kernel", RK3568 (ROCK 3B)
= vision works. In kernel-patches-rk3568/ there are THREE patches, the key one:
**0003-iommu-rockchip-force-gfp-dma32-for-v2-page-tables.patch**:
> The RK3566/RK3568 IOMMU v2 hardware cannot fetch a page table located
> above 4 GB, but iommu_data_ops_v2 left .gfp_flags = 0, so on >4GB boards
> the NPU IOMMU bus-errors.
Fix: `.gfp_flags = GFP_DMA32` in iommu_data_ops_v2 (drivers/iommu/rockchip-iommu.c).
In our 7.1.7-edge-rockchip64: `.gfp_flags = 0` — the bug is present. m1 = 8 GB RAM.

### Test 3: reading MMU_DTE_ADDR (0xfde4b000+0x0) during a task
- iommu-watch.py: DTE_ADDR=0xad38f100, STATUS=0x9, INT_RAWSTAT=0 (no faults).
- v2 decoding: bits 31:12 = phys[31:12], bits 11:8 = phys[35:32] ⇒
  **DT physically @ 0x1_ad38f000 (~6.7 GB) — ABOVE 4 GB. Hypothesis confirmed.**
- The walker reads the "directory" at the truncated address 0xad38f000 (random memory)
  ⇒ all translations are garbage, no faults are generated (the walker does not know it erred),
  the PC fetches garbage → no sub-block gets configured → neither IRQ nor
  completion. This also explains INT_RAW bit16 (garbage/erroneous fetch) and
  "Enable stall timed out" on detach.

### Test 4 (decisive): reboot with mem=4G
If with RAM ≤ 4 GB (all tables <4G) the inference completes — the root cause is proven.
The permanent fix is the GFP_DMA32 kernel patch (in Armbian as a userpatch + upstream).

### Test 4: mem=4G — GATE #1 FOUND AND PROVEN
- /boot/armbianEnv.txt: extraargs=cma=256M mem=4G (backup armbianEnv.txt.pre-mem4g),
  rockpi-power cycle, m1 came up with 3.8 GiB.
- Run (mesa MR branch + upstream rocket):
  * DTE_ADDR=0x64864000 — the directory is now <4 GB.
  * **CNA_S_STATUS=0x5, CORE_S_STATUS=0x5 — the sub-blocks get configured and
    WORK** (previously a perpetual 0x0).
  * TASK_STATUS=0xf000 (was 0x7000), OP_EN self-clears to 0 (PC finished),
    INT_RAW=0 all the time — the garbage bit16 is GONE (it was a symptom of the garbage fetch).
  * But still IRQ=0, tasks do not retire (counter 0), timeouts.
- Conclusion: **the root of the "dead" behavior = rk_iommu v2 page tables above 4 GB**
  (kiln patch 0003 is correct). After the workaround we are exactly in midgy's state:
  the pipeline is alive, MAC/DPU does not finish, no S_POINTER bit16 "engage".
  Gate #2 remains.

### Test 5–7: unmasking all 17 bits + IRQ logging (diagnostics)
- T5 (mask=0x1ffff, any bit = completion): **GIC 183 = 28 interrupts!**
  All raw_status=0x00010000 — bit16 is a REAL RK3568 interrupt
  (not garbage): maskable, the line actually toggles. ~1 IRQ per task cycle.
  One watch sample caught CORE_S_STATUS=0x00020005 (status_1=group 1 engaged).
- T6/T7 (bit16 logged only, completion only on DPU): IRQs arrive about
  ~1 ms AFTER the resubmit (after reset+attach), and at that moment
  **PC raw=0, status=0** — the source of the line at that moment is not the PC (probably
  the IOMMU neighbor on the line or a self-clearing pulse). rk_iommu
  is silent because of the early exit in `pm_runtime_get_if_in_use`.
  The DPU bits (0x100/0x200) NEVER arrive. MAC/DPU does not finish — gate #2.
- Note: restarting the module with power/control=on + after the iommu WARNs gave a
  machine check + oops in probe → **always return power/control=auto before
  rmmod**; after an MC — only a power cycle.

### Test 8: remove the S_POINTER writes (full RK3568 vendor parity) — WORSE
- Without the 0xe writes into CNA/CORE S_POINTER: **interrupt storm** (38k+ and growing),
  line 183 stays high, RCU starvation, m1 died until a power cycle.
- Conclusion: on the mainline path the S_POINTER writes (pp_en) are NEEDED; their absence
  leaves some interrupt source permanently active. Reverted.
- NB: S_POINTER read 0xe in the watch — the units probably did not accept the
  config without pp at all (or the executer that became active set them itself) — and the timeouts
  came faster. Not our path.

### Test 9: TASK_CON 0x6001 — did not help (even with the working base)
The run behaved no better (classification did not fit into 60 s; NB: my
"408 timeouts" counter was inflated by the iommu "stall timed out" lines — a dirty
metric). Reverted to upstream 0x7001. Also: a panic "Fatal exception in
interrupt" — our handler read MMIO with the domain asleep; added a
`pm_runtime_get_if_in_use` guard (as in rk_iommu) in the hard handler.

### Findings from midgy's vendor RFC (mail-archive, dri-devel)
RFC v2 (29.05) → v3 (04.06) → v4 (12.06.2026), "accel: rocket: Add RK3568 NPU
support", 9 patches: per-SoC data; **SCMI/PVTPLL compute clock**; reset before
detach; **keep the IOMMU domain attached between tasks**; bindings+dts;
domain-supply in pmdomain. Dependencies: (1) per-device ops in rockchip-iommu
(Simon Xue) — to describe the NPU MMU as v1 "rockchip,iommu" (32-bit DTE,
GFP_DMA32) while the rest are v2; (2) **"iommu/rockchip: disable fetch dte time
limit" — AUTO_GATING bit31** (in BSP: `rk_iommu_enable()`:
"Workaround for iommu blocked, BIT(31) default to 1").
midgy's status (July, msg622858): everything visible in software matched byte-for-byte, all 5
units engage (S_STATUS=0x0c, S_POINTER=0x1000e), but wt_rd=0 — CNA does not fetch
weights; the question to Rockchip remained without a public answer.
The kiln project (gahingwoo): the vendor rknpu.ko on mainline 7.1 WORKS on RK3576
(LLM+vision); the RK3568 path is implemented but NOT verified on hardware. kiln also has
the GFP_DMA32 patch for v2 ops (our gate #1) and a capture/env-trace toolkit.

### Test 10/11: enabling clk_scmi_npu (SCMI id 2, TF-A/PVTPLL) — PROGRESS
TEST in rocket_core_init: of_clk_get_from_provider(scmi,2) + prepare_enable
(+set_rate 600 MHz). For the first time REAL pipeline interrupts appeared:
raw=0x4 (CNA_WEIGHT_0 done) on some of the mobilenet ops. Without the clock — never.
Counters (window 0x8000, vendor amount registers 0x8034/38/3c — read
safely): WT_RD up to 0x80, DT_RD ~0xaf, DT_WR always 0.

### Decoding the "empty" IRQs and "bit16"
- The correct NPU MMU map: PFA=0x0c, RAWSTAT=0x14, MASK=0x1c, INTST=0x20,
  AUTO_GATING=0x24 (in the first measurements I had a +4 offset).
- During tasks, **NPU-IOMMU page faults at MAPPED addresses** are caught
  (PFA inside the regcmd page, steps 0x80/0x100) — pseudo-faults of the walker
  due to the "fetch dte time limit". rk_iommu silently swallows them
  (pm_runtime_get_if_in_use → IRQ_NONE). These were the "empty" IRQs with PC raw=0.
- PC INT_RAW bit16 — a real maskable RK3568 interrupt (probably
  "PC finished fetch/dispatch"), neither completion nor garbage.

### Test 12: AUTO_GATING bit31 via a 1 kHz "poker" — INVALID/worse
Constant writes into AUTO_GATING during operation themselves silence the walker
(WT_RD=0, faults across the whole regcmd page). Discarded as a method.

### Test 13: kept-domain (RFC 5/9) + clean bit31 after attach — PROGRESS
rocket: the domain is kept attached between tasks (kref), the detaches from
handle_irq/reset removed (this also removed the __iommu_group_set_core_domain WARN storm
and "Enable stall timed out"); right after attach rocket itself writes
AUTO_GATING|=BIT(31) (mmu_iomem = ioremap(0xfde4b000)).
Result: **conv2d.tflite engages for the first time** (CNA_S=CORE_S=0x5,
TASK=0xf000, WT_RD=0x30, DT_RD=0xaf, no faults). DPU does not write (DT_WR=0).

### Test 14: + vendor values of 7 registers (RKT_VENDOR_OVR=1) — ALMOST
In mesa (branch rk3568-test-session-20260820, e38c2fa8d3b) — an env-switchable
substitution of values from the MR's byte-exact table for this same conv2d:
CONV_CON2=0x70, CONV_CON4=0, CBUF_CON1=0x500028, DMA_CON0=0x171c07,
DMA_CON1=0x50, DMA_CON2=0x1900, 0x40c0=0x6400.
Result: **CNA FULLY completes both DMA stages: WT_RD=0x1900 (exactly
the vendor value!), DT_RD=0x3289 (vendor 0x32c9), the interrupts
CNA_FEATURE_0 (0x1) and CNA_WEIGHT_0 (0x4) arrive.** DT_WR=0, no DPU int,
timeout. Remainder: the DPU/ACCU/DPU_RDMA values in mesa are still RK3588-flavored,
their vendor list is not published in the MR (only the 0x40c0 example).

## SESSION SUMMARY: three kernel gates found and removed
1. **rk_iommu v2 page tables >4 GB** (the walker is 32-bit) — proven by reading
   DTE_ADDR (0x1_ad38f000); workaround mem=4G; the proper fix — GFP_DMA32 in
   iommu_data_ops_v2 (kiln 0003) or v1-compatible for the NPU MMU (midgy's path).
2. **clk_scmi_npu (SCMI 2, PVTPLL) not enabled** — mainline does not touch it;
   without it there are no pipeline interrupts at all.
3. **AUTO_GATING bit31 (DISABLE_FETCH_DTE_TIME_LIMIT)** — the BSP always sets it
   in rk_iommu_enable; without it the walker aborts walks on the time limit →
   silent faults at mapped addresses → fetches die.
Plus: keep the domain attached (RFC 5/9), reset before detach (RFC 4/9),
pm_runtime guard in the IRQ handler.

The state reached is FURTHER than what is publicly known (midgy has wt_rd=0):
CNA fully reads weights and features with vendor counters. The wall is only
the DPU output stage, and everything points to wrong RK3588 values of the DPU/ACCU/
RDMA registers in mesa (limitation A from the MR), not to the kernel.

## Next steps (by cost)
1. Obtain the vendor DPU/ACCU/RDMA values for conv2d: bring up the vendor
   rknpu.ko on mainline m1 by the kiln recipe (driver/ + their DT approach + mem=4G
   instead of their iommu patch) and take a capture (kiln capture/run-capture.sh).
   The kiln RK3568 path is not verified on hardware — we can become the first test.
2. Or/and: rebuild the Armbian kernel with 2 patches (GFP_DMA32 v2 ops;
   AUTO_GATING bit31 in rk_iommu_enable) — removes mem=4G and the /dev/mem hacks.
3. On DPU success — write up: the kernel part (scmi clk in the rocket variant,
   kept-domain, resets) + the mesa part (per-SoC values) for an upstream dialogue
   with midgy/Tomeu (with the user's permission).

## Machine state at the end of the session
- m1: kernel 7.1.7, overlay v3 active; mem=4G REMOVED (8 GB returned);
  the rocket module is NOT loaded after the final reboot; mesa on branch
  rk3568-test-session-20260820 (= MR + TEST-override, commit e38c2fa8d3b).
  To continue NPU work mem=4G (or the GFP_DMA32 kernel patch) is needed!
- The module in ~/npu-rk3568-test (m1 and droid, in sync): upstream + TEST:
  scmi clk, kept-domain, AUTO_GATING poke, irq diagnostics (all under /* TEST */).
- Scripts: npu-watch2/3.py (PC+CNA/CORE+counters 0x8034..3c), iommu-watch2.py
  (correct MMU offsets), ag-poker.py (DO NOT use — invalid),
  run1.py (quick single-model conv2d run, ~1.5 s).
- Hazards confirmed by the session: rmmod with power/control=on and/or after
  a WARN storm → machine check; reading /dev/mem with the domain powered off →
  SError/panic. Before rmmod — power/control=auto; before watchers —
  power/control=on.

---

## Session 2026-08-20, late evening: kernel 7.2-rc7 in a separate BE

Built kernel **7.2-rc7** (Armbian `BRANCH=bleedingedge`, rockchip64) with a single
userpatch — `GFP_DMA32` for rk_iommu v2 tables (taken from kiln, placed in
`userpatches/kernel/archive/rockchip64-7.2/`). The build ran on a cloud builder
Hetzner `cpx52` (x86, cross-compilation), ~32 min, the builder destroyed.
Packages — `/home/iav/npu-rk3568-test/debs-7.2/` (on droid).

### What the fresh kernel gives
- **Gate #3 (`AUTO_GATING` bit31) closed by upstream itself**: in 7.2
  `rk_iommu_enable()` unconditionally sets `DISABLE_FETCH_DTE_TIME_LIMIT`
  (in 7.1 there was only a `#define` of the address). Our workaround is no longer needed.
- **Gate #1** closed by our patch: `iommu_data_ops_v2.gfp_flags = GFP_DMA32`
  (in 7.2 upstream still has `0`).
- Gate #2 (`clk_scmi_npu`) remains TEST code in the module.

### Installation (BE)
`be-btrfs create 7.2-rc7` → `be-btrfs mount` → chroot → `dpkg -i` image/dtb/headers
→ symlink check → `be-btrfs activate 7.2-rc7` → reboot. The `/boot` symlinks
(`Image`/`uInitrd`/`dtb`/`vmlinuz`) were re-pointed consistently, the
`run-init` loops (the pitfall from the backlog) did not occur. The old BE `7.1.7` = **subvolid 3741**,
the new one — 4107.

### Pitfalls when swapping the kernel on m1
- **bcachefs — a DKMS module** (`bcachefs/1.39.2`), built only for the old kernel.
  After booting into the new BE `/media/nvme` did not mount → garage, docker,
  `/srv/netboot` and `tftpd-hpa` went down in a cascade. Cured by
  `sudo dkms install bcachefs/1.39.2 -k <new kernel>` (builds in ~3 min), then
  `modprobe bcachefs`, `systemctl start media-nvme.mount`.
- **`rocket.ko` does not build under 7.2 without an edit**: in 7.2 the field
  `num_rqs` was removed from `struct drm_sched_init_args` (the only change
  in upstream `rocket_job.c` between 7.1 and 7.2). Remove the line — it builds.

### State on 7.2 (8 GB, without mem=4G)
`insmod rocket.ko` → `/dev/accel/accel0`, the SCMI clock gets enabled (600 MHz),
no aborts. conv2d run: `invoke ≈ 507–524 ms`, the output is solid zero-point,
`TASK_STATUS` goes **0x5000 → 0xf000** (the "gate #1 removed" signature; before the workaround it was
0x7000). That is, the mem=4G behavior is reproduced with full memory.
The wall is the same and it is in userspace: DPU does not write the output (limitation A from mesa MR !42134).

---

## Session 2026-08-21: single task, cold session, regression on 7.2/8GB

### Test 15: how many tasks in the conv2d submit — ONE
- Hypothesis (from the public RK3576 findings: "real MACs only on the first
  task per power session"): our conv2d is cut into several tasks.
- Fact: `ROCKET_DEBUG=dump_bos` → a single `mesa-regcmd-000-000.bin`
  (1112 bytes = 139 uint64). Manual calculation by rkt_task.c agrees:
  entries_per_slice=10, input_banks=4, weights_banks=ceil(400/256)+1=3;
  3+1<8 → reuse_weights_cbuf=true; 4≤(8−3) → "full weights, full input" →
  1 task, kernel job.task_count=1.
- Conclusion: **conv2d is already a single task** — the hypothesis "the wall is in the task chain"
  does not apply to our symptom: for us even the FIRST task does not work.

### Test 16: cold power session (rmmod → npu off-0 → insmod → 1 run)
- CPU reference (seed 42): saturated output, 166 unique values
  (255×102918, 0×101531). File /tmp/out-cpu.npy on m1.
- Cold run RKT_VENDOR_OVR=1: output solid 128 (zero-point),
  timeout. BUT in dmesg: **WARN from upstream `WARN_ON(raw & DMA_READ_ERROR)`**,
  irq raw=0x00001000 (= PC_INTERRUPT_…_DMA_READ_ERROR), task_status=0xf000.
  The same raw=0x1000 was also in a warm run earlier in this boot.
- Conclusion: on 7.2/8GB the coldness of the session changes nothing; a DMA_READ_ERROR appeared,
  which on 7.1+mem=4G NEVER occurred (T5 unmasked all 17 bits).

### Test 17: watchers during a run (7.2, 8 GB, OVR=1)
- IOMMU: DTE=0x416d000 (<4GB — the GFP_DMA32 patch works), RAWSTAT=0 (no
  faults), AG=0x80000003 (bit31 from upstream is set).
- NPU: TASK_STATUS 0x5000→0xf000, but **CNA_S=0x0 the whole way, WT_RD=0**,
  DT_RD=0x87 (≈ the regcmd size of 139 words — looks like the PC fetch, not features),
  CNA_PTR/CORE_PTR=0xe.
- Conclusion: **on 7.2/8GB the Test 13/14 state does NOT reproduce** — CNA does not
  start at all, DMA_READ_ERROR. The "0x5000→0xf000 signature" was an incomplete
  metric. The regression is either from 8 GB (something else >4G) or from kernel 7.2.
  Discriminator: mem=4G on the same 7.2.

### Test 18: discriminator mem=4G on kernel 7.2 — GATE #1b FOUND
- Reboot m1 with `extraargs=cma=256M mem=4G` (backup /boot/armbianEnv.txt.pre-mem4g-72).
- The same run (OVR=1, watchers): **the Test 14 state fully returned** —
  CNA_S=CORE_S=0x5, WT_RD=0x1900 (vendor value), DT_RD=0x3289,
  interrupts CNA_WEIGHT_0 (0x4) and CNA_FEATURE_0 (0x1), NO DMA_READ_ERROR.
- Conclusion: **gate #1b — the BO data (shmem) is physically above 4 GB.** The GFP_DMA32 patch
  covers only the page tables; the buffers themselves are translated by the IOMMU to phys >4G, and
  this path on the RK3568 NPU gives DMA_READ_ERROR (a v2 PTE >4G either does not work in
  this MMU, or is not encoded that way by mainline). **The thesis "mem=4G is no longer
  needed" is REFUTED** — for NPU work mem=4G (or a shmem/DMA32 fix in rocket_gem)
  is mandatory. TASK_STATUS 0x5000→0xf000 is unusable as a "signature": it is the
  same with DMA_READ_ERROR too.

### Test 19 (the main one): a single task on a cold power session — NO MACs
- Canonical: control=auto → rmmod → `npu off-0` → insmod → ONE run.
- conv2d (80×80×16→40×40×128, 1 task, OVR=1): CNA interrupts 0x4, 0x1
  arrive, DPU — none; output solid zero-point 128 (unique=1), timeout.
  CPU reference with the same input (seed 42): 166 unique values.
- Tiny convolutions (generated on droid, TF 2.21 + tf-keras legacy path
  of mesa: tfwork/gen_tiny.py): conv-tiny-1x1 (8×8×4→8×8×8, 1×1) and conv-tiny-3x3
  (8×8×4→8×8×8, 3×3), both uint8, per dump_bos — 1 task each (1112 bytes regcmd).
  Cold runs without OVR: both — solid 128, timeout; 1x1 gave CNA_WEIGHT_0
  (0x4), 3x3 — not a single pipeline interrupt.
- Conclusion: **the hypothesis "the wall is in the task chain" for RK3568 is REFUTED**:
  the task is already single, the session is cold — MACs do not happen. Our wall ≠ the
  RK3576 wall (for them the first task of a session COMPUTES, the subsequent ones break; for us
  even the first does not compute). Everything still points to limitation A of mesa MR !42134
  (DPU/ACCU/DPU_RDMA values RK3588-flavored): CNA is fed completely,
  DPU does not start (DT_WR=0, no DPU int) in any configuration.

## Machine state at the end of session 2026-08-21
- m1: kernel 7.2.0-rc7 (BE 7.2-rc7), 8 GB returned (mem=4G removed, the backup
  .pre-mem4g-72 remained in /boot); the rocket module unloaded; systemctl --failed
  is empty; garage/docker/nvme work. **For NPU work mem=4G is needed again!**
- New on droid: ~/npu-rk3568-test/tfwork/ — a venv with TF 2.21 + gen_tiny.py
  (generation of quantized tflite by the mesa recipe); conv-tiny-{1x1,3x3}.tflite
  copied into the project root; run2.py — a deterministic runner
  (seed 42, NPU/CPU, saves .npy) — also present on m1 (~/npu-rk3568-test/).
- References/run outputs on m1: /tmp/out-*.npy (will disappear on reboot).

---

## Session 2026-08-21 (continued): formulas from DISASM-LOG into mesa

Preparation: m1 returned to `mem=4G` (extraargs, backup `/boot/armbianEnv.txt.bak-t20`),
kernel 7.2.0-rc7. conv2d.tflite recreated on droid with the same generator
(tfwork/gen_conv2d.py: 80×80×16→40×40×128, 5×5, s2, seed 7) — the old one lived in /tmp
and vanished; the weights were quantized anew, hence a new CPU reference (seed 42):
**149 unique** values (255×102916, 0×101570), /tmp/out-cpu.npy on m1.

### Test 20: baseline reproduced (= Test 14/18)
OVR=1, warm session: CNA_S=CORE_S=0x5, WT_RD=0x1900, DT_RD=0x3289,
IRQ raw 0x4 (CNA_WEIGHT_0) and 0x1 (CNA_FEATURE_0), DT_WR=0, no DPU int,
timeout, output solid 128 (unique=1). We have a reference point.

### Test 21: formula DPU_SURFACE_ADD = Wout×Hout (mesa commit 1)
rkt_task.c: `surfaces_per_row = output_width * output_height` (the ×2 removed).
NB: the vendor value 0x6400 was ALREADY in the OVR table of Test 14 — i.e. the DPU
with the correct SURFACE_ADD had already been tested; as expected NO CHANGES:
CNA complete, DT_WR=0, output zero-point.

### Test 22: CNA line/surf stride formulas (mesa commits 2–3)
rkt_task.c: `input_line_stride = calc_line_stride(Win)/FEATURE_ATOMIC_SIZE`
(=Win), `input_surface_stride = line_stride*Hin` (=Win×Hin); the OVR table
reduced to the 4 non-formula registers (CONV_CON2, CONV_CON4, CBUF_CON1,
DMA_CON0). Run: **the Test 14 state is fully reproduced by the formulas**
(CNA_S=0x5, WT_RD=0x1900, DT_RD=0x328a, raw=0x5) — the formulas are correct throughout.
DPU is still silent.

### Audit of the DPU/ACCU/DPU_RDMA family (dump: mesa decode.py, 139 words)
All size values are already in vendor units (16-byte cells):
DST_SURF_STRIDE=1600, SURFACE_ADD=1600, cubes 39/39/127, WDMA 127/39/39.
There are NO more "extra ×2" candidates in the family. The other DPU registers are mode
registers (BS/BN/EW bypass, OUT_CVT scale/shift), they do not gate the start.
**Finding:** vendor DT_RD=0x32c9 versus our 0x3289 — a difference of 0x40
units ×8 bytes = exactly 512 bytes = the size of the bias buffer. In the vendor's case DPU_RDMA
(BRDMA) reads the biases, in ours — never. The wall refined: DPU_RDMA enabled
(op_en=1, BRDMA_CFG=data_use), but does not even begin the bias fetch.

### Test 23: CMAC OPERATION_ENABLE (0x2008) — no effect
mesa emits op_en for 4 of the 5 units (CNA/CORE/DPU_RDMA/DPU); CMAC (0x2000,
target 0x4) receives only the S_POINTER wake. Added emit 0x2008=1 —
behavior identical (probably the register is absent/ignored). Reverted.

### Test 24: DPU_RDMA burst_len 7 (like the vendor CNA burst) — FATAL
Hypothesis: mesa burst_len=15 in RDMA_FEATURE_MODE_CFG, while the vendor for CNA
uses 7, and CNA with mesa-burst=15 did not read to the end for us either (Test 13).
Result: **m1 died entirely** (SSH vanished at the moment of the run), watchdog reboot,
a second spontaneous reset during the first boot; a cold
`rockpi-power cycle` was needed. The value 15 in this field is apparently legal, while 7 breaks
the field encoding (not len-1?) → bus hang. Commit reverted.
**A new hazard for the collection: a wrong burst_len in DPU_RDMA = death of the bus.**

### Session summary
- The three DISASM-LOG formulas are put into mesa as formulas (3 commits in
  rk3568-test-session-20260820) and verified on hardware: they give the vendor
  values and a full CNA fetch without the corresponding OVR entries.
- DPU still does not start. The wall has narrowed: not the DPU size values
  (they are now the vendor ones), but the very start of DPU_RDMA/DPU — BRDMA does not read
  even the biases. Remaining unknowns: the 4 non-formula CNA registers
  are covered by OVR, but the vendor values of the DPU mode registers
  (FEATURE_MODE_CFG output_mode, BRDMA_CFG, RDMA FEATURE_MODE_CFG)
  are publicly unknown — a vendor capture is needed (kiln, step 1 of the "Next
  steps" of the previous session).

### Test 25 (control after the revert of Test 24, cold boot)
The baseline is back in place: CNA_S=CORE_S=0x5, WT_RD=0x1900, DT_RD=0x3289,
IRQ 0x4/0x1, DT_WR=0, output zero-point. The mesa branch at the end of the session:
3 formula commits (43b6638, 2ce088e) + shrink OVR (59a9aae); Test 23 and
Test 24 reverted with reverts (e086437, 734fe69). The working tree is clean,
teflon rebuilt. m1: mem=4G KEPT (memory 3.8 GiB!), rocket unloaded,
control=auto, systemctl --failed is empty, /media/nvme mounted.
Next — vendor capture (rknpu.ko/kiln), led by the user.

## Session 2026-08-21 (continued): THE VENDOR STACK COMPUTES ON m1

### Test 26: vendor rknpu.ko + librknnrt on mainline 7.2 — WORKS
Built the kiln path (github.com/gahingwoo/kiln), which its own authors mark
as "implemented, NOT yet tested on real hardware" for RK3568:
- vendor rknpu v0.9.8 (armbian linux-rockchip rk-6.1-rkr6.1) + kiln-mainline
  shims built out-of-tree against 7.2.0-rc7 (`~/kiln-build/kiln`), cleanly;
- overlay `rk3568-rknpu-vendor.dts` (compatible `rockchip,rk3568-rknpu`,
  window 0x10000, clocks clk/aclk/hclk, rknpu-supply, no pm_qos set);
- userspace: librknnrt 2.3.2 + a ready-made `mobilenet_v1.rknn` for RK3566_RK3568
  from the airockchip repository + our own runner `vendor-blob/rknn_run.c`.

probe: `[drm] Initialized rknpu 0.9.8 ... on minor 2`, /dev/dri/renderD129.
(The harmless `can't request region ... 0xfde40000-0xfde4ffff` is an overlap
with the iommu@fde4b000 node; the driver ioremaps it anyway.)

**Run: `rknn_init=0`, `rknn_run=0`, argmax=905, probabilities non-zero.**
That is, the RK3568 NPU REALLY COMPUTES under the mainline 7.2 kernel.

Consequence: hardware, DT, power domain, IOMMU and clocks are fine; the rocket
wall is entirely in userspace/regcmd. The RK3576 authors' conclusion ("arm — an
internal cold-start RTL state") does NOT carry over to RK3568. Incidentally this
is the first test of kiln's RK3568 path on hardware (we will not publish it).

### Test 27: vendor regcmd captured (133 registers, task 0 of mobilenet conv1)
Patch `capture/rknpu-regcmd-dump.patch` + `driver/patches/add-regcmd-dump.py`,
`echo 1 > /sys/module/rknpu/parameters/rknpu_dump_regcmd`.
Raw data: `vendor-blob/regcmd-mobilenet-task0.txt`, decode —
`vendor-blob/regcmd-mobilenet-decoded.txt`.
Target map confirmed via `0x40c0=0x31000` (SURF_ADD=12544=112×112):
0x02→CNA 0x1000, 0x04→CMAC 0x2000, 0x08→CORE 0x3000, 0x10→DPU 0x4000,
0x20→DPU_RDMA 0x5000 — i.e. the mesa layout is correct.

**Divergences vendor vs mesa (mode registers, independent of geometry):**
| Register | Vendor | mesa |
|---|---|---|
| CMAC 0x200c | 1 | not written (register absent from registers.xml) |
| CORE_MISC_CFG 0x3010 | 0x3e006f (SOFT_GATING=56, DW_EN=1, QD_EN=1) | QD_EN(1) |
| DPU_DATA_FORMAT 0x4010 | 0xe0 (BS_MUL_SHIFT_VALUE_NEG=14) | 0 |
| DPU_BS_CFG 0x4040 | 0x148 (BS_MUL NOT bypassed, ALU_ALGO=0) | ALU_ALGO=2 + MUL_BYPASS |
| DPU_BS_MUL_CFG 0x4048 | 0xe01 (SHIFT=14, SRC=1) | 0 |
| DPU_BN_CFG 0x4060 | 0x92 (BN_BYPASS NOT set, RELUX_EN=1) | BN_BYPASS=1 |
| DPU_BN_RELUX_CMP_VALUE 0x406c | 40144 | 0 |
| DPU_DST_SURF_STRIDE 0x4024 | 6272 = Wout×Hout/2 | Wout×Hout |
| DPU_RDMA_BRDMA_CFG 0x501c | 0xe → BRDMA_DATA_USE=7 | =1 (0x2) |
| DPU_RDMA 0x5024 | 0x1f | not written |
| DPU_RDMA_NRDMA_CFG 0x5028 | 1 | 0 |
| DPU_RDMA_FEATURE_MODE_CFG 0x5044 | 0x4000 (BURST_LEN=8, MRDMA_DISABLE=0) | 0x7810 (BURST_LEN=15, MRDMA_DISABLE=1) |

Confirmed on the second convolution: CNA_DMA_CON0 burst 7/7 is a constant, not
a fit (FETCH_PIXEL_LEN is geometric: 28 at Win=224).
NB: Test 24 killed the board by setting burst_len=7 in DPU_RDMA; the vendor sets 8 there.
CNA_CONV_CON2: FEATURE_GRAINS=5 at 224×224×3 3×3 s2 (ours is 7 at 80×80×16
5×5 s2) — the formula is derivable, we now have numbers for two points.

---

## Test 28 (2026-08-21): the entire vendor regcmd is readable straight from the `.rknn` — offline, no hardware

**Finding.** The command stream that librknnrt uploads to the NPU lives entirely
inside the model container. The live dump of task 0 from Test 27 matched the
contents of `mobilenet_v1_rk3568.rknn` (offset 46016) **word for word, except four**:

| idx | block | register | in file | in live dump |
|---|---|---|---|---|
| 27 | CNA | `0x1070` FEATURE_DATA_ADDR | 0 | `0xff9f5000` (weights) |
| 36 | CNA | `0x1110` DCOMP_ADDR0 | 0 | `0xffdade00` (features) |
| 69 | DPU | `0x4020` DST_BASE_ADDR | 0 | `0xffa3ec00` (output) |
| 119 | DPU_RDMA | `0x5020` BS_BASE_ADDR | 0 | `0xfffe0880` (bias) |

The runtime patches exactly these four addresses. Everything else is a model
constant. So the reference for any network can be taken **without hardware and
without risk to the board**.

Tool: `tools/rknn_regcmd.py` (extraction, slicing into tasks, decode via
`vendor-blob/regnames.json`, `--table` mode with geometry).
Dumps: `vendor-blob/regcmd-mobilenet-all.txt` (full decode of 51 tasks),
`regcmd-mobilenet-table.txt`, `regcmd-resnet18-table.txt`.

**mobilenet_v1 = 51 tasks, 6729 words**, resnet18 = 23 tasks, 3425 words.

### Correction to Test 27

Task 0 is **not the whole first convolution but a band along the height**:
`DATAIN_HEIGHT=127`, `DPU DATA_CUBE_HEIGHT=62` out of 112. The vendor slices the
layer into bands (task 0: rows 0–62, task 1: 63–111). The "vendor task vs whole
mesa layer" comparison in Test 27 was a comparison of a band with a full tensor —
some conclusions from there are refined below.

### Formulas confirmed on 44 convolution tasks (mobilenet) + 23 (resnet18)

| Register | Formula | Check |
|---|---|---|
| `CNA_DATA_SIZE3` DATAOUT_ATOMICS | `Wout × Hout_of_band` | 44/44 |
| `DPU_DATA_CUBE_WIDTH` | `Wout − 1` | 44/44 |
| `CNA_DMA_CON2` SURF_STRIDE | `Win × Hin_of_full_tensor` | 38/44 (exceptions — ARGB input) |
| `CNA_DMA_CON1` LINE_STRIDE | `Win` | 42/44 (exceptions — ARGB input, there 84 = 224×3/8) |
| `CNA_DMA_CON0` | constant `0x00171c07` | 44/44 — **FETCH_PIXEL_LEN=28 is not geometric** |
| `DPU_DST_SURF_STRIDE` | `Wout × Hout_full / 2` | 44/44 (full tensor, not band) |
| `DPU_RDMA 0x5024` | `Cout − 1` | 10 distinct values, all matched |

**`CNA_CONV_CON2` FEATURE_GRAINS = `kh + strY·(L−1)`**, where L is how many
output rows the vendor computes per CBUF pass. L=2 for long rows, grows to 3–4
for short ones (Win≤14). Points: k1s1→2, k3s1→4, k3s2→5, k5s2→7,
k7s2→9 — all at L=2. At Win=14: k3s1→5, k3s2→7; at Win=7: k3s1→6.
**mesa computes `strY + kh + 1` — exactly one more than the vendor's L=2.**

**`CNA_CONV_CON2` KERNEL_GROUP = 0 in all 51 tasks**, including layers with 512 and
1001 kernels. mesa writes `kernels/32 − 1`, i.e. 15 for 512 kernels and `-1`
(wrapped into unsigned) for depthwise, where `kernels=1`.

### What is genuinely mode-related for the vendor (constant across all 44 tasks)

`CNA_S_POINTER/CMAC 0x2004/CORE_S_POINTER/DPU_S_POINTER/RDMA_S_POINTER = 0xe`,
`CNA_CVT_CON4 = 0x10000`, `CNA_DMA_CON0 = 0x171c07`, `DPU_EW_CFG = 0x383`,
`DPU_EW_CVT_SCALE_VALUE = 1`, `RDMA_NRDMA_CFG = 1`, `RDMA_ERDMA_CFG = 1`,
`RDMA_WEIGHT = 0x01010101`. Plus 70 registers that are always zero.

### What is switched by convolution type (and not a "mode", as Test 27 assumed)

| Register | regular (PW/ARGB) | depthwise |
|---|---|---|
| `CMAC 0x200c` | 1 | **3** |
| `CORE_MAC_GATING 0x300c` | 0 | **2** |
| `DPU_FEATURE_MODE_CFG 0x400c` | `0x108` | **`0x138`** (+CONV_MODE=3) |
| `DPU_RDMA_FEATURE_MODE_CFG 0x5044` | `0x4000` (BURST_LEN=8) | **`0x4006`** (+CONV_MODE=3) |

`DPU_DATA_FORMAT=0xe0`, `BS_CFG=0x148`, `BS_MUL_CFG=0xe01`, `BN_CFG=0x92`,
`BRDMA_CFG=0xe` (BRDMA_DATA_USE=7), `PC_OPERATION_ENABLE=0x1f` — identical across
all 43 convolution tasks and change only on the final FC task.

### Prime suspect for the bias-fetch that never happened

`DPU_RDMA 0x5024` — the register **is absent from mesa's `registers.xml`**, mesa
does not write it at all, while for the vendor it is `Cout − 1` for every task.
It is the channel counter of the bias stream; without it BRDMA has nothing to
read, which matches our `DT_RD` being smaller than the vendor's by exactly 512
bytes (the size of the bias buffer).

---

## Test 29 (2026-08-21): the RK3568 NPU finished a task under mainline `accel/rocket` for the first time

Reference convolution `conv2d.tflite` (80×80×16 → 40×40×128, 5×5 s2):

```
invoke: 4.6 ms                       (was 516 ms and "NPU job timed out")
irq raw=0x00000150 status=0x00000150 task_status=0x0000f000
DT_WR=0x6400  DT_RD=0x3309  WT_RD=0x1900
```

`0x150` = `DPU_0 | CORE_0 | CNA_CSC_0` — the whole pipeline reported. `DT_WR`
25600 × 8 bytes = exactly the size of the output tensor. No timeouts in dmesg.

Method: mesa was taught to dump its stream (`RKT_DUMP=1`, patch in `emit_raw`), and
it is compared with the vendor's from the `.rknn` register by register on **the
same model**. Fixes are then derived from the diff rather than guessed.

### What fixed the wall

1. **The CORE block map in `registers.xml` is shifted by one slot.** The xml has an
   extra `MAC_GATING` at `0x300c`, due to which `MISC_CFG`/`DATAOUT_SIZE_0`/
   `DATAOUT_SIZE_1` moved 4 bytes up. mesa wrote `QD_EN` where the hardware reads
   `DATAOUT_SIZE_0` — CORE got an output of width 3 and height 1.
   The real layout: `0x300c` MISC_CFG (0, or `DW_EN=2` for depthwise;
   the vendor does not set `QD_EN`), `0x3010` DATAOUT_SIZE_0 `((H−1)<<16)|(W−1)`,
   `0x3014` DATAOUT_SIZE_1 `Cout−1`, `0x302c` = 0.
   Independent confirmation: `CMAC 0x200c` — the same slot within its own block —
   equals 1 normally and 3 (`QD_EN|DW_EN`) exactly in depthwise tasks.
2. **`CMAC MISC_CFG 0x200c`** mesa did not write at all.
3. **`DPU_RDMA 0x5024 = Cout−1`** — the register is absent from the xml; without it
   the bias-RDMA has nothing to read. (The first attempt via `EMIT()` missed the block:
   `rkt_get_target()` returned 0, the word went out with target `0x1`. `emit_raw()`
   with `DPU_RDMA | 0x1` is needed.)

### Formulas that replaced the OVR hack

| Register | mesa had | Vendor |
|---|---|---|
| `CNA_DMA_CON0` | `burst 15/15, FETCH_PIXEL_LEN=Win` | constant `0x00171c07` (7/7, 28) |
| `CNA_CONV_CON2` FEATURE_GRAINS | `kh + strY + 1` | `kh + strY` |
| `CNA_CBUF_CON1` | only `DATA_ENTRIES` | `(Win << 16) | entries` |
| `CNA_CONV_CON4` | `Win×Hin×Cin` always | 0 outside ARGB |
| `CNA_CVT_CON0` | `0xb` (`CVT_BYPASS=1`) | `0xa` — the converter is not bypassed |
| `DPU_DST_SURF_STRIDE` | `Wout×Hout` | `Wout×Hout / 2` |

After these `RKT_VENDOR_OVR` is no longer needed: without overrides the same 4.6 ms.

### What is still wrong

The numbers do not agree with the CPU. On the reference convolution 101,744 of
204,800 match, but the output is almost binary (0 or 255) — the DPU requantization
chain is still only half vendor-like: `DATA_FORMAT` (vendor `0xe0`, mesa 0),
`BS_CFG` (`0x148` vs `0x20150`), `BS_MUL_CFG` (`0xe01` vs 0), `BN_CFG`
(`0x92` vs `0x53`), `BN_RELUX_CMP_VALUE`, `BRDMA_CFG` (`0xe` vs `0x2`),
`NRDMA_CFG` (1 vs 0), `RDMA_FEATURE_MODE_CFG` (`0x4000` vs `0x7810`).
The vendor pushes per-channel bias and scale through BRDMA/NRDMA, mesa makes do
with a single common `OUT_CVT_SCALE` — that is no longer a register fix but a
different data layout.

The whole mobilenet_v1 does not pass yet: some tasks still time out (the first
layer in ARGB mode mesa does not enable — its `CONV_CON1` is 0 versus the vendor's
`0xa000`, and the condition `input_channels_real == 1` never fires for a
three-channel input; depthwise is unverified).

Commits in `/media/nvme/mesa`, branch `rk3568-test-session-20260820`:
`693c157f708`, `1364c60ea5c`.

---

## Test 30 (2026-08-21): full offline diff mesa vs vendor across all of mobilenet_v1

Tool `tools/rknn_diff.py`: the vendor stream from the `.rknn`, the mesa stream from
`RKT_DUMP=1`; tasks are matched not by number (the two sides slice into bands
differently — vendor 51 tasks, mesa 50) but by layer signature
`(kw, kh, kernels, strides, Win, Cin, Wout)`. 11 layers matched; divergences
are sorted into three classes — band-related (expected), address-related
(meaningless) and real. Dump: `vendor-blob/diff-mesa-vs-vendor-mobilenet.txt`.

### Real divergences, by decreasing impact on the numbers

| # | Register | Vendor | mesa | What it is |
|---|---|---|---|---|
| 1 | `CNA_PAD_CON0` bits 16–31 | `0xff80` | 0 | **padding value −128**; mesa pads with zero instead of the zero-point |
| 2 | `CNA_CONV_CON2` KERNEL_GROUP | 0 always | `kernels/32−1`, for depthwise `0xff` (underflow) | |
| 3 | `CNA_CBUF_CON1` DATA_ENTRIES | twice mesa's | | CBUF window size |
| 4 | `CNA_CBUF_CON0` | `0x17` (weight 1, data 7) | `0x26` (2, 6) | CBUF bank split |
| 5 | `CNA_DMA_CON0` | `0x00171c07` | `0x00071c07` | the vendor sets bit 20, which the xml marks RESERVED |
| 6 | `DPU_DATA_FORMAT` | `0xe0` (`BS_MUL_SHIFT_VALUE_NEG=14`) | 0 | |
| 7 | `DPU_BS_CFG` | `0x148` (BS_MUL active, ALU_ALGO=0) | `0x20150` (ALU_ALGO=2, MUL_BYPASS) | |
| 8 | `DPU_BS_MUL_CFG` | `0xe01` (shift 14, SRC=1) | 0 | |
| 9 | `DPU_BS_OW_CFG` | `+OW_SRC=1` | without it | |
| 10 | `DPU_BN_CFG` | `0x92` (BN in the path, RELUX_EN) | `0x53` (BN_BYPASS) | |
| 11 | `DPU_BN_RELUX_CMP_VALUE` | per-layer value | 0 | ReLU6 threshold |
| 12 | `DPU_OUT_CVT_SCALE/SHIFT` | own value | own, different | consequence of 6–11 |
| 13 | `RDMA_BRDMA_CFG` | `0xe` (DATA_USE=7) | `0x2` (=1) | |
| 14 | `RDMA_NRDMA_CFG` | 1 | 0 | BN stream channel |
| 15 | `RDMA_FEATURE_MODE_CFG` | `0x4000` / `0x4006` (dw) | `0x7810` / `0x7816` | BURST_LEN 8 vs 15, MRDMA not muted |
| 16 | `PC 0x0000` and `PC_OPERATION_ENABLE` | writes 0 and `0x1f` | not written | the vendor enables five units with one write |
| 17 | `CNA/CORE/DPU/RDMA OPERATION_ENABLE` | not written | 1 into each | the flip side of item 16 |
| 18 | `CNA 0x1210…0x1230` | nine zeros | not written | |
| 19 | `CNA_CVT_CON5`, `CNA_PAD_CON1`, `DPU_WDMA_SIZE_0/1`, `RDMA_EW_SURF_NOTCH` | not written | written | extra mesa writes |
| 20 | `RDMA 0x5030`, `0x503c` | zeros | not written | |

Items 6–12 are one big difference of approach: the vendor computes per-channel
requantization through BS (bias + multiply with shift 14) and BN (ReLU-X), pulling
the coefficients in via BRDMA/NRDMA streams; mesa bypasses all of that and scales
with a single common `OUT_CVT_SCALE`. That is no longer a register fix but a
different data layout in memory.

### First layer (ARGB) and depthwise

Pairs for these did not match: the vendor encodes the input as 8 channels
(`DATA_SIZE1 = 0x00020008`, `CONV_CON1 ARGB_IN=10`, `LINE_STRIDE = Win×3/8 = 84`,
`CVT_CON0..3 = 0xe38e1/0x4000ff80`), mesa as 16 without ARGB mode: its condition
`input_channels_real == 1` never fires for a three-channel input.
Large pointwise layers the vendor additionally splits by kernels (1024 → 448 + 192 +
…), mesa takes them whole.

---

## Test 31 (2026-08-21): depthwise started working; the wall narrowed to the first layer

The comparison method has reached single layers: `tfwork/gen_layers.py` makes
tflite of vendor shapes with **a sane weight spread** (`gen_tiny.py` had
σ=127, because of which both NPU and CPU produced an almost binary output — comparison
was pointless). Plus per-task logging is enabled in the driver:
`echo "file rocket_job.c +p" > /sys/kernel/debug/dynamic_debug/control` —
`Submitted regcmd at <addr>` appears in dmesg, and it is visible on exactly which
task the pipeline stalls.

### Three fixes

1. **`rkt_task.c` doubled `output_channels` for depthwise** with ≤32 channels and
   aligned to 64. The vendor writes the real 32 (`DPU_DATA_CUBE_CHANNEL =
   0x001f001f`), while mesa declared 64 channels to the DPU — it waited for data that
   would never come and stalled right after CNA. The trick is apparently needed on
   RK3588; on RK3568 it breaks things. **Depthwise layers pass after this.**
2. **The padding value** lives in the upper half of `CNA_PAD_CON0`, not in a
   separate `PAD_CON1` (`0x1184`), which the vendor does not write at all. mesa
   computed the number correctly (`input_zero_point − 0x80`) but put it in the wrong
   place — i.e. every padded convolution padded its edges with zero instead of the zero-point.
3. **`CONV_CON2` KERNEL_GROUP = 0** always (mesa had `kernels/32−1`, for
   depthwise that is `0xff`). Plus `CBUF_CON1` DATA_ENTRIES twice as large.

Single layers of vendor shapes — depthwise 3×3 s1 and s2 on 112×112×32, regular
3×3 on 56×56×32, the reference 80×80×16 → 40×40×128 — run **without timeouts**.
On `layer-conv-56` the mean absolute deviation from the CPU is **8.25** (was 140).

### Where the wall is now

The whole mobilenet_v1 still gives 28 timeouts, but the cause is a single one:
**the second band of the first layer** (input 224×224×3) fails, after which the
driver does not reset the hardware and times out everything remaining. The isolated
`layer-first-rgb.tflite` reproduces this exactly:

```
Submitted regcmd at 0x252000   irq 0x004, 0x001, 0x150   ← band 0 went through
Submitted regcmd at 0x252440   irq 0x008, 0x002          ← band 1: CNA only
NPU job timed out
```

Ping-pong has nothing to do with it: for depthwise band 1 on the same second bank
gives `0x2a0` (`DPU_1|CORE_1|CNA_CSC_1`) and completes. The slicing is correct too — 5 bands
of 26 output rows each, offsets exactly 52 input rows apart. What remains is the
configuration of the RGB input itself: the vendor runs the first layer in ARGB mode as 8 channels
(`CONV_CON1 = 0xa000`, `DATA_SIZE1 = 0x00020008`, `LINE_STRIDE = 84 = Win×3/8`,
`CVT_CON0 = 0xe38e1`, `CVT_CON1..3 = 0x4000ff80`, `CONV_CON4 = 256032`), while
mesa's condition `input_channels_real == 1` never fires for a three-channel
input — it takes 16 channels and `LINE_STRIDE = 224`.

Commit `cad7109cfbe`.

---

## Test 32 (2026-08-21): the wall is not ARGB but multi-band regular convolutions

The hypothesis "the first layer fails because it is RGB" is **refuted**: the same layer
with a genuine 16-channel input (`layer-first-16ch.tflite`, 224×224×16 →
112×112×32, 3×3 s2) fails in exactly the same way. The big rework of the input layout for
ARGB is not needed.

The real boundary was found by sweeping shapes:

| model | shape | tasks | result |
|---|---|---|---|
| `layer-conv-56` | 56×56×32, 3×3 s1, regular | 1 | **clean** |
| `conv2d` | 80×80×16 → 40×40×128, 5×5 s2 | 1 | **clean** |
| `layer-dw-112` | 112×112×32 dw 3×3 s1 | 3 | **clean** |
| `layer-dw-s2` | 112×112×32 dw 3×3 s2 | 2 | **clean** |
| `layer-conv-s2-112` | 112×112×32 → 56×56×32, 3×3 s2, regular | 2 | timeout on band 1 |
| `layer-pw-112` | 112×112×32 → 112×112×64, 1×1 | 3 | timeout on band 1 |
| `layer-first-16ch` / `-rgb` | 224×224 → 112×112×32, 3×3 s2 | 5 | timeout on band 1 |

That is, **multi-band jobs pass for depthwise and fail for regular
convolutions**. One failed task brings down the entire remainder of the model: the driver
does not reset the hardware, and everything after that times out in a row — hence 28 timeouts on mobilenet
with a single real breakage.

What is visible in the hardware on band 1: `CNA_S = CORE_S = 0x10008`
(the second ping-pong slot is occupied and never completes), `DT_WR` = 77,952 of 100,352
bytes — the DPU wrote part of the output and stalled.

### What was checked and ruled out

- **Ping-pong.** For depthwise band 1 on the same second bank gives `0x2a0`
  (`DPU_1|CORE_1|CNA_CSC_1`) and completes.
- **Address alignment.** For the passing depthwise band 1 is aligned to
  1 KB, for the failing convolution to 2 KB.
- **`WEIGHT_REUSE`.** Disabling it does not help; the vendor sets exactly the same
  bit on second bands (`CBUF_CON0` = `0x0017` on band 0 and `0x2017` on
  band 1 — mesa matches).
- **Band geometry.** For the failing convolution and the passing depthwise of the same
  shape it is identical down to the number: `Hin` 64/50, `atomics` 1736/1400, `dpuH`
  30/24, `entries` 56 — and matches the vendor's.
- **`SURFACE_ADD`.** The vendor writes `Wout×Hout×16` for regular convolutions and
  `×32` for depthwise; mesa reproduces both cases correctly.

### A regression worth remembering

Substituting the vendor's `RDMA_BRDMA_CFG = 0xe` (`DATA_USE=7`) and
`RDMA_NRDMA_CFG = 1` **broke even what used to work** — a timeout appeared on
all layers. These bits enable additional streams (bias and BN), which simply do
not exist in mesa's memory layout. RDMA registers cannot be copied without the vendor's
data layout. Reverted.

### Side note

Removed the `banks++` in `calc_weights_banks()` (the comment there itself admitted the
calculation may be wrong for this hardware). After this mesa outputs
`CBUF_CON0 = 0x17` and `CBUF_CON1 = 0x00e00038` — byte for byte as the vendor.
Commit `ab0f3b21fe8`.

Numbers on the single-task 56×56×32 convolution unchanged: mean absolute deviation
from the CPU 8.24, exact matches 50,171 of 100,352.

---

## Test 33 (2026-08-22): CBUF is twice as large as mesa thought

The "multi-band regular convolutions" wall was not broken through but **bypassed**: almost
all layers do not need slicing at all.

**Finding.** `CBUF_BANK_SIZE` in `rkt_ml.h` = 32768. The vendor stream says
otherwise: the first convolution of mobilenet_v1 (Win=224, 7 data banks) is sliced by the vendor
into bands of 127 and 98 input rows. At 14 entries per row the first band needs
128 slices, and that fits only if a bank holds 256 entries of 256 bytes,
i.e. **64 KiB**. A check on the second layer (56×56×64 pointwise, the vendor does not
slice at all) agrees. With half the capacity mesa sliced the same layer into five
bands, and layers that need no slicing into two or three.

**Second fix.** The slicing loop handed the task all available slices, which
produced an extra input row when the number of slices is not congruent to the kernel
height modulo the stride: mesa fed CNA 128 rows where 63 output rows
consume `(63−1)·2+3 = 127`. The vendor has exactly 127.

**Result.** Single layers of vendor shapes — pointwise 1×1 112×112×32→64,
regular 3×3 s2 on 112×112×32, depthwise 3×3 s1 and s2, regular 3×3 on 56×56×32,
the reference 80×80×16→40×40×128 — now **fit in one task and pass**.
Three of them previously required two or three bands and timed out.

Commit `5c1bfd4fa9e`.

### What remains and what was checked

The only layer that still needs slicing after the fix is the first one
(224×224 → 112×112×32, 3×3 s2, two bands). It still stalls on band 1,
and it is because of it that mobilenet gives 28 timeouts with a single real breakage.

The bands now match the vendor's: `Hin` 127/98, `atomics` 7056/5488,
`dpuH` 62/48, `CBUF_CON0` `0x0017`/`0x2017`, both sides start band 1 from
input row 126. The remaining divergences are only those common to the whole layer
(ARGB packing, CVT, DPU quantization chain, RDMA) — there are none in the band geometry.

Negative results of this round:

- **ARGB is irrelevant** (confirmed again): the same layer with a genuine
  16-channel input fails identically — 2 tasks, band 0 gives `0x150`,
  band 1 only `0x2`, then a timeout.
- **`WEIGHT_REUSE` is irrelevant.** With the bit off band 1 honestly
  re-reads the weights (`irq 0x8`), but CORE still does not start.
- **The output offset cannot be halved.** The vendor's is half as large
  (56448 versus 112896), but substituting it broke the numbers on `conv-56`
  (the output became zero). Reverted.
- **Broadcast unit start works but is harmful.** The two "mysterious"
  words of the vendor tail — `target 0x41, reg 0x0000 = 0` and
  `target 0x81, reg 0x0008 = 0x1f` — are exactly the enabling of the five units:
  with them band 0 passes with no per-unit `OPERATION_ENABLE` at all. But detached
  from the rest of the vendor plumbing they cause a regression — `conv-s2-112` starts
  timing out, and exact matches on `conv-56` drop to 11,991 instead of
  50,171. Reverted; the purpose of these words itself is recorded.

---

## Test 34 (2026-08-22): CORRECTION — "the numbers almost agree" was an artifact

The check that should have been done earlier: two consecutive invocations in one process
with **different inputs** (`run3.py`). Result:

| model | outputs for different inputs | unique values |
|---|---|---|
| `layer-conv-56` (regular 3×3) | matched (or 2 differences of 100,352) | 1–2 |
| `conv2d` (regular 5×5 s2) | **matched bit for bit** | 2 |
| `layer-band1-shape` (regular 3×3 s2) | 2 differences of 175,616 | 2 |
| `layer-dw-112` (**depthwise** 3×3) | **37,634 differences of 401,408** | 5 |

That is, for regular convolutions the output **does not depend on the input** — it is a constant. For
depthwise there is a dependency; it really computes.

**All earlier statements of the form "mean absolute deviation from the CPU 8.25,
50,171 elements of 100,352 match" are void.** These models with
random weights give an almost binary reference (roughly half 0 and half 255), and the
constant NPU output matched it on about half the elements purely by
chance. The correct description of the state: **the pipeline passes end to end (`DPU_0`
arrives, `DT_WR` equals the full tensor size, no timeouts), but for regular
convolutions a constant is written.**

### Instability between runs

Separately it surfaced that the very same run yields **two different constant
states** — "almost all 0" and "almost all 255" (an exact inversion), alternating
in groups of 1–3 runs. This is not a regression of today's fixes: on the pinned
`5c1bfd4fa9e` the picture is the same.

What was checked and is **not** the cause (all reverted):

- **Register bank ping-pong.** Disabling the `POINTER_PP_*` /
  `EXECUTER_PP_*` bits in mesa, in the kernel, and in both at once — the instability remains.
  The `POINTER_PP_CLEAR`/`EXECUTER_PP_CLEAR` bits (S_POINTER 4–5) do not help either.
- **Extra `S_POINTER` writes from the kernel** before each task (the regcmd duplicates
  them) — removing them changes nothing.
- **Core reset between tasks** (`rocket_core_reset()` in the interrupt
  handler) — band 1 after it produces no interrupt at all.
- **Unwritten registers.** Adding the vendor zeros in `CNA 0x1210…0x1230`
  and `DPU_RDMA 0x5030/0x503c` — no effect.
- **Model file mutation** — the md5 of the `.tflite` does not change between runs.

### What this means for priorities

The "band 1 of the first layer" wall and the "output does not depend on the input" wall are most
likely one and the same: the MAC path does not deliver the result to the DPU. What needs
looking at is not the slicing but the data path CNA → CORE → DPU, i.e. exactly those mode registers
that still diverge from the vendor: `CVT_CON0..3` (ours has the converter at scale=1,
the vendor's 16384/−128 for ARGB and `0xa` for the rest), `DATA_FORMAT=0xe0`,
`BS_CFG=0x148` + `BS_MUL_CFG=0xe01`, `BN_CFG=0x92`. They cannot be copied one at a time
(the RDMA experiment showed that) — a consistent bias/scale layout
in memory is needed.

**Test 34b.** The check "give a regular convolution the depthwise MAC bits"
(`CMAC 0x200c = 3`, `CORE 0x300c = 2` instead of 1 and 0): does not work and is worse —
timeouts appear, and there is still no dependency of the output on the input. In
retrospect expected: `DW_EN` makes the MAC compute per-channel, i.e. an
arithmetically different operation. Reverted, tree at `5c1bfd4fa9e`.

---

## Test 35 (2026-08-22): diagnosis — weights land in CBUF with a shift, period 8

Method: a 1×1 convolution with **identity weights** and zero bias (`layer-identity.tflite`,
56×56×32 → 32) — a perfect oracle, the output must repeat the input. Plus feeding
a constant input (`run5.py`, all 0 versus all 255) so the response is
unambiguous.

**Result:** exactly **4 output channels of 32 respond, with period 8** —
`[4, 12, 20, 28]` in one run and `[6, 14, 22, 30]` in another. The other 28
channels do not react to the input at all. The period is constant, **the phase changes from
run to run** — this is the same instability seen in Test 34, but now
with an exact signature: weights and channels drift apart by a variable offset.

Checked along the way (buffer dump `ROCKET_DEBUG=dump_bos`):

- the input in the buffer is correct (255 unique values, the data is in place);
- the weights are packed flat `weights_out[oc*32 + ic]`, the ones stand at a stride of 33 —
  that is exactly the diagonal of a 32×32 matrix, i.e. the packing matches the code's
  intent; the second half of the buffer (the buffer is allocated at twice the size) is zero;
- the buffer addresses are sound: IOVA `0x0` (weights, 2048 B), `0x1000` (bias),
  `0x2000` (input, 200704 B), `0x33000` (output), descriptors 1–4. The zero address
  of the weights is simply the start of the address space, not an error.

### Requantization arithmetic worked out

The weights in the model are quantized with zero-point **1** (one → 255, zero → 1). mesa
puts `q − 128` into the buffer (hence 127 and 129) and compensates the difference with a separate
term `DPU_BS_OW_OP = 0x80 − zp`. The scheme is coherent; the vendor's "always 0" does not
fit it.

Negative results (reverted):

- **`BS_OW_OP = 0`** (the vendor value) — breaks the compensation: the output
  collapses to the zero-point, the accumulator is at zero.
- **`BS_OW_CFG` bit `OW_SRC`** (vendor `0x125`, mesa `0x124`) — changes
  nothing, the saturation remains.
- **Grouping input channels by 16** instead of 32 in `rkt_coefs.c` — timeouts and
  zero responses.

### Where to look next

Not at the weight packing in memory (it matches the code) and not at the addresses, but at
**where the weights end up in CBUF**: the offset drifts between jobs, so the
write pointer of the weight bank is not being reset. Candidates — `CNA_CBUF_CON0`
(banks and reuse bits), `CONV_CON2` bit `CMD_FIFO_SRST`, and the order of CNA reset
before a task. The period 8 with 32 channels and a group of 32 is a separate lead:
it looks like four subgroups of 8.

---

## Test 36 (2026-08-22, autonomous check): the failures are `DMA_READ_ERROR`, and their frequency depends on memory pressure

While collecting statistics on the response phase (Test 35), timeouts suddenly
started pouring in. Breakdown:

- **every timeout is accompanied by an interrupt `raw=0x00001000` —
  `DMA_READ_ERROR`**, the counters match one to one (11 timeouts = 11 errors,
  then 5 = 5). The driver catches this with `WARN_ON`, hence the traces in dmesg;
- there are **zero** messages from `rk_iommu` at the same time — it swallows them
  silently, as noted in [[npu-rk3568-m1-hazards]];
- **the failure rate depends on free memory.** With 119 MB free and kswapd
  active (565 thousand pages stolen) — 11 failures out of 12 runs. After
  `drop_caches` (2494 MB free) — 5 out of 12. Same code and models.

Responses in this state are meaningless: runs give sometimes 0, sometimes 1,
sometimes all 32 "responding" channels — the latter simply because one of the
two runs of a pair hung and returned garbage.

**Correction to Test 35.** The version "weights not mapped, IOVA 0 is a sign of
failure" is **wrong**: `drm_mm` is initialized from
`domain->geometry.aperture_start` (`rocket_drv.c:100`), and with `rk_iommu` the
aperture starts at zero. So IOVA 0 for the first buffer is legitimate, and
yesterday's reading was correct.

### What this changes

The periodicity of 8 and the floating phase from Test 35 may not be a register
error but a consequence of the NPU periodically reading something other than
what lies in the buffer. So we should look at memory management in the driver,
not at the block configuration: are the pages of the shmem buffers pinned for
the duration of NPU work, do they migrate during reclaim, is the IOMMU mapping
synchronized. An indirect argument — m1 runs with `mem=4G` and production
services, i.e. reclaim there is constant.

Testable consequence: if page pinning is insufficient, the failures should
disappear when memory pressure is relieved and return as it grows — which is
exactly what is observed.

## Test 37 (2026-08-22): oracle in a single process — the output of a regular convolution is a constant, the "bands" were an artifact of comparing across processes

`layer-identity.tflite` (1×1 identity, 56×56×32→32; CPU: fill0→0, fill255→42),
`run7.py`: six inputs (fill 0/64/128/192/255 + gradient) in ONE process.
All six NPU outputs are **byte-for-byte identical**: {0, 255}, 62.5 % zeros.
The morning's "band degradation by channel" (28 rows per channel, period 4/8) and
"0→0, 255→255" — a comparison of outputs of DIFFERENT processes under a floating
two-phase instability; within a process the dependence on the input is zero. The
28-row bands are not mesa slicing: there is a single task (CBUF_CON1=28 — that is
entries per row, 56·32/64).

## Test 38 (2026-08-22): CNA_DMA_CON0 bit 20 IS LOAD-BEARING; MAC computes the input for the first time

Cross-check of the CNA block of identity against vendor task 4 of mobilenet:
mesa wrote DMA_CON0 by named fields → `0x071c07`, the vendor everywhere `0x171c07`.
Bit 20 (RESERVED in registers.xml) was lost — exactly what the memory note
warned about. Fix: write the value as a whole (mesa commit `1a26bf32109`).
**Effect:** the first invoke of a process began to depend on the input
monotonically: fill 0→255, 64→0, 128→0 (= zero-point ✓), 192→mixture (12/32
channels 255), 255→255. The numbers still do not converge (no compensation for
the weights' zero-point: identity has wzp=1, weights are stored as w−128, the OW
mechanism on 3568 does not fire; on fill192 the 12/20 channel split surfaces
again — CBUF phase), but this is the first input-dependent result of a regular
convolution. dw did not regress (mean|d| 157 versus 155 without the bit — noise;
dw was previously only input-dependent, not accurate).
Checked and reverted: the vendor's CBUF_CON0 bank split (weights=required,
data=remainder; instead of mesa's "weights=remainder") and DATA_ENTRIES×2 — on
identity both gave solid 255 with no improvement, alone and together.

## Test 39 (2026-08-22): the second invoke hangs on the other ping-pong bank; DMA_READ_ERROR is secondary, the MMU is disarmed at that moment

`run8.py` (10 alternating invokes in one process) + dmesg: invoke 0
completes normally (raw 0x4, 0x1, 0x150), invoke 1 starts on the OTHER
ping-pong pair of bits (raw 0x8, 0x2 — even twins) and stalls: CNA went,
CORE/DPU are silent → 512 ms drm_sched timeout → resubmit on hung hardware →
`DMA_READ_ERROR` (raw 0x1000). MMU dump from the IRQ handler (rocket_job.c patch)
at the moment of the error: `dte=0 status=0x18 fault_addr=0 rawstat=0` — this is
NOT a page fault, the MMU is not armed at all (DTE base is zero). Hence:
- "failure rate depends on free memory" (Test 36) — buried for good;
  mechanism: timeout → resubmit without MMU. After a fresh boot
  12/12 single runs are clean.
- all the previous statistics of "two constant states" are explained by the fact
  that only the first invoke of the process was actually executed, the rest
  timed out and garbage remained in the output buffer.
Next: (1) why the config does not reach the second pp-bank (revisit
ping-pong AFTER the CORE map fix — Test 34 checked before it); (2) a consistent
move of requantization to the vendor model (BS bias stream + OW_SRC=1,
OW_OP=0, weights without shift, DATA_FORMAT=0xe0) — this is now the main path to
the numbers.

## Test 40 (2026-08-22): the second-invoke wedge is defeated — core reset + manual MMU re-arm

Pre-submit instrumentation (rocket_job.c, dump S_POINTER of all units via
ioremap DPU 0xfde44000): before submit 1 all units show `sp=0x1000e` —
**EXECUTER (bit 16) stayed on bank 1** after task 0; the config is written to
bank 0, the units wait on bank 1 → stall. The PP_CLEAR bits (0x3e in S_POINTER,
Exp E) do NOT release the executer. The vendor on RK3568 (num_irqs=1) does not
write S_POINTER from the driver at all (Exp D1 — skipping the write does not
cure), and the PC rotates the banks itself via the array of task descriptors
(`PC_TASK_DMA_BASE_ADDR`; mainline writes 0). The vendor's
`PC_DATA_EXTRA_AMOUNT+4` is equivalent to our word count (Exp D2
breaks invoke 0 — reverted).

**Working workaround (Exp F, rocket_job.c.test-f):** if CNA has bit 16 set —
`rocket_core_reset()` + manual re-arm of the NPU MMU (reset_control wipes the
MMU behind the back of rk_iommu, which reprograms the DTE only on runtime
resume): stall(2) → DTE → zap(4) → INT_MASK=3 → **ENABLE_PAGING(0)** →
unstall(3). Important: the rk_iommu command codes are ENABLE_PAGING=0, DISABLE=1
(the first attempt with cmd=1 turned paging off, status=0x18). After the fix
status=0x19.

**Result:** 10/10 alternating invokes in one process, 0 timeouts,
3 ms each; run7 (6 inputs) passes stably; **mobilenet in full:
522 ms, 1 timeout instead of 28**. The output is input-dependent, but the numbers
do not converge (the CBUF phase floats between processes, f128≠0 — fronts:
requantization + phase).
This also explains the old wall of Test 32 "band 1 of regular convolutions
fails": inside a multi-task job, task 1 steps onto the same executer wedge.

## Test 41 (2026-08-22): replay of vendor tasks through rocket — EXACT MATCH

A replay harness `replay.c` (m1:~/npu-rk3568-test/) was built: it takes a live
capture of the vendor stack (kiln capture.so, patched: post-dump of BOs after a
synchronous submit + `RKNPU_TASK_LIMIT` — trimming task_number for clean per-task
references), creates rocket BOs of the same sizes, copies the contents, rewrites
all address words of the regcmd (260 words: range match of old IOVAs → new) and
submits the task through /dev/accel/accel0.

**Result: task 0 of mobilenet (ARGB conv 3×3 s2, full vendor
per-channel requantization BRDMA DATA_USE=7) through rocket gives byte-for-byte
the vendor output (225792/225792, 0 mismatches), deterministically (4 runs).**

Consequences:
- the rocket driver environment is SUFFICIENT (clocks/iommu/PC launch are not to
  blame);
- the BS stream `[bias i32×4][ow i16×4][mul u16×4]` (8 bytes/channel, groups of 4)
  is read correctly by the hardware under rocket too;
- all of mesa's incorrectness is in the contents of its regcmd/buffers; the
  harness gives a deterministic bisection without phase noise.

Harness mechanics:
- regcmd_count for rocket = vendor regcfg_amount + 4 (for task 0: 133+4=137);
  the amount formulas are equivalent (rocket: count−1; vendor: amount+EXTRA−1).
- Vendor t0 lives in bo01 (blob) @0x429d40; PC chaining of tasks — words
  `PC 0x10/0x14` in the tail of the task (amount of the next one); in a single
  replay it is cut off by the amount, no harm.
- PITFALL: a BO must not be put simultaneously into in_bo_handles and
  out_bo_handles — the job silently hangs in the scheduler (submit ret=0,
  hw_submit is not called).
- References: bo02.post with RKNPU_TASK_LIMIT=1 changes by exactly 225792 bytes
  (output of t0); the full bo02.post (53 tasks) is INVALID for early tasks —
  the intermediate buffer is reused by later tasks.

Earlier in this session (same day): the vendor requantization model was
uncovered from the .rknn container and the live capture:
- BS stream 8 bytes/channel: bias i32 (bias_q − (xzp−0x80)·Σ(w−wzp)),
  ow i16 (= wzp−0x80; OW stage: acc − ow·Σx), mul u16 (scale, shift=14);
- registers: BS_CFG=0x148 (bit 3 RESERVED — the vendor sets it!), BS_MUL_CFG=0xe01,
  DATA_FORMAT=0xe0, BS_OW_CFG=0x125/0x36d (conv/dw, OW_SRC=1), BRDMA_CFG=0xe,
  NRDMA_CFG=1 (bit0=disable), 0x5024=ch−1; on top of BS — BN RELUX (0x92 + CMP
  in the intermediate domain) and per-layer OUT_CVT (scale u16, shift 19..22);
- mul normalizes channels to s_wmax (per-tensor: exactly 2^14), the layer scale
  s_in·s_wmax/s_out sits in OUT_CVT — the identity arithmetic converges by hand
  (f255→42 ✓);
- mesa was moved to this scheme (rkt_coefs.c: stream packing; rkt_regcmd.c:
  vendor values of the BS registers), but on identity the output became
  input-independent garbage [0,255,255,0] with a floating phase between
  processes — the bias band does not reach the hardware (the mul band does
  reach it: mul=0 in the stream quenches the output to zero). The cause is NOT
  in the stream layout (see EXACT MATCH) — to be found by bisection on the harness.

## Test 42 (2026-08-22): broadcast start = the cure for the phase; identity is BYTE-EXACT

Bisection on the replay harness (vendor task 4, mutations toward mesa values):
- PAD_CON0=0, FEATURE_MODE_CFG=0x7810, absence of zeros 0x1210..0x1230 —
  all HARMLESS (EXACT MATCH preserved);
- **replacing the vendor's broadcast start (`0x41:0=0`, `0x81:8=0x1f`)
  with mesa's per-unit OP_EN=1 → 41% of output bytes are wrong.**
  A sequential start desynchronizes the pipeline; this was the source of the
  "floating CBUF phase" and the garbage in the BS bands.

mesa fixes:
1. `rkt_regcmd.c`: tail = `0x00810000001f0008` (broadcast enable of all five
   units) instead of four per-unit OP_EN. The comment "0x81 does not work on
   RK3568" in the code was FALSE — the vendor uses it on every task.
2. `rkt_coefs.c`: sign of OW: the hardware ADDS ow·Σx → ow = 0x80 − wzp
   (symptom before the fix: mirrored output with gain, f0→255 instead of 0).

**Result: layer-identity 6/6 fills = 0 mismatches with CPU, stable
across processes (the phase is gone).** Other layers: dw ~1 unit of mean,
1×1 pw ~2-4, regular 3×3 worse (mean up to 20) — the per-channel gain is chaotic
⇒ the layout of the WEIGHTS of a regular 3×3 is wrong (and always was; the 1×1
and dw layouts are correct).

Digging into the weight layout (RKT_WPOKE probe: a single byte 127 in a zeroed
buffer, neutral BS stream; model layer-delta.tflite 3×3 16→32, each channel has
one tap): the hardware reads the buffer as
**[group of 16 kernels][tap][kernel-in-group 0..15][16 bytes ic]** (unit 16 B,
256 B per tap per group, 2304 B per group) — mesa's [oc-groups of 32][x][y]
are incompatible with this. Remaining questions: tap permutation (an x-reverse
was observed), ic shift (+8 mod 16?) and the "ghost" oc+8 (one byte lights up
two channels — possibly the hw expects ic aligned to 32, and the rows overlap).
The full probe atlas is being written to /tmp/watlas.{log,json} on m1.

## Test 43 (2026-08-22): the weight layout of a regular convolution is solved
Method: rknn-toolkit2 on droid (venv rknnvenv, py3.12; tflite does not work on
arm64 — ONNX→rknn conversion offline), probes with marker weights and a
dense probe V[oc,ky,kx,ic]=(oc·7+ky·5+kx·3+ic·11)%256; reading the packing
directly from the data section of the .rknn + RKT_WDUMP cross-check of the mesa buffer.
**RK3568 formula (Cin>8, regular conv):**
`pos = (oc/16)·KH·KW·slices·16·rowic + (ky·KW+kx)·slices·16·rowic +
(ic/32)·(16·rowic) + (oc%16)·rowic + (ic%32)`,
rowic=min(align(Cin,16),32), slices=ceil(Cin/32), byte = w−0x80.
Groups of 16 kernels (not 32), tap-major, ic slices of 32 ABOVE the kernels.
Check: 0 byte-wise diffs against the vendor buffer. Also uncovered the per-tensor
requantization scheme (probe-D2): BS_CFG=0x158 (MUL_BYPASS), OW from the register
BS_OW_OP=0x80−wzp, BRDMA_CFG=2 (bias-only, 4 B/channel), 0x5024=(bytes/8)−1.

## Test 44 (2026-08-22): the 8-channel atomic — the root of the "dense residue"
Symptom: with correct weights/BS/regcmd, dense convolutions gave mean 5–13,
bands 0/14, "quadrants". Digging: the map of the NPU output c28 = 2×2 tiles,
bottom = channels+8; impulse probes through identity (a 255 diagonal of all
channels is permutation-resistant) showed: the value (y,x,c) lives in the dump
twice (offset and offset−224 B), columns 14-27/42-55 are missing ENTIRELY.
A live vendor capture probe-D2 (reboot into the vendor overlay, capture.so,
encoded inputs + impulses) gave the truth:
- **input and output: planar 8-channel surfaces, 8 B/pixel,
  surf stride = W·H·8; ALL CNA/DPU strides are in 8-byte units**
  (impulse (3,14,0) → byte (3·28+14)·8=784 in the input BO);
- **DATA_ENTRIES = W·ceil(C/8)/4** (entry 32 B, atomic of 8 channels) — matched
  all 51 tasks of the vendor mobilenet (C=8..512);
- **CBUF = 8 banks × 32 KiB** (1024 entries/bank); data banks = the minimum
  for entries·H, weights — the remainder (7+1, 4+4, 6+2 at the vendor ✓);
- band offsets on split = rows·W·8 bytes.
The past "fixes" CBUF_BANK_SIZE 32→64K and the 16-byte atomic were
RK3588 heritage. The "band-boundary bug" (rows 0/14 dic), the "dead rows
first-16ch", the c28 quadrants — all of that is ONE layout bug.

## Test 45 (2026-08-22): full layer set clean
After the fixes (mesa be39766243f): identity, id64, d32, dic, dtap, c28,
conv-56, pw-112, dw-112, dw-s2, conv-s2-112, first-16ch — on f0/f128/f255,
row/col gradients, channel code and mix: **max ≤1, mean ≤0.22**
(dtap f255 1/0.86 — rounding of the u16 multiplier). first-16ch was 128/55.
mobilenet as a whole still diverges: the ARGB path of the first layer (Cin=3) is
not implemented — the only remaining front before end-to-end inference.

## Test 46 (2026-08-22): first-layer ARGB path implemented
Method: probe-RGB.rknn (28×28×3→32 s2, dense weights) + vendor capture of
mobilenet tasks. Implemented in mesa (fa07b52bb10): CONV_CON1 ARGB_IN=10,
RGB_BYTELENGTH=W·H_band·9, input = dense RGB (3 B/px, rows aligned
to 8 B, WITHOUT −128 — the shift is done by CNA CVT: scale 16384 / offset −128 /
truncate 14, alpha scale 1), weights = the same 16-kernel tap-major scheme with
an 8-byte kernel row (ic + zeros), band offsets in dense RGB bytes,
entries=align(ceil(W·8/32),2). Knob TEFLON_MAX_OPS=K — per-layer
bisection of chains. **layer-first-rgb: ≤1 LSB on all patterns.**
PITFALL: get_tensor(intermediate) on the NPU interpreter is invalid
(the arena is reused by CPU ops) — trust only the final logits.

## Open: depthwise with C>32 (mobilenet op3)
mobilenet as a whole: logits mean 0.71 max 110, top5 miss. Bisection with
TEFLON_MAX_OPS: K≤3 clean, K=4 breaks → op3 = dw s2 112×112×64.
The vendor emits such dw as 32-channel tasks (t6/t7: Cin=32, DPU cube
0x1f001f with real 64), mesa feeds 64 (0x3f0040/0x3f003f).
Next step: channel splitting of dw tasks into groups of 32
(weights/input/output with group offset, 4 surfaces per group).

## Test 47 (2026-08-22): dw groups of 32 channels — mobilenet END-TO-END
Vendor mechanics of dw C>32 uncovered: tasks t6..t9 = full tasks of
group 0 + delta tasks of group 1 (addresses only: FEATURE +W·H·32,
DCOMP_ADDR0 +KH·KW·32, DST +Wout·Hout·32, BS +32ch; config inherited
via the pp bank). mesa (96c6ea32da9): full tasks for every
(band × group), dw-weight buffer [group][tap][32], entries per group
(bands = vendor 63/50). **The killer was WEIGHT_REUSE**: group 1 silently
reused group 0's weights from CBUF; reuse is now only within a
group. Isolated op3: max 0. The final conv 1024→1001 (weights 1 MB >
CBUF 256K) needs the vendor fp16-FC mode — for now honestly unsupported
(goes to CPU; previously it wedged the job until timeout — those very "110/0.71").
**Result: mobilenet_v1 end-to-end on NPU: 87 ms (CPU 117 ms), 0 timeouts,
logits mean 0.11 / max ~48, top5 4/5.** Knobs: TEFLON_MIN_OPS (window
of nodes with MAX_OPS), RKT_MAXROWS (forced bands). Verified: ≥4
tasks in a job do not break the pipeline. Remainder: mean 0.11 (accumulation of ±1),
rare flaky runs with chunks of ±128 (configuration race, rarer after
FC left) — next candidate: PC task-descriptor mode in the kernel.

## Test 48 (2026-08-22): FC path (final 1024→1001) implemented
Vendor t50 (297 words) = an ordinary u8-conv task with weight streaming
(1 MB with 7 weight banks) + a second DPU sub-configuration (FLYING_MODE,
OUT_PRECISION=2 — an fp16 copy for their runtime; we don't need it). Keys
(mesa 01351cd9c1d): FEATURE_MODE_CFG |= NONALIGN | SURF_LEN=align(K,16)/8
(without them the DPU never finishes → timeout — the former "FC wedge"); the input 1×1×C
is read by CNA as (C/8)×1×8; GRAINS=1; output channels align 16
(the 1007 family); kernels exact (1001); **the last kernel group of weights is
compact: K%16 rows without padding** (probe-FCL byte by byte: 62 full
groups + tail of 9 rows = 1025024 B). Small FC (weights<CBUF) — the usual
formula without the tail (probe-FCS).
Isolated op28: max 1 ×4. Full network with FC: best run **19 ms,
top5 = CPU completely (top1 412 ✓)**; but the flaky race hits more often with
the extra partition → the path is opt-in (RKT_BIG_FC=1) until the race
is beaten. Regression suite: clean.
PITFALL (again): get_tensor(boundary tensor) on the NPU interpreter =
arena garbage; trust only the raw dump and the final output.

## Test 49 (2026-08-23): PC task chain — flaky race killed
Dissection of the vendor submit (kiln rknpu_job.c + capture: task_base_addr=0!):
the RK3568 hardware does NOT read the descriptor array; the task chain moves
via PC tails of the regcmd streams ([BASE next][AMOUNTS next][0x0041 word]
[broadcast OP_EN 0x1f]) — the tails were ALREADY in mesa (RK3588 heritage),
but the AMOUNTS patcher divided by 2 (RK3588 scale); on RK3568 = words−1.
The kernel's legacy stepping ON TOP of the auto-chain (double start of every task)
was the source of the flakes/timeouts all these days.
Kernel (chained mode, flag = drm_rocket_job::task_desc_addr):
one start per job (TASK_NUMBER=N, TASK_DMA_BASE=0, OP_EN pulse),
no S_POINTER poking, no force-bank0 hack, mask open, mid-chain
interrupts (CNA/CORE done) are only acknowledged; **DPU-done = drain of the whole
chain** (last task). The TASK_STATUS counter ticks (0xa001), but
zeroes out by the end — not a criterion.
Pitfalls along the way: (a) the PC_TASK_DMA_BASE_ADDR macro shifts <<4 — writing the
address through it = hard bus-hang (2 power cycles, truncated
bcachefs files — SYNC before dangerous tests!); (b) INTERRUPT_MASK=0
from the hard handler was not restored.
**Result (mesa e92bba7628c + module): mobilenet_v1 with FC (default on):
128 ms, top-5 = CPU INCLUDING ORDER, logits identical between runs
(mean 0.11), 0 timeouts 10/10. Suite ≤1 LSB.** Tail: 128 ms > 19 ms
of the best legacy run — inter-job overhead, the next optimization.

## Test 50 (2026-08-23): 9 ms — printk was the main brake
Without the Exp F reset the chain does not wedge (the workaround is kept only for the legacy path).
The 128 ms was explained by dev_info prints in the hard IRQ (3 lines per
interrupt, serial console 1.5 Mbaud, loglevel 8): converted to
dev_dbg → **mobilenet_v1 = 9 ms (CPU 117 ms, ×13), top5 = CPU with
order, 0 timeouts, suite ≤1 LSB.**

## Test 51 (2026-08-23): accuracy ceiling + MMU re-arm in recovery
Accuracy (negative result, knobs RKT_SC/RKT_RB in mesa 6d3…):
the OUT_CVT_SCALE field is effectively 15 bits (16-bit scale with shift+1 → garbage
±90); the current 15-bit mantissa is already round-exact. Rounding bias in acc
(½ quantum: the minuses vanish, but +1 grows to 6001 versus the 1609 baseline;
¼ and ⅛ are also worse) — the hardware does NOT floor; the residual ±1 = an irreducible
difference between its rounding pipeline and tflite doubling-high-mul (double
rounding). The baseline (−977/+632 out of 25088) is the optimum.
Recovery: rocket_reset after a timeout did not re-arm the MMU → one bad
job turned into an endless series of timeouts (the morning's 11.8 s) until
rmmod. rocket_core_reset_rearm() is now in timeout recovery and in the
legacy pre-submit; the first run after insmod also became clean.
Stress: a deliberately broken job → the following mobilenet 9 ms without intervention.

## Test 52 (2026-08-23): avgpool on NPU; DST_SURF_STRIDE is in bytes
avgpool → synthetic dw-conv: weights q=255, w_scale=1/(fw·fh·255),
wzp=0 → every tap exactly 1/(fw·fh); packed bytes 0x7f = vendor t49
byte for byte. Only pad=0 (tflite at the edges divides by the valid elements).
Isolated avgpool: 0 diffs. Bug found along the way: DST_SURF_STRIDE is a raw
byte register (Wout·Hout·8); the shift macro <<4 gave the same number
for even WH (WH/2·16≡WH·8), but at WH=1 wrote 16 instead of 8 → the second
8-channel surface slid (channels 8+ garbage). The MAX2 hack for FC is removed too.
**Result: conv+dw+FC+avgpool all on NPU: 9 ms, top5 = CPU perfectly,
mean |diff| of logits 0.03 (was 0.11 with CPU avgpool in the middle). On CPU
only reshape and softmax.** Regression suite clean.

## Test 53 (2026-08-23): mobilenet_v2 end-to-end — four new defects uncovered
Model: mobilenet_v2_1.0_224_quant (65 ops: 36 conv, 17 dw, 10 add,
avgpool, reshape). Start: NPU computes garbage (mean logit diff 12).
Bisection with the TEFLON_MIN/MAX_OPS window (semantics [MIN..MAX)) + reliable
readout: get_tensor of a boundary tensor on the NPU interpreter is arena
garbage (that same trap, 3rd time); reliable: preserve_all_tensors on
single windows and the raw BO via ROCKET_DEBUG=dump_bos.

1. **FEATURE_GRAINS on narrow features** (op49/52/60, 7×7): grains=stride+kh
   is too small — MAC starts before the first weight group has been fetched into CBUF, oc0-15
   is unstable garbage. The vendor's 51 mobilenet_v1 tasks give the formula
   grains = stride_y + kh + stride_y·((Wout<28)+(Wout<14)); Wout==1 —
   special cases (FC=1, avgpool=kh). Knobs RKT_GRAINS / RKT_GRAINS_OLD.
   Note: on the v1 7×7 layers the new grains (=vendor's 4) slightly change
   the rounding of individual logits (top-2/3 swap places, mean 0.03 the same).
2. **align-32 of output channels** (op2, Cout=16, bands 64+48): DPU waited for
   4 surfaces instead of 2 → all banded tasks after the first wrote
   nothing (constant = fill). Fix: announced = align(Cout,16) without the floor
   of 32 (vendor FC: 1001→1008). align(·,8)=24 the hardware does not accept (op5
   broken at announced 24) — 24 is announced as 32. Knob RKT_OUTALIGN.
3. **Compact weight tails** (op5/8/12, Cin=144 or Cout=24): the WPOKE
   map of op8: the tail ic-slice (Cin%32=16) — rows of 16 bytes
   (byte 2064 → oc1, not padding), the tail oc-group (Cout%16)
   is compact in ALL convs (not only FC). Symptom: the WEIGHT_BYTES register
   always was compact (24·144=3456). With the old packing only oc0 was
   intact (its row coincides in both layouts).
4. **Fused add**: the vendor's RK3568 config captured from the resnet18 capture
   (25 tasks, 8 of them conv+add: EW_CFG 0x900000d0, ERDMA 0x40000000,
   EW base/line(surf-8)/surf strides as raw bytes over the 8ch planar layout,
   EW converter = s2/(si·sw) with the same 15-bit mantissa as OUT_CVT —
   vendor's 366.0 = 23423>>6 confirms the formula). The hardcoded
   rk3588 table removed, OUT_CVT shared. Two bugs of ours: (a) the fuse did not update
   output_zero_point/scale — requant into the conv-output domain, not add's;
   (b) the vendor EW_CFG without RELU_BYPASS — resnet fuses add+ReLU, while
   v2 residuals are linear: got was clamped from below at zp (bit 9 added).
   REMAINDER: add with C%32≠0 (add9, C=24) — EW reads the last pair of
   surfaces of the second input as deterministic noise (ch16-23), ch0-15
   are exact; mechanics not decoded → such adds stay on the CPU for now (gate in
   supported; in v2 it is one op out of 65).

**Result: mobilenet_v2 end-to-end: 15-16 ms (CPU 76, ×5), top-1 matches,
top-5 same set (logits max 6, mean 0.98, bit-exact repeatable).
No regressions: v1 9 ms mean 0.03, suite ≤1 (band1-shape and conv-tiny —
old anomalies, do not move with the knobs, not today's).**
mesa: 4 commits 5f71f4686c1..8b2f3482402, pushed to iav/mesa.

## Test 54 (2026-08-23): add C%32 — kernel padding; banded EW not decoded;
## conv-tiny defeated
1. **Fused add C%32**: pad kernels up to align(C,32) (group geometry by
   pad, memset background zp−0x80 = zero weight, BS tail already zero, unpacking
   by real). Registers come out perfectly (kernels 32, ORIG=CH=31,
   WEIGHT_BYTES 4608) — on single-task such adds work.
2. **Banded fused adds are not cured by padding**: reconstruction
   of the actually-read EW bytes (E=(got−za−conv)·sa/sB+zB) +
   histogram match: band 2 reads surface 2 (pair 1) FROM THE ROWS
   OF BAND 1 (the band offset is not applied to pair 1; pair 0 = ch0-15
   is exact in both bands). Band 1 s2 — not op5 data at all (source
   not found: not op7, not conv8, not shifts). Forced RKT_MAXROWS splits into
   3+ bands break EVERYTHING (even ch0-15) — by themselves a useless
   discriminator; along the way it turned out: RKT_MAXROWS has no effect on
   full-fit tasks (my earlier "banded add16 test" was empty).
   Gate refined: only adds with a BANDED producer-conv go to the CPU
   (CBUF-fit estimate in tfl_device.c from tensor shapes), not C%32.
3. **conv-tiny (Test 19/34) DEFEATED with a single line**: 8-byte rows of
   the weight kernel — ONLY for ARGB (Cin=3); an ordinary conv with Cin=4..8
   announces WEIGHT_SIZE1=16 and reads rows with stride 16. rowic: Cin≤3 →
   8, otherwise the general align(rem,16). Both conv-tiny models are now bit-exact
   with the CPU. ARGB/16ch regression clean.
4. layer-band1-shape (ordinary 3×3 s2 banded, Cin=16, input 98×224) —
   deterministic moderate error (max 81, present on a constant
   input too, grains/OUTALIGN/reuse have no effect). Combination outside real
   networks (ordinary 3×3 in v1/v2 — only the ARGB first layer). Known
   anomaly, in the backlog.
5. Release v2026.08.22: rocket.ko replaced with a fresh one (reset-rearm).
Regression: v1 9ms 0.03, v2 15ms 0.98, suite ≤1 (except band1). mesa:
8f0e045597f (pad+gate), bffb3ba6041 (rowic) — pushed.

## Test 55 (2026-08-23): banded fused add DECODED — RDMA_SURF_NOTCH
Method: there is no vendor banded add in the resnet18 capture (56×56×64
the vendor does not band — it streams weights, wbank=1 at wbytes=36864), so
a probe probe-ADDB was built (tfwork/gen_addband.py, rknn-toolkit2 on droid):
residual block 56×56×128 with 3×3, data 13 CBUF banks → the vendor is forced
to band. Probe TRAP: with ONE convolution between the add inputs the toolkit
bakes the residual into the center tap of the weights (no EW in regcmd at all!) — two
convolutions in the branch are needed, as in the resnet basic block. Offline decode of the .rknn
(Test 28) showed banded add tasks 3/4 (31+25 rows):
- EW_SURF_STRIDE = 0x6200 = W·H_full·8 — FULL-height in both bands;
- EW_BASE = band offset (0 / 0x3640);
- **RDMA_SURF_NOTCH (0x504c) = (H_full − H_band)·W·8** (0x2bc0/0x3640) —
  the missing register: the tail of each full-height surface outside the band,
  which the EW RDMA skips over when moving to the next surface.
False trail: our EW_SURF_STRIDE was already full-height — fill_task
computes output_surface_stride BEFORE the split, the split cuts only
task->output_height (hence the first "full stride" fix changed nothing,
and hence the NOTCH formula via output_surface_stride gave 0).
Result: v2 entirely on the NPU, all 10 adds: 14 ms, top-5 = CPU INCLUDING
ORDER, mean 0.98 → 0.82; 30/30 runs clean (flakes in the early series —
dirty state from runs interleaved with the old library).
Gate add_producer_is_banded() from tfl_device.c removed. Regression: v1 9ms
0.03, suite ≤1, conv-tiny bit-exact. mesa: eb3de81b6cb (pushed).

## Test 56 (2026-08-23): resnet18-tflite — three fixes, remainder in add accuracy
Model: tfwork/gen_resnet18.py (Keras resnet18 random weights, legacy
uint8 per-tensor, ranges (0,6) → zp=0) → resnet18-quant.tflite;
m1:/media/nvme/npu-assets/. CPU 345ms, NPU 40-66ms. maxpool on the CPU
(partitions get torn — works normally).
1. **MAIN FIX (3661863e7c5): weight ic-slices ABOVE tap.** The order within
   a 16-kernel group = [ic-slice][tap][oc%16][ic%32], not
   [tap][ic-slice] from Test 43. The orders are indistinguishable on 1×1 (all
   multi-slice convs of v1/v2!) and at Cin≤32. Proven byte-by-byte by
   probe-DENSE128 (3×3 Cin=128 dense, 0/36864). Fixes ordinary 3×3 with
   Cin>32: layer-ic128-fit/ic64-band/band128 127→1. The "banded" hypothesis
   was false — full-fit 16×16×128 was broken just the same (bands acted
   as a symptom, not the cause).
2. **Add fuse — host = the LATE producer (77615af90f6).** In downsample blocks
   both add inputs are in the partition; tflite places the 1×1 skip AFTER the main path;
   fusing into the early one = EW reads a tensor not yet computed (window [9..12)
   max 150). The vendor fuses into the 1×1-s2 (tasks 8/13/18 of its resnet18).
   fused_add_pad — the same rule. Our 1×1-s2+EW task is now
   register-for-register = vendor task 8.
3. **Fused RELU of ADD propagated** (EW_CFG bit 9 by activation; at zp=0
   numerically equivalent to saturation — it was not a defect).
REMAINDER: conv+conv+add windows give deterministic max 25-26/mean 3.2-3.5
(single convs 0.14; EW registers = vendor's) — fused add accuracy
in resnet style; end-to-end resnet18 mean ~18 (non-deterministic BECAUSE OF DIRT:
after a broken run the next one is broken; after a clean one — stable).
Method TRAP: measurements after broken runs are invalid — interleave
with a clean v2 run. band1-shape: 128/58 after a broken run = dirt; really
the same old max 81. A single ADD window (MIN=N) crashes (no producer in
the partition) — not a tool. [3..5) also silently crashes (edge case).
Regression: v1 0.03, v2 0.82, conv-tiny/ic128 bit-exact.

## Test 57 (2026-08-23): resnet18 CLEAN; FLAKE RACE KILLED (surface alignment)
mesa 7d417f79d9c. resnet18-quant: **max 1 mean 0.32, 44ms vs CPU 351 (×8),
top-5 matches (one swap of neighbours at diff 1), 15/15 runs
without flakes, 0 DMA errors**. Three findings:
1. **Surface alignment — the root of the old flake race.** A feature
   surface = align(W·H,4) px (whole 32B CBUF entries): the vendor on ALL 7×7
   maps writes 52 (DMA_CON2, DST_SURF_STRIDE, SURFACE_ADD=52/104 dw,
   EW_SURF_STRIDE 416; 1×1 stays 1). Our 49-px surfaces gave
   sporadic PC DMA_READ/WRITE_ERROR with a clean IOMMU (fault_addr=0)
   on the 7×7 tails — resnet18 flaked 4/10. Layout transition: strides
   (rkt_task), BO sizes and converters (rkt_ml), channel_group offsets and
   EW (rkt_regcmd), helper rkt_surf_px() in rkt_ml.h.
   EW SURF_NOTCH generalised: surf_bytes − W·H_band·8 (covers both bands
   and the alignment tail: vendor 7×7 full-fit add: notch 24!).
2. **Real fused ReLU (BN RELUX).** u8 saturation implements the lower
   clamp only at zp=0; on (−4,4) models a negative accumulator survived
   (addmini2: 48→2). BN_CFG=0x92 + BN_RELUX_CMP = (255−zp)·so/(si·sw)
   (ceil) — the top expressible by the u8 output; at zp=0 equivalent to the vendor's
   6/(si·sw) (v1 t0: 40144).
3. **Duplicate-handle BO**: add(x, conv(x)) put one handle twice into
   in_bo_handles → the job silently wrote nothing (related to the
   in+out BO trap). Dedup in submit.
Fused add semantics (limitation, same as the vendor): the intermediate
conv output is NOT clamped to its own quant range before the addition — on
models with clamping quants (legacy ranges (0,6) + large values)
the CPU reference diverges; on normally quantized networks the effect is within
the usual drift. Toolkit-legacy TRAP: one default_ranges for
all tensors; (−4,4) for resnet18 checks, relu is now honest.
Regression: v1 0.03, v2 0.82 (3×), suite ≤1, conv-tiny bit-exact.

## Test 58 (2026-08-23): gate #1b CLOSED — DMA32 in rocket_gem, m1 back to 8 GB

Patch: `mapping_set_gfp_mask(gem_obj->filp->f_mapping, GFP_USER | __GFP_DMA32 |
__GFP_RETRY_MAYFAIL | __GFP_NOWARN)` in rocket_ioctl_create_bo() right after
drm_gem_shmem_create(), before drm_gem_shmem_get_pages_sgt() (precedent — etnaviv
for addressing-limited GPUs). BO pages land in the DMA32 zone (<4 GB), which
removes the NPU DMA_READ_ERROR with physical memory >4G (Test 18).

Procedure: edit rocket_gem.c (m1 ~/npu-rk3568-test, backup .pre-dma32) →
make -C headers → cp into /lib/modules/.../accel/rocket/ + depmod (the module
autoloads at boot!) → mem=4G removed from /boot/armbianEnv.txt (backup
armbianEnv.txt.pre-dma32) → reboot.

Result: **free 7.5Gi; full regression clean** — v1 9ms/0.03, v2 13ms/0.82
(3×), resnet18 15/15 max 1 mean 0.32, suite (5 models) ≤1, conv-tiny bit-exact,
dmesg 0 errors. `mem=4G` for NPU work is NO LONGER NEEDED: a pair of
patches suffices — GFP_DMA32 in iommu_data_ops_v2 (in the 7.2-rc7 BE kernel) + DMA32 in
rocket_gem (in the module). Tests moved from /tmp to ~/npu-rk3568-test/tests/
(survive reboot); models — /media/nvme/npu-assets/; venv ~/npu-venv.

Session TRAP: rocket.ko is INSTALLED in /lib/modules and loads itself at boot —
insmod after reboot gives "File exists", while the OLD build is what is running;
update specifically the copy in /lib/modules + depmod. Second (repeat): sudo in
the iav session hangs at the password — root operations only via root@m1.

## Test 59 (2026-08-23): end-to-end PC chain over the whole graph — v1 6ms, v2 7ms

Profiling: per-job floor ~0.21ms (conv-tiny invoke), v1 has 27 jobs →
~5.4ms of overhead out of 9. The jobs already went in a single SUBMIT ioctl, but each
operation was a separate job: its own IRQ+fence, its own hw_submit cycle.

Merge (mesa 0c50ebbb615, kernel NOT touched): the tail of the last
regcmd stream of operation i is patched with the address/count of the first stream
of operation i+1 — exactly like tasks within an operation; the whole graph = ONE job,
TASK_NUMBER = sum of tasks. The tail of a single task is now always a full
PC_BASE_ADDRESS command (a bare 0x0 is not patched via |=). BO lists: in =
external inputs (without producer), out = outputs of all operations; intermediate
tensors in NEITHER list (read and written inside the job; repeating a BO in
both lists wedges the scheduler). RKT_NO_CHAIN — fallback to per-op jobs.

The concern "DPU-done will arrive at the boundary of each operation and the handler will complete
the job early" was NOT confirmed: a chain of operations with different DSTs behaves like
a banded chain — completion via 0x300 arrives when it should, the kernel handler did not
change.

Results (8 GB, DMA32 module): **v1 9→6ms (0.03), v2 13→7ms (0.82,
10/10), resnet18 44→39ms (max 1 mean 0.32, 10/10)**, full layer run
(23 models) within baseline, dmesg clean. v2 now 7ms versus 76 CPU
(×11); band1-shape — old unresolved, unchanged.

## Test 60 (2026-08-23/24, NOT CLOSED): PPU maxpool — computes, but does not signal

The vendor's maxpool arrangement uncovered (resnet18 task 1 + probe-MP.rknn,
offline decode + scan of the 40-byte task table):
- PPU (0x6xxx, target 0x4001) + PPU_RDMA (0x7xxx, 0x8001), chunk of 29 words +
  4-word PC tail with broadcast OP_EN **0x60** (unit bits 5-6).
- Registers: IN/OUT cube (W/H/C minus 1), OPERATION_MODE 0x11 (max method +
  "flying"), KERNEL_CFG (all fields minus 1), PADDING_CFG top<<4|left,
  PAD_VALUE 0x7ff80 (−128 in 19 bits), DST/SRC addresses and strides IN BYTES
  (line = W·8, surf = W·H·8), 0x60dc burst=3, RDMA 0x7018=1.
- In the .rknn task table the PPU chunk is a FULL-FLEDGED task: enable_mask=0x60,
  **int_mask=0xc00**, amount WITHOUT the tail (conv 133/137, pool 29/33).
- Vendor kernel (rknpu_job_subcore_commit_pc): masks EVERYTHING except the
  int_mask of the LAST task (one interrupt per submit; NB int 0x30 =
  TASK_CON, 0x34 = DMA_BASE), int_clear of the first, TASK_CON (0x6|pp)<<12|N,
  DMA_BASE = address of the task table (walk!), OP_EN pulse 1→0.

What WORKED: the PPU on our stack COMPUTES and WRITES the output (mplast probe:
deterministic maxpool values in the output tensor; layout in question).
The vendor's single-interrupt scheme is implemented and WORKS for conv chains
(v1: 1 IRQ per 27 operations, 6ms) — the last_int_mask field in our uapi.

What did NOT work: the PPU does not raise a done interrupt on ANY of the 17 bits
(0xc00, 0x300, bit16, full 0x1ffff — silence at the GIC counter level), and
- in tails-only mode (DMA_BASE=0) the PC task counter waits for completion of the
  PPU task and the chain stalls (the conv after the pool does not start; TASK_STATUS
  0x20001); with pool last the job completes on the conv drain, but there is no PPU done;
- in walk mode (DMA_BASE=desc, vendor's) the PC fetches the first stream by the
  descriptor (dt_rd=134=amount+1, WITHOUT the tail) and the units do not start at all
  (wt_rd=0, raw bit16) — something in our walk setup is still not vendor-like.

Session TRAPS: (1) bit16 IRQ storm with the mask 0x1ffff open in
pool chains; after a series of such experiments IRQ line 183 died FOR GOOD
(0 interrupts from the GIC even after rmmod) — cured only by reboot. (2) our
hard-irq handler woke the thread only on 0x300 — 0xc00/0x10000 were swallowed.
(3) /dev/mem peek of the PPU block with the NPU suspended = Bus error (peek only
with autosuspend_delay raised). (4) first reboot of m1 ~4 min — btrfs,
tolerate; the PSCI hang did not happen.

State: mesa cf8ccb5ed60 (PPU skeleton behind RKT_PPU=1, by default maxpool
on the CPU; single-interrupt scheme active), kernel module local (uapi
last_int_mask, params rocket_desc_walk/rocket_open_mask, wake 0xfc00|bit16).
Full regression clean: v1 6ms/0.03 (1 IRQ!), v2 0.82×3, resnet18 0.32×5,
conv-tiny bit-exact, mp3s2 (CPU maxpool) ≤3.

Next: (a) capture a live vendor submit of probe-MP (vendor stack on m1,
2 reboots) — see their walk and the completion of the PPU task in dynamics; (b) descriptor
fields (op_idx grouped by operations? flags?); (c) possibly the PPU int
enable hides in the upper bits of some 0x60xx register.

## Test 61 (2026-08-24): PPU MAXPOOL CLOSED — resnet18 13 ms (×27)

Live capture of the vendor probe-MP (rknpu stack on m1, 2 reboots):
- trace commit: **task_base_addr=0x0** — the vendor goes tails-only, WITHOUT
  a descriptor walk (our walk theory was a false trail).
- conv→PPU→conv (3 tasks) = ONE interrupt raw=0x2aa; the PPU raises
  NOT A SINGLE bit in PC INT — and that does not stop the PC from running the chain.
- The live stream byte-for-byte = ours (structure/tails/minus-1 fields).
- Full snapshot of the first submit (capture.so LD_PRELOAD, 5 BOs + table) →
  vendor-capture/mp_full.

Replay of the snapshot through rocket (replay3.c): **the chain with PPU passes on our
kernel, PPU output = exact maxpool (0/1024)**. Mutational bisection:
1. SPLIT_CONV1 (conv chunk in a foreign BO) — works; SPLIT_POOL (PPU chunk in
   a foreign BO) — WEDGE. **The PPU chunk must lie in the same BO as the streams
   of the previous task** (conv streams jump between BOs freely).
2. PATCH_REG 0x6040 (POOLING_PADDING_CFG): zeroing it in a WORKING vendor
   stream reproduces our wedge. Bit map by brute force:
   **[3:0] left, [7:4] top, [11:8] right, [15:12] bottom**; single
   nibbles wedge, 0x11/0x1100/0x1111 work. Rule: every pooling window
   must fall into input+padding ALONG BOTH AXES, otherwise the PPU
   starves forever and the whole chain stalls. Our mp3s2 wedge = TFLite SAME puts
   the padding on the right/bottom, while we programmed only top/left.

mesa fixes (589a026b9ad): the pool chunk is appended into the regcmd BO of the previous
operation (POOL_CHUNK_ROOM 384 B, the chain patcher takes the previous BO);
0x6040 with all 4 fields; window-coverage gate in supported; maxpool on the PPU
BY DEFAULT (RKT_NO_PPU=1 — fallback to CPU).

**resnet18: 39 → 13 ms** (CPU 345, ×27), the partition has fused (maxpool
no longer breaks the chain), max 1 mean 0.32, top-5 the same, 10/10 stable.
Regression: v1 7 ms/0.03, v2 0.82×3, conv-tiny bit-exact, layer sampling at
baseline, mp tests = CPU accuracy (2-4/hundredths), dmesg clean.

Note: the erroneous branches of Test 60 (walk, dpu_drains) removed; in the kernel
remains the working vendor single-interrupt scheme (last_int_mask) +
test params rocket_desc_walk/rocket_open_mask (default off).
Dump PITFALL: mesa-regcmd-000-000.bin was overwritten by every operation —
now a running dump_nr (I was comparing the vendor conv0 with our conv1 and
nearly went off into a false diff).

## Test 62 (2026-08-24): avgpool via PPU — "almost for free", and so it was

Probe tfwork/gen_ap.py (conv 3×3 14×14 + AvgPool 7×7/s7 + conv 1×1) →
probe-AP.rknn, offline decode:
- OPERATION_MODE_CFG = 0x10: method in bits [1:0] (0=avg, 1=max), [4] —
  the vendor flying bit.
- RECIP_KERNEL_WIDTH/HEIGHT = floor(65536/k): 7 → 9362 (0x2492).
- PADDING_VALUE for avg: 0x7fffc/0x7fff8 (−4/−8?) — not used with pad=0,
  did not copy it.

mesa 3989cf11a7f: pool_avg flag; PPU path for avg when kernel ≤8,
stride_x==stride_y ≤8 and EQUAL in/out quants (chunk without requantization);
otherwise fallback to the old dw-conv lowering (it requantizes via the BS pipeline).
Global avgpool v1/v2/resnet18 now on the PPU (verified by dump: MODE 0x10,
RECIP 9362 in the live v1 stream) — without synthetic dw weights and CBUF.

Regression: v1 7 ms/0.03 ×3, v2 0.82, resnet18 13 ms/0.32 ×3, conv-tiny
bit-exact, mp tests unchanged, dmesg clean.

## Test 63 (2026-08-24): FULLY_CONNECTED — lowering into 1×1 conv

Op FULLY_CONNECTED (resnet18 op30: 512→1000) used to go to the CPU —
rocket did not accept it (UNREACHABLE in count_tensors). The lowering
is trivial: the tflite weight matrix [out, in] is byte-for-byte equal to the conv tensor
OHWI [out, 1, 1, in] — only the dims need permuting; teflon right-aligns
2D into [1,1,out,in]. lower_fully_connected builds a synthetic pointwise
conv and hands it to lower_convolution: large weights (512000 B > 7×64K CBUF)
go to the big-FC streaming of Test 48 on their own. supported condition: per-tensor
quants + input 1×1 spatially (flatten [1,N] passes too —
right-alignment gives [1,1,1,N], the linear order matches).
Frontend: fcon.relu (the field existed, was not filled), guard weights_format
== default and activation ∈ {none, relu}.

mesa 6e23e65e091. resnet18 ENTIRELY on the NPU (partition up to op30 inclusive):
13 ms, diff max 2 mean 0.4, deterministic 5/5; top5 permutation at pos.
4/5 — a CPU tie 211/211, ±1 splits it (the ceiling of Test 51, not a defect).
New tests: layer-fc-small (64→96, small weight path) and layer-fc-relu
(fused relu) — ≤1 LSB (tfwork/gen_fc_tfl.py). Regression: v1 7 ms/0.03,
v2 7 ms/0.82, layer-suite unchanged, the mp56 diff (colgrad 239) is PRE-existing
— verified by a stash rebuild on 3989cf11a7f, a separate PPU maxpool tail.

## Test 64 (2026-08-24): CONCATENATION — concat as addressing

Channel concat in the planar 8-channel layout = pure addressing:
the block of surfaces of each input is self-contained. The output BO is created
at full size; the producer of each input is retargeted to write its slice
(new operation->dst_offset in DST base of the DPU and PPU), requant into the output
domain via OUT_CVT — by the same mechanism as the ADD fuse. Offset granularity
— 16 channels (allocation DIV_ROUND_UP(C,16)·2 surfaces) →
supported requires C_i%16==0 for all inputs except the last.
An input that cannot be written in place (produced outside the partition, read
by other consumers, producer is a PPU pool without a requant stage) —
is copied by an identity-conv into the slice. The retarget breaks find_producer for
readback (several operations share output_index) — a concat_shapes
table was added to the subgraph.

**UNCOVERED ALONG THE WAY: the dw weight layout is correct only for C≥32.** Probes
layer-dw1x1: C=16 — garbage (f0 diff 128, signature "domain shifted"),
C=32 — bit-exact clean. Requant is not to blame (noreq C=16 is broken the same way).
mobilenet dw are all ≥32 — hence it never surfaced. Because of this the identity copy
is made pointwise (C×C identity matrix, q255 diagonal, scale 1/255),
not depthwise. Separate backlog: dw C<32.

mesa 76c6f190f3d. Tests: layer-concat (2 branches, clean zero-copy),
-mixed (graph input → copy), -multi (leg with 2 consumers),
-copy (copies only — BIT-EXACT). All ≤1 LSB. Regression: v1 6 ms/0.03,
v2 7 ms/0.82, resnet18 13 ms/0.4, layer-suite unchanged, FC ≤1 LSB,
dmesg clean. Generator tfwork/gen_concat.py.

## Test 65 (2026-08-24): mp56 tail uncovered — TWO defects, neither in the pool

Probe bisection: the pool is innocent — conv 1×1 8→16 on 56×56 WITHOUT a pool is broken
with the same numbers (mpH). The geometric series mpW40..112 gave a law:
the tail of the map is read from the start, boundary = align(surf,32K)/2... which
turned out to be an arithmetic coincidence with the real root:

**Defect 1 (rkt_task.c, 0a7e0335c7a): CBUF-entries desync.**
The splitter counted entries from the REAL channels (8 → 14/row, 1 bank),
while DATA_SIZE1 declares align(max(C,16),16)=16 → the hardware writes 28/row
→ the bank (1024 entries) runs out at row 36.57 → input rows beyond that
wrap to the start of the bank. The boundary prediction by banks matched across
the whole series: W64→32 rows, W80→51.2, W96→64, W112→73.14. Real networks
were not affected: large maps are always banded (band < bank), 8ch
single-task — only synthetics. Fix: entries from the declared channels
(ARGB 3ch — line path, unchanged). Verification by the vendor: probe-PW56
(conv 1×1 8→16, 56×56) — the vendor writes an HONEST 8 (DATAIN=8, entries 14,
weights 128 B); our padding to 16 is an RK3588 legacy, kept, but the banks
are now counted consistently.

**Defect 2 (rkt_regcmd.c, 0d913913e4a): PPU CUBE_IN without a clip.**
The PPU derives the number of windows from the input cube ROUNDING UP: VALID 3×3
s2 on 56 → cube 56 yields a 28th column into a 27-wide DST → shift (mpC:
f128/chan bits = addressing, not arithmetic). The vendor (probe-MPvalid56)
clips the cube to (out−1)·s+k−pads = 55, RDMA strides — from the full map.
Done the same way. SAME was not affected (consumed == in).

Method: offline decode of vendor .rknn (gen_mp56/gen_mponly/
gen_mpvalid/gen_pw56 + rknn_regcmd.py) — a reboot into the vendor stack was NOT
needed. Pitfall: probe-MPonly (PPU-first task) is not found by the
decoder via the CNA prologue — search by the word 0x40010000000e6004.

Regression: ALL mp ≤4 (was 239/255), v1 7 ms/0.03, v2 7 ms/0.82,
resnet18 13 ms/0.4, concat/FC/layer-suite unchanged, dmesg clean.

## Test 66 (2026-08-24): dw C<32 CLOSED — two desyncs with the vendor

Probes probe-DW{16k1,32k1,16k3,24k3} by offline decode (no reboot needed):
1. **DPU cube**: for dw the vendor declares the FULL 32-group even with
   fewer real channels: ORIG_CHANNEL=real−1, CHANNEL=31, CORE
   0x3014=0x1f. We declared the real ones → the whole pipeline stayed silent, output =
   zero point (the layer-dw1x1 C=16 signature "NPU outputs the constant zp").
   Fix: fill_task, dw → output_channels=align(max(C,32),32).
2. **Weight layout**: the row stride of a tap = align(max(C,16),16) — what
   DATAIN_CHANNEL/WEIGHT_SIZE declare (vendor dw24k3:
   WEIGHT_BYTES=288=9·32; dw16k3: 144=9·16). The legacy generic packer laid out
   the real C: for 8/16 it coincided (equal stride, or the tail in unreadable
   output channels), for 24 — rows of 24 B were read as 32 → a shift of all
   channels after the first tap. Fix: own dw C≤32 branch [tap][channel]
   with a zero tail (rkt_coefs.c).

False move: "DATAIN=real like the vendor" (the vendor dw24 writes 24!) —
broke c8/c24 even worse; our pipeline is consistent around align16, the vendor
around real — the working point is not unique, changes must be made IN PAIRS
(register+layout). Reverted, aligned the layout to our align16.

mesa b885d8042c1. All dw probes BIT-EXACT (c8/16/24 3×3, c16 identity/
requant 1×1, c32). Regression: v1 7 ms/0.03, v2 7 ms/0.82, resnet18
13 ms/0.4, mp/concat/layer-suite unchanged, dmesg clean.
Tail: lower_identity_copy in concat stayed pointwise (C²) — now
dw (C) can be brought back if it becomes noticeable at large C.

## Test 67 (2026-08-24): NPU frequency — it was always 600, now 800

Clock analysis: the MAC array is clocked from SCMI/PVTPLL (clk_scmi_npu,
TF-A), NOT from cru CLK_NPU: raising the cru clock 200→600 (overlay
assigned-clock-rates) did not change the timings at all — the cru branch is decorative
for computation. The actual frequency was always 600 MHz (the TEST code
of gate 2 in rocket_core.c set 600 hard and overwrote the DT-assigned one).

Done: (1) overlay — assigned-clocks += <&scmi_clk 2>, rate 800 MHz
(this is the vendor point: rk3568-rknpu-vendor.dts set 800 at the same
0.85V — all captures were made at 800); (2) rocket_core.c — module param
`scmi_rate` (default 800000000) instead of the hard 600; module rebuilt,
installed into /lib/modules + depmod. Ordering PITFALL: the module's TEST code
executes AFTER the DT assigned-rates and overrides them.
Editing the vdd_npu floor 850→900 mV via overlay did NOT take effect
(the regulator stayed at 850 mV) — reverted, we work at the vendor point
800@0.85V.

Result: v1 5.6 ms (was 6.2), v2 6.4 (6.9), resnet18 10.8 (12.2) —
+10%. From the delta it is visible: the NPU part of resnet18 is ~5.6 ms, the remaining ~6 ms —
CPU packing/unpacking of tensors in teflon (the next optimization target
— it now dominates). Accuracy: bit-exact tests are bit-exact, dmesg
clean. 120 s resnet18 stress — see below.

Addendum to Test 67: 60 s resnet18 stress at 800 MHz — 4852 runs,
0 discrepancies, 38°C, dmesg clean.

## Git cleanup of the mesa branch (2026-08-24)

rk3568-test-session-20260820: 42 → 33 commits. Dropped the TEST+Revert
pairs (CMAC OP_EN, RDMA burst7) and the RKT_SC/RB knobs (negative result —
the knowledge is in Test 51); squash: the five [TEST OVR → real formulas] into
"vendor-exact register formulas", [single-interrupt+PPU groundwork +
PPU default] into one. Content diff against the old branch = exactly the removal
of 48 lines of knobs. Force-push db7ec18b866; backup branch
backup-pre-cleanup-20260824 (locally on m1). Regression after rebase
clean (v1/r18/dw/concat/mp56).

## Test 68 (2026-08-24): AXIS convention — band1 anomaly killed

The "minor band1 tail" uncovered a fundamental defect: the driver read
dims[1] as width, dims[2] as height, while input packing wrote MEMORY
rows (dims[2]) — self-consistent ONLY on square maps. Any
non-square: CNA reads rows with the wrong stride → the whole output is slightly broken
(band1 max 81 — "the old unsolved anomaly"), and the output unpacker with
the same mixed axes WROTE PAST THE USER BUFFER (probe 24×16 — heap
corruption, double free).

Diagnostics: rect series (24×16 crash / 16×24 broken / 24×24 square clean);
false moves — wzp/sign-extension OW (killed by first-16ch: wzp=136, clean),
banding (b1-s1 broken without bands... with bands), weights (vendor
layout probe-B1W byte-for-byte = our formula, 4608/4608).

Fix (mesa d2c731a0b01): width=dims[2] (memory row, vendor
convention — the vendor probe 98×224 declares DATAIN_WIDTH=224),
height=dims[1] (band axis). Packing/unpacking loops inverted
(ARGB, 1ch, 8ch-atomic paths), pool window-fit remapped by axes,
weights kw/kh swapped (square kernels — no change in values),
concat_shape made consistent. rkt_coefs untouched (reads dims itself).

All rect/b1/band1 probes ≤1 LSB. Full regression clean: v1 6 ms, v2
7 ms, r18 12 ms, layer-suite/concat/dw/mp unchanged, dmesg clean.
Along the way: m1 went down (cause lost — the previous boot's journal was
not preserved after the hard power-off), the first rockpi-power cycle
got stuck, the second brought it up. The m1 console now exists: /dev/ttyUSB-m1-console
(CP2102N, 1.5 Mbaud) on droid.

## Test 69 (2026-08-24): concat/pool leftovers — dw copy and pool as first op

Leftover 1 — identity-copy for concat as depthwise (c53a1ee10c4). After
the dw fix for C<32, switched the copy from a pointwise C×C identity
matrix to a 1×1 dw (C weights 0xff, scale 1/255). Immediately surfaced:
dw with C<32 declares align(C,32) channels to the DPU cube and WRITES
the padding surfaces — in the concat output this clobbers the adjacent
slice (layer-concat-mixed: a C=16 copy at offset 0 corrupts the C=32
slice after it, max 9; layer-concat-copy masked it — the second slice
was overwritten later). Gate: dw only when C%32==0, otherwise
pointwise. concat-multi (C=32 copy) exercises the dw path, ≤1 LSB.

Leftover 2 — pool as the first op of a partition (b79f837a162). The PPU
chunk lives in the regcmd BO of the preceding non-pool op; without such
a predecessor (pool first, or pool-after-pool) the pool task was not
compiled at all — segfault of the chain linker on empty tasks
(host-side, NPU untouched; reproduced with the new layer-pool-first
probe). Fix: avgpool in such a position — ordinary dw lowering; maxpool
— insertion of an identity-copy carrier (input → synthetic intermediate
tensor, a slot in tensors is reserved per number of pools, the copy is
exact for equal scale/zp). Probes gen_poolfirst.py: pool-first ≤1,
pool-only BIT-EXACT, apool-first ≤1. Full regression unchanged (v1 6ms,
v2 7ms, r18 12ms).

## Test 70 (2026-08-24): yolo reconnaissance — op composition and first delegate attempt

Models: yolov8n + yolov5nu, int8, imgsz=320, exported with ultralytics
8.4.127. Export is impossible on droid (arm64): the new litert-converter
is an x86-only binary, the old onnx2tf path of ultralytics pulls
dependencies through the dead pypi.ngc.nvidia.com. Exported on a
throwaway DO x86 builder (~30 min, torn down). Files: tfwork/yolo/*.tflite
(droid), tests/ (m1).

Composition of v8n (v5nu is analogous): CONV_2D 64, LOGISTIC 58 + MUL 60
(SiLU = x·sigmoid AFTER EVERY conv), CONCAT 17, MAX_POOL 3 (SPPF),
RESIZE_NEAREST 2 (upsample), ADD 8, SLICE 18, TRANSPOSE 8, PAD 7,
RESHAPE 8, SOFTMAX 1 (DFL), SUB 2, QUANTIZE 32. IO float32,
weights per-channel (126 tensors).

Barriers to the NPU (in decreasing order of pain):
1. SiLU after every conv — without sigmoid on the NPU the partitions
   crumble into single convs (the vendor keeps a LUT — RE not started).
   Alternative — relu variants of the models (retraining).
2. Per-channel weights — supported() cuts per-axis, although per-channel
   BS requantization in hardware is already mastered (remove the gate +
   pass the scales through).
3. int8 signed + float32 IO + QUANTIZE nodes (we have the uint8 path).
4. RESIZE_NEAREST / SLICE / TRANSPOSE / SOFTMAX / SUB / MUL — CPU.

Actual run of v8n through the delegate on m1: teflon took a partition of
2 ops (ADD 171,177→178 + CONCAT 417,416,178→180, int8) and SEGFAULTED
(host-side, /tmp/yolo_probe*.log). A separate bug — to be fixed
independently of yolo (int8 tensors? ADD as first op of a partition — a
twin of pool-first?).

## Test 71 (2026-08-24): INT8 + per-channel CLOSED; standalone ADD and the EW riddle

**INT8 = uint8 with a shift (mesa b53692820ee).** All int8 semantics is
reduced to a domain shift: teflon, when creating tensors, adds 128 to
the zero points and XORs constant data (weights) with 0x80; invoke/read
fold the runtime buffers with the same XOR. Bias int32 is in the acc
domain, untouched. The driver below still lives in pure uint8.

**Per-channel weights — the vendor's two-stage scheme.** Full BS stream
[bias i32×4][ow i16×4][mul u16×4] instead of bias-only;
mul_c = round(2¹⁴·s_wc/s_wmax), OUT_CVT from s_wmax. Registers (per
vendor mobilenet task 4 / probe captures): BS_CFG 0x148, BS_MUL_CFG
0xe01, BS_OW_CFG 0x125 conv / 0x36d dw (OW from the stream, OW_OP 0),
DATA_FORMAT 0xe0, BRDMA DATA_USE 7. MAIN FINDING: **DPU_RDMA
0x5024 = length of the BS stream in 8-byte words − 1** (channels·4/8−1
was good only for bias-only; on the full stream half the channels of
every 32-group came in as ZEROS — the surfaces were not written; found
by diffing against the vendor stream). supported() lets per-axis through
ONLY for conv weights (zp equal; TFLite per-channel is symmetric) and
bias. Probes layer-i8-{conv,dw,c16,c64,c32in,chain}: BIT-EXACT/≤1 LSB.
uint8 regression untouched. Generator: tfwork/gen_int8pc.py.

**Standalone ADD (both inputs from outside the partition) — segfault
killed (ef7af777701).** A yolo partition opening with an ADD: both
find_producer NULL → host NULL deref. Fix: both inputs get identity-conv
producers (input1 → synthetic tensor, its identity also carries the EW
read; input0 → host with requantization into the add domain).
invoke/read learn the packing of the add-fed input (geometry of the
host's OUTPUT, zp of the operand), the BO is created explicitly. yolov8n
no longer crashes (invoke 161ms, convs one at a time between SiLUs).

**EW riddle OPENED** (probes layer-i8-addonly/addfirst,
layer-u8-addonly; gen in tfwork): in the int8 version of the construct
the EW contribution is ZERO (output = pure requant(a), raw byte 0xff
proven by a BO dump); the u8 version is BIT-EXACT on uniforms, but
gradients are broken (7-59) in both. Refuted by diffs: dw host (pw the
same), CPU-vs-NPU EW source (double identity did not help),
EW_CVT_OFFSET 0x80 (the vendor writes the same), registers u8 vs i8
IDENTICAL down to the LSB of OUT_CVT_SCALE. RKT_EWPOKE (EW→weights) DOES
contribute — ERDMA is alive. The difference is only in the data; next —
the replay harness. Probes in code: RKT_COPY_PW, RKT_EWPOKE,
RKT_TRACE_IN.

## Test 72 (2026-08-24): EW riddle CLOSED — two roots (mesa 4c05b9fbe5b)

False trails eliminated methodically: a uniform sweep showed the
arithmetic BIT-EXACT on all values (incl. x<128 and saturation 1.1),
i.e. the error is only spatial; BO dumps (inputs/weights/bias) i8 vs u8
byte-for-byte; registers down to the LSB. Next — RKT_COPY_PW and an
RKT_EWOFF sweep.

**Root 1 — depthwise host fused-add.** The identity-copy serving as the
host of the ADD was dw (C%32==0) → RDMA_FEATURE_MODE_CFG in dw mode
0x7816, EW RDMA reads the second operand with the wrong surface walk:
the first two 8-channel surfaces ~¼ of the range lower on any
non-uniform (chan pattern: channels 0–15 broken, 16–31 clean). Pointwise
host — bit-exact. Fix: the identity host of the ADD is forced pointwise
(force_pointwise parameter), in ADD lowering a dw-conv is not assigned
as host (prefer a pointwise producer later in the chain, otherwise
insert a pointwise identity of the dw output as host). Did not surface
before — all add fuses in v2/r18 sit in pointwise.

**Root 2 — loss of the implicit mantissa bit in OUT_CVT.**
`((bits>>9)&0x7fff)+1`: mantissa 0x3fff rounds to 0x4000,
`|=1<<14` becomes a no-op, value = 2^e·1.0 instead of 2^e·2.0 —
HALF the scale. Caught by any conv_scale "slightly below a power of
two" (0.0039062488 at the identity host of i8-addonly: s_in 1/256,
s_w=s_out=1/255). Symptom: a constant offset 128/72 on all patterns
with correct EW (RKT_MULSH=1 gave bit-exact — the key). Fix: move the
rounding into the exponent (shift−1), same for 0x8000. A latent pitfall
for ANY layers — but in v1/v2/r18 it never fired (verified by
regression: unchanged).

Thrown out: the double identity (the conclusion "EW does not read the
job input" was false — the dw cause), probes
RKT_COPY_PW/EWPOKE/TRACE_IN/EWOFF.
Probes layer-{i8,u8}-add{only,first,half,09,099,11}: ALL BIT-EXACT.
Regression: v1 6ms, v2 7ms, r18 12ms, layer-suite as before.

## Test 73 (2026-08-24): SiLU — the vendor computes on the NPU via DPU LUT (reconnaissance)

Probes: tfwork/gen_silu.py → probe-SILU.rknn (conv3×3→SiLU→conv1×1)
and probe-SILU-CTRL.rknn (ReLU). Verbose build: ConvExSwish Target=NPU,
1872 cycles. Our rknn_regcmd.py saw 1 task — because the LUT tasks have
no CNA prologue. Full scan of the container — 6 regcmd runs:

1. **run@1016 and @1608 (586 words each): LUT loaders.** DPU-only
   (BS/BN 0x53 bypass, EW 0x383, OUT_CVT scale 1), LUT_ACCESS_CFG
   (0x4100) = 0x20000 → 515 writes to LUT_ACCESS_DATA (0x4104) —
   table LE; the second run with CFG=0x30000 → table LO. Two tasks of
   the PC chain (amt 0x1280/0x2500) BEFORE the conv.
2. **run@2200 (135): conv0 + swish.** Differences from an ordinary conv:
   DATA_FORMAT 0x5000; BN_CFG 0x48 (BN active, not bypass);
   BN_MUL 0x4068 = 0x47aa1400; **EW_CFG 0x302** (LUT path instead of
   the 0x383 bypass); LUT_CFG 0x4100=0; 0x4108 = 0x68; **LUT_INFO 0x410c =
   0x50500**; LE_START 0x4110 = 0xffffc000 (−16384), LE_END 0;
   LO_START 0; **LO_END 0x411c = 0x4000**; LE_SLOPE_SCALE 0x4128 =
   0x40320000; LO_SLOPE 0x412c = 0x1a0; OUT_CVT offset 0xffffff9a
   scale 0x4450 shift 0x14.
3. run@2344 (135, with prologue): conv1 ordinary. run@2488/2568 (71):
   DPU-only output copies.

Tables (json: tfwork/silu-lut.json): LE 515 signed — 0, −99…
minimum −1578 …0 (negative branch of swish: x·σ(x) for x<0,
acc domain ±16384); LO 515 — 0,0,32,65…32767 (positive branch,
almost linear ≈x). I.e. LE = "linear-exponential" for x<0, LO =
"linear-only" for x≥0 — the classic NVDLA LUT scheme (in rocket the
registers are already in registers.xml, mesa writes zeros).

Conclusion: SiLU on the NPU is implementable — three pieces of register
knowledge: (a) the format of the LUT-loading task (DPU-only, ACCESS_CFG
0x20000/0x30000 + 515 words), (b) the LUT mode of the conv (EW 0x302,
BN 0x48, INFO/START/END/SLOPE), (c) generation of the tables for the
layer's quantization (NVDLA formulas: offset/scale from
LE_START/LO_END and SLOPE). Cost: 2 extra PC links of 586 words each
per SiLU layer (yolov8n has ~60 → ~70K words of regcmd, acceptable;
tables can be shared between layers with the same quantization).

## Test 74 (2026-08-25): width of PC_TASK_CON.task_number — 12 bits (answer to Boardcon_yang, forum 61651)

- Reason: Boardcon_yang in thread 61651 claims "task_number is 8-bit on RK3568 vs 12 on RK3588".
- Probe: in rocket_core_init after reset, without OP_EN: write TASK_CON=0xfff → read 0xfff;
  write 0x1ff → read 0x1ff (dmesg m1, uptime 79413). Probe reverted, module rebuilt
  clean, regression v1/v2/r18 unchanged.
- Conclusion: the register is 12-bit, as in the vendor (rknpu_drv.c rk356x pc_task_number_bits=12).
  Whether the PC actually counts through >255 tasks — not verified (needs a job of >255 tasks);
  our hw_submit always writes TASK_NUMBER(1), the chain is driven by the PC — truncation cannot happen.

## Branch sanitation (2026-08-25): BACKLOG item 0 closed

- 0.1 mesa 8fb3fa00200: thrown out 19 dead getenv probes, the
  RKT_VENDOR_OVR table, two weight-layout candidates, RKT_PROBE, the dead
  "dw && false" block; TEST comments → statements "RK3568 (RE date): …".
  Regression: u8/i8 suite at the known maxima, v1/v2/r18 unchanged,
  yolov8n_int8 rc=0 (129 ms).
- 0.2 mesa 74c04966edc: src/gallium/drivers/rocket/README-rk3568.md — a map
  for an outsider (data flow, memory conventions, BS stream, table of "pitfalls"
  with test numbers, knobs, methodology, module delta). Copy in rk3568-npu.
- 0.3 rk3568-npu 9241c50: rknn_regcmd.py cuts by the PC tail (0x81 word),
  a stream = the longest run of valid words; kind conv/ppu/lut/dpu;
  reloc records (runtime address patches: S_POINTER+*_BASE_ADDR+PC)
  are discarded (--all). probe-SILU: 9 tasks (2 lut 588 = 73+515 words,
  2 conv, 2 dpu copies 73, 3 dpu tails of the fp16 output). Discovered along
  the way: the container has 27-word conv tasks with FC_DATA_SIZE0/1 (mobilenet
  t8/t9) — FC mode of data loading, previously glued together with neighbours.
  suite.py = u8+i8 (suite_i8.py removed), mob.py in one line + warm-up.
- Note: on m1 the `github` remote of /media/nvme/mesa keeps a PAT in the URL
  in .git/config — replace with a credential helper.

## Test 75 (2026-08-25): SiLU on the NPU CLOSED — DPU LUT, three discoveries on the way

Implementation (mesa 5fd254e29ca): teflon recognizes CONV→LOGISTIC→MUL
(silu_pattern, the only readers of the conv output), rocket fuses it into
the conv: BN_MUL (x_lut = acc·mul>>shift, the FIELD = shift, same for
per-channel and per-tensor), EW_CFG 0x302, LUT_INFO 0x50500, OUT_CVT scale
= s_lut/(2·s_out).  One LUT domain for all layers at first: x∈[−8,8),
1/2048 per unit, y = 2·silu(x).  Probes layer-silu{,-i8,-pt,2}: max 2 LSB;
yolov8n — all 57 SiLU on the NPU.

1. **The PC LUT loaders are unreliable.**  The vendor layout (2 DPU-only
   tasks, CFG + 515 data words) — only the FIRST DPU-only task of a chain
   "arrives" (matrix el/le/Eel/edl/…: `el`→LE ok LO garbage, `le`→LO ok,
   `Eel`→nothing); the second sometimes lands partially (idx<~100).
   Delays/retries do not help, a stream >1024 words hangs the PC (per-task
   limit), S_POINTER ≠0xe hangs, FLYING_MODE=0 hangs (RDMA from address
   0).  An MMIO write from the kernel (DPU idle) is stable 3/3.  Solution:
   uapi `lut_data/lut_count` (2×515 words, vendor framing [0][513][t1]),
   the kernel writes through dpu_iomem before hw_submit.  Reading the LUT
   back through ACCESS_CFG/DATA over MMIO does NOT return the contents.
2. **The first ~5 entries of the LO table are broken**: for x∈[LO_START,
   +~160) the block outputs LO[512]·(x+8)/256 (spike tables: only LO[512]
   has an effect).  Workaround: LO_START=−512 (inside the LE range) +
   LUT_CFG 0x28 (HYBRID_PRIORITY=0 → LE wins in the overlap); with 0x68
   (vendor) LO took the overlap.  LE_END/framing/LUT_INFO are not involved.
3. **RKT_MULSH also changes the per-channel BS multiplier** (rkt_coefs:
   2^(14−n)) — ramp measurements with MULSH gave "field = e−5/e−3"; without
   the knob the field = e.  Ramp tables (LE=i, LO=−i) + spikes — the
   method that uncovered everything.
The uapi changed (kernel+mesa together): module/rocket_job.{c,h},
rocket_accel.h.  Cost: 0 extra tasks in the chain; the LUT is loaded by the
kernel on every job (~1030 MMIO writes, ≈50 µs).

## Test 76 (2026-08-25): yolov8n ENTIRELY on the NPU — detections match, 27 ms vs 69 CPU

The chain after SiLU (Test 75).  Graph breaks in yolov8n_int8: QUANT×31
(requant of CONCAT legs), SLICE×16 (C2f split), PAD×7 (before s2 convs), +
the tail.  Implemented in mesa (teflon+rocket):
1. **Views** (`rkt_view`): a channel SLICE on an 8-boundary is pure
   addressing: the consumer gets `src_offset` (surfaces) and
   `src_channels`; PAD — the consuming conv gets +padding and the source
   dims.  Resolved in lower_convolution (identity copies and pooling too);
   the fused-add operand as well (`add_src_offset` in EW_BASE_ADDR).  A
   partition may export a view — read_outputs reads the source at the
   offset.
2. **QUANT int8→int8**: if the producer is in the partition and the QUANT
   is its only consumer → retarget the producer (OUT_CVT into the new
   domain, writes the output tensor directly; `orig_output_*` kept for a
   CPU requant if the old tensor still leaves the partition); otherwise a
   pointwise identity conv.
3. teflon: builtin SLICE → STRIDED_SLICE (begin/size), begin/end masks of
   StridedSlice; `pipe_tensor.dims_count` — rocket takes 4D only (the 3D
   tail [1,84,2100] was right-aligned and read as a 1×80 map with 1600
   channels — scores went to zero).
Defects found on the way:
- **Bands with a top pad** (layer-big-first-pad 320×320): the last row was
  garbage — the consumed formula subtracted pad_bottom, and at an odd phase
  (top pad 1, s2) no bottom padding is needed → the band under-read row
  319.  Fix: subtract pad_top only.
- **A SiLU conv cannot host a fused add** (the EW stage is taken by the
  LUT): the yolo bottleneck add(b, silu(conv(silu(conv(b))))) → as with a
  depthwise host, a pointwise identity copy hosts the add.  layer-bneck:
  180 → 3 LSB.
- **Width >2047** (the DFL conv 1×1 over 4×2100): DATAIN/DATAOUT_WIDTH are
  11-bit fields → garbage (layer-w2100-c1/c8 vs layer-w300 clean).  Gate:
  dims[1..2] ≤ 2047, else CPU.
- **LUT framing** (from Test 75, finished): entry k = word k (no leading
  zero): SiLU probes max 2 → max 1 LSB, mean 0.2; c2f max 3.
Result: yolov8n_int8 on bus.jpg — 5 detections CPU = 5 NPU (same
boxes/classes/scores), raw output mean diff 0.0003; NPU 27.4 ms vs CPU 69
ms (XNNPACK, 4 cores).  Left on the CPU: the input QUANT float→int8,
TRANSPOSE/RESHAPE, the 3D DFL tail (SOFTMAX, conv 4×2100, 3D
SLICE/SUB/ADD/MUL/CONCAT), DEQUANT.  RESIZE×2 (upsample) unsupported,
splits the neck into 2 partitions: next candidate.
Probes: layer-{quantcat,pad,slice,c2f,c2f-add,c2f-add-out,bneck,first-*,
big-first-*,dfl,w2100-*,w300-*}; tests/yolo_cmp.py (bus.jpg, numpy NMS).
Method: TEFLON_MAX_OPS bisection along SiLU-triple boundaries (LOG+MUL in
one partition, else a segfault in read_outputs — an artifact of the knife),
then the smallest probe of the factor.

### Test 76, addendum: a residual race in the PC chain (open)
Commits: mesa 5996173fbd7, rk3568-npu 13c943f.  layer-c2f-add flakes: ~2%
of fresh processes, one pixel of row 0 (random column, all cv2 channels).
Observed:
- with the LUT rewritten over MMIO on every job — 26/160 (a race with the
  running chain → the LUT is loaded before the PC starts + readl + 50 µs
  settle, identical tables are not rewritten; the cache is invalidated on
  reset and runtime resume, otherwise the LUT is empty after a suspend —
  silu-c32 80/80 garbage);
- after that 0/160 within one process's loop, but ~2% in fresh processes,
  and not only the first invoke (a 2000 µs settle changes nothing);
- RKT_NO_CHAIN=1 (per-op jobs with a fence) → 40/40 clean ⇒ a race inside
  the chain: the consumer (cv2, 1×1) reads the concat BO while the write of
  a tiny producer (identity copy 28×28×16) is not visible yet; "row 0, one
  column" = the write front.  The vendor puts 73-word DPU-only tasks
  between operations (8px×32ch, OUTPUT_MODE=4) — a drain-barrier candidate.

### Test 76, the chain race — what was tried the same evening (OPEN, parked)
- Barrier = a DPU-only task (the vendor's 73-word skeleton, flying): does
  not complete without CORE data and EATS the first pixels of the next task
  (deterministic garbage max 87) — this also explains the lost PC LUT
  loaders (Test 75).  DPU-only tasks without an RDMA input cannot sit in a
  chain.
- Barrier = a tiny 1×1×8 identity conv between producer and consumer: does
  not remove the flake (1/40).  Descriptor int_mask (0x100/0x200/0x3ff) —
  the chain proceeds at any value: the PC does not wait for done, only for
  unit busy (consistent with Test 61: the vendor walks by tails, no
  descriptors).
- Exact picture (c2f-add-out, output = concat): 1/40 — row 0, one column,
  channels 12–13 = bytes 4–5 of ONE 8-byte pixel of leg a (identity copy
  cv1[0:16] → concat).  Not a whole pixel — a partial WDMA write / byte
  merge when tasks overlap.  The producer is tiny (28×28×16).
- yolov8n: 20/20 fresh processes bit-identical (maps ≥10×10×256).
  RKT_NO_CHAIN=1 clean 40/40.  The harm is limited to tiny graphs.
Ideas for later: (a) remove the copies themselves — cv1 writes straight
into the concat BO (legs a,b = ordered slices of one tensor → dst_offset at
cv1, m0 reads b as a view of the concat BO) — also −2 tasks per C2f; (b)
find out whether the WDMA writes bytewise (the int8 per-channel BS path?)
and whether the DPU has a "write drain" register.

## Test 77 (2026-08-25): RESIZE nearest ×2 on the NPU — yolov8n 18 ms (2 partitions)

Reconnaissance: tfwork/gen_upsample.py (rknnvenv) → probe-UP.rknn /
probe-UP-CTRL.  Vendor: Upsample = one DPU-only task per 8-channel surface
(73 words, the same skeleton as the "copies" and LUT loaders): the RDMA
reads the Win×Hin surface in "unpooling" mode (RDMA_SRC_DMA_CFG 0x1249:
KERNEL 2×2, STRIDE 2, UNPOOLING_EN=1; RDMA_FEATURE_MODE_CFG 0xc001 —
FLYING_MODE=1 in DPU-only tasks means "input from the RDMA", not "from
CORE": our earlier reading was inverted), the DPU writes 2W×2H:
DATA_CUBE_WIDTH=2W−1, **DATA_CUBE_HEIGHT=Hin−1** (unpooling doubles the
rows; with 2H−1 the DPU waits for data forever → every job times out, the
second surface stays empty), NOTCH_ADDR=2W|2W<<16, SURFACE_ADD=2W·2H·8,
DST_SURF_STRIDE=2W·8, BS_OW_CFG 0x126, DATA_FORMAT 0x04000000, tail 0x18.
Implementation: rkt_operation.is_upsample, a task per surface
(fill_upsample_regcmd, the vendor task skeleton with addresses/sizes
patched), enable 0x18 / int 0x300.  A QUANT after a pool/upsample is an
identity copy (a DPU copy cannot requantize; the retarget broke yolo's
scores).
Result: layer-up/-out/-c24 ≤1 LSB; yolov8n: 4 partitions → 2, 27 → 18 ms,
detections = CPU, raw mean diff 3e-4.  Left on the CPU: only the input
QUANT float→int8 + TRANSPOSE and the 3D DFL tail.

## Test 78 (2026-08-25): performance tails — yolov8n 18 → 14.9 ms

RKT_PROF=1 (invoke phases) showed the NPU computing for 8 ms and the rest
being CPU around it: (1) a pipe_context was created per invoke (~2.5 ms) →
one per partition; (2) bytewise packing/unpacking → 8-byte words per pixel;
(3) an mmap of the BO per map (a leak) → cpu_map lives with the BO; (4)
one LUT domain for all layers ([−8,8)) saturated the coarse layers → a
domain per job from the x_max of its SiLU hosts (mesa 4fdbdba48f0,
RKT_OPS=1 — operation dump); (5) the input QUANT float→int8 + TRANSPOSE
and the output TRANSPOSE+RESHAPE pairs (yolo exports NCHW) — ~1.5 ms of
CPU between the delegate and the user buffers → views: the input is read
from the float NCHW buffer and quantized while packing (NEON, 16 px per
step for C=3), the outputs are written as planar CHW (mesa 7b1bb194076).
Pitfalls: (a) the RESHAPE view took its channels from dims[3] of its NCHW
input (=W) — channels 20–23 came out zero; only the TRANSPOSE (NHWC input)
knows the channel count; (b) the pattern function in partition_init (plan
== NULL) returned true for EVERY QUANT — yolo's 32 requantizing QUANTs
became "input views" (23 detections); without a plan only local criteria
(float32→int8; perm 0,2,3,1); (c) scalar lrintf over 307k floats — 4.6 ms,
NEON vcvtnq+vst3q — 0.9; (d) a TEFLON_MAX_OPS window cutting a partition
down to views only — segfault → a partition without operations does
nothing.
Result: yolov8n 14.9 ms (pack 1.8, wait 7.9, unpack 0.9; the remaining ~4
ms is the 3D DFL tail on the CPU, not low-hanging); v1 5.4, v2 6.3, r18
11.2 ms; detections = CPU; regression at the baseline.  Not taken: C2f
without copies (retargeting cv1 into the concat BO broke yolo, gain ~0.2
ms), the chain race (2% on tiny graphs, yolo 20/20 identical).

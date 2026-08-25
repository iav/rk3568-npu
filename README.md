# RK3568 NPU on the open stack (rocket + Mesa Teflon)

Working setup for the Rockchip RK3568 NPU on ODROID-M1 using only open
components: the mainline `accel/rocket` kernel driver (with local fixes)
and a Mesa Teflon (TFLite delegate) branch. No librknnrt, no vendor
runtime.

Status (2026-08-25):

- single-operation probe layers (regular, depthwise, stride-2,
  3-channel first layer, fully-connected, concat, standalone add —
  uint8 and int8, per-tensor and per-channel weights) match the TFLite
  CPU reference within 1 LSB across uniform fills, row/column
  gradients, channel-coded and mixed inputs;
- full mobilenet_v1 (224×224 quant) runs on the NPU — convolutions,
  depthwise, the FC-shaped final layer and average pooling: ~5.6 ms vs
  117 ms CPU-only on the same board (~21×), top-5 matching the CPU
  reference, mean absolute logit difference 0.03, bit-identical results
  across repeated runs;
- full mobilenet_v2 (224×224 quant) runs as well, including all ten
  fused residual additions: ~6.4 ms vs 76 ms CPU-only (~12×), top-5
  matching the CPU reference including order, mean absolute logit
  difference 0.82, bit-identical across runs;
- resnet18 (224×224 quant) runs end-to-end in a single NPU partition —
  convolutions, residual adds, both poolings and the final
  FULLY_CONNECTED layer: ~10.8 ms vs 345 ms CPU-only (~32×), max pooling
  and global average pooling executed by the NPU's dedicated PPU unit;
- channel CONCATENATION is delegated as pure addressing (each input's
  producer writes its own slice of the output buffer), with an identity
  convolution copy as the fallback when a leg cannot be written in
  place;
- INT8 tensors are handled without any hardware change: the delegate
  shifts int8 zero points by 128 and XORs the data with 0x80, so the
  NPU always sees uint8; per-channel weight quantization is expressed
  through the DPU BS stream (a per-channel multiplier relative to the
  largest channel scale) — this is the format most int8 detection
  models ship in;
- standalone ADD (no convolution producer inside the partition) runs on
  a pointwise identity-copy host; a depthwise host reads the second
  operand with the wrong surface walk and is never chosen;
- the MAC array runs at 800 MHz (the vendor's own RK3568 operating
  point; the driver default used to be 600 MHz — see the `scmi_rate`
  module parameter below), stress-tested for an hour with bit-identical
  outputs throughout;
- the whole delegated graph is submitted as ONE job: every task's
  command-stream tail is patched to chain into the next one and the
  NPU's PC unit walks the chain in hardware, raising a single interrupt
  at the end (the vendor driver's submit model; per-job overhead is
  ~0.2 ms, which is where most of the mobilenet speedup over the
  earlier per-task submit came from);
- for all three networks only reshape and softmax stay on the CPU.

RK3568 differs from the RK3588 path already in Mesa. The differences,
derived by byte-level comparison against captured vendor command
streams, are described in the Mesa commit messages (see below): 8-channel
feature atomics (all CNA/DPU strides in 8-byte units), 8×32 KiB CBUF
banks, a different weight layout (16-kernel tap-major groups with
compact tails for partial input-channel slices and kernel groups),
32-channel group tasks for wide depthwise, a packed-RGB first-layer
mode, weight streaming for FC-shaped layers, PC-driven task chaining,
the DPU BS-stream requantization, a FEATURE_GRAINS prefetch formula
that grows on narrow feature maps, the element-wise (residual add)
unit configuration — including the RDMA surface-notch addressing that
band-split add tasks need — and the PPU/PPU_RDMA pooling programming
(max and average), all taken from vendor resnet18 and probe-model
captures. Two PPU pitfalls worth writing down: the PPU register chunk
must live in the same BO as the preceding task's command stream, and
POOLING_PADDING_CFG (0x6040) carries all four paddings — a pooling
window not covered by input+padding starves the unit forever and
stalls the whole PC chain.

Three more hard-won conventions from the later sessions: CBUF entry
accounting follows the DECLARED channel count (align 32), so a
depthwise task with C < 32 both declares and pads to a 32-channel
group; the PPU input cube must be clipped to the rows/columns actually
consumed, or the window starves; and the axis convention is
width = dims[2] (the contiguous memory row) — the driver used to mix
the two spatial axes, which was self-consistent only on square feature
maps and corrupted every non-square one.

## Components

| dir | contents |
|---|---|
| `module/` | out-of-tree build of the `rocket` kernel driver with RK3568 fixes (fork of the in-tree driver from 7.2-rc7) |
| `overlay/` | device-tree overlay enabling the NPU on ODROID-M1 |
| `mesa-patches/` | the Mesa branch as a patch series (also pushed as a branch, see below) |
| `tests/` | probe-layer test harness (`suite.py`, `suite_i8.py` for int8 probes), mobilenet comparison (`mob.py`), single-layer oracle (`run7.py`), vendor-capture replay tool (`replay.c`) |
| `models/` | pre-generated quantized single-layer .tflite probes |
| `capture/` | LD_PRELOAD shim used to capture vendor command streams for comparison (needs the vendor stack, only for further RE work) |
| `tools/` | `rknn_regcmd.py` (extract + decode the vendor command stream from a .rknn), `rknn_diff.py` (diff an `RKT_DUMP` stream against it) |
| `README-rk3568.md` | map of the driver for the next reader: data flow, memory conventions, BS-stream requantization, the load-bearing settings with their lab-log test numbers, debug knobs (copy of the file in the Mesa branch) |

Mesa branch: https://github.com/iav/mesa tree `rk3568-test-session-20260820`
(based on mesa upstream + the RK3568 draft from
https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/42134).

## Reproduction

Hardware: ODROID-M1. Tested with 8 GB RAM.

### 1. Kernel

Armbian `bleedingedge` rockchip64 kernel 7.2-rc7 (the exact .deb set used
is attached to the GitHub release of this repo). Boot requirements in
`/boot/armbianEnv.txt`:

```
extraargs=cma=256M
user_overlays=rk3568-npu-test
```

The NPU cannot reach physical memory above 4 GiB. Two patches cover this:
page tables are constrained by a GFP_DMA32 patch already included in the
released kernel .debs (`iommu_data_ops_v2` in `rockchip-iommu.c`), and BO
pages by a DMA32 gfp mask in `module/rocket_gem.c`. With both in place
the full 8 GB stays usable. If you run an unpatched module, add `mem=4G`
to `extraargs` instead — otherwise jobs die with `DMA_READ_ERROR`.

Build and install the overlay:

```
armbian-add-overlay overlay/rk3568-npu-test.dts   # or dtc -O dtb manually
```

### 2. Kernel module

The in-tree `rocket` driver loads but wedges on back-to-back jobs
(ping-pong register-bank state machine). The `module/` fork adds a core
reset + manual IOMMU re-arm on the wedge condition, plus register dump
helpers:

```
cd module
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo rmmod rocket && sudo insmod ./rocket.ko
```

`/dev/accel/accel0` should appear.

Note: if the kernel already auto-loads a `rocket` module from
`/lib/modules`, replace that copy and run `depmod` — an `insmod` next to
a loaded stale build silently keeps the old code. `rmmod` only while the
NPU is runtime-suspended.

The module and the Mesa branch extend the uapi together (a
`last_int_mask` field in `struct drm_rocket_job` for the
single-interrupt chained submit) — build them from matching revisions
of this repo and the branch below.

The MAC-array clock (TF-A PVTPLL via SCMI) defaults to 800 MHz, the
vendor DT's RK3568 operating point at the same 0.85–0.9 V NPU supply.
`scmi_rate=<Hz>` on the module command line overrides it (600 MHz was
the old hardcoded value; the speedup at 800 is ~10%, the bottleneck at
this point is CPU-side tensor packing).

### 3. Mesa / Teflon

```
git clone -b rk3568-test-session-20260820 https://github.com/iav/mesa
cd mesa && meson setup build -Dgallium-drivers=rocket -Dteflon=true
ninja -C build src/gallium/targets/teflon/libteflon.so
```

### 4. Run

Python venv with `tflite-runtime` (or `ai-edge-litert` on newer python) and
numpy. Then:

```
python tests/suite.py models/layer-c28.tflite      # per-layer probes vs CPU
python tests/mob.py                                 # mobilenet_v1 CPU vs NPU
```

`suite.py` prints `max/mean` absolute difference against the CPU
reference for 7 input patterns; every bundled probe layer should show
max ≤ 1. `mob.py` needs `mobilenet_v1_1.0_224_quant.tflite` (the classic
TFLite hosted model); point it at another model with
`MOBILENET=<path>` — `mobilenet_v2_1.0_224_quant.tflite` works too.

Debug knobs: see the table in `README-rk3568.md` (`RKT_DUMP`, `RKT_WDUMP`,
`RKT_NO_CHAIN`, `RKT_NO_PPU`, `RKT_MULSH`, `RKT_MAXROWS`, `RKT_POOL_INT_MASK`,
`ROCKET_DEBUG=dump_bos`, `TEFLON_MAX_OPS`/`TEFLON_MIN_OPS`).

## Known limitations

- Residual ±1-per-layer rounding drift (the 15-bit OUT_CVT scale and
  the hardware rounding pipeline vs the CPU's int32 requant path; see
  the commit log for the precision-ceiling experiments).
- PPU pooling covers kernels ≤ 8, equal x/y strides ≤ 8 and equal
  input/output quantization; other average poolings fall back to a
  depthwise-convolution lowering (zero-padding cases only). A max pool
  with no preceding NPU operation to host its PPU chunk gets an
  identity-copy carrier inserted (exact copy, one extra task).
- Concatenation is delegated for axis -1 (channels), equal spatial
  dims, and every leg but the last a multiple of 16 channels; a graph
  with an unsupported concat keeps the whole partition off the NPU.
- Softmax and reshape are not delegated.
- Only convolution/depthwise/add/pooling/concat/fully-connected with
  fused ReLU/ReLU6 (via output saturation); uint8 or int8 activations
  (per-tensor), per-tensor or per-channel weights. No other
  activations yet — the vendor stack runs SiLU and friends through a
  DPU lookup table, which is understood but not implemented.
- Timings above are at the NPU clock pinned to 800 MHz via SCMI;
  "bit-identical" means same board, fixed clock, repeated runs.
- Debug/rollback knobs: `RKT_NO_CHAIN=1` (per-task submit),
  `RKT_NO_PPU=1` (pooling on CPU / dw-conv path).

## References

- Forum thread for this work (discussion, updates):
  https://forum.armbian.com/topic/61651-odroid-m1-rk3568-npu-on-the-open-stack-rocket-kernel-driver-mesa-teflon/
- Mesa RK3588 rocket/teflon MR (base of this work):
  https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/42134
- Vendor-stack setup on the same board (rknpu DKMS + RKNN runtime, incl.
  the >4 GB IOMMU discussion):
  https://forum.armbian.com/topic/58036-odroid-m1-npu-fully-working-on-armbian-618x/
- Vendor kernel driver sources used for reference (kiln):
  https://github.com/gahingwoo/kiln
- rknn-toolkit2 (only needed to build .rknn probe models for further RE,
  not for running the open stack):
  https://github.com/airockchip/rknn-toolkit2
- Armbian: https://www.armbian.com

## Licenses

`module/` is GPL-2.0 (fork of the in-tree driver, original copyright
Tomeu Vizoso and contributors). `mesa-patches/` follow Mesa's MIT
license. Everything else in this repo: MIT.

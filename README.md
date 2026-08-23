# RK3568 NPU on the open stack (rocket + Mesa Teflon)

Working setup for the Rockchip RK3568 NPU on ODROID-M1 using only open
components: the mainline `accel/rocket` kernel driver (with local fixes)
and a Mesa Teflon (TFLite delegate) branch. No librknnrt, no vendor
runtime.

Status (2026-08-23):

- single-convolution probe layers (regular, depthwise, stride-2,
  3-channel first layer, FC-shaped final layer) match the TFLite CPU
  reference within 1 LSB across uniform fills, row/column gradients,
  channel-coded and mixed inputs;
- full mobilenet_v1 (224×224 quant) runs on the NPU — convolutions,
  depthwise, the FC-shaped final layer and average pooling: 9 ms vs
  117 ms CPU-only on the same board, top-5 matching the CPU
  reference, mean absolute logit difference 0.03, bit-identical results
  across repeated runs;
- full mobilenet_v2 (224×224 quant) runs on the NPU as well, including
  all ten fused residual additions: 14 ms vs 76 ms CPU-only, top-5
  matching the CPU reference including order, mean absolute logit
  difference 0.82, bit-identical across runs;
- the task chain is driven by the NPU's PC unit in hardware (the vendor
  driver's submit model); only reshape and softmax stay on the CPU.

RK3568 differs from the RK3588 path already in Mesa. The differences,
derived by byte-level comparison against captured vendor command
streams, are described in the Mesa commit messages (see below): 8-channel
feature atomics (all CNA/DPU strides in 8-byte units), 8×32 KiB CBUF
banks, a different weight layout (16-kernel tap-major groups with
compact tails for partial input-channel slices and kernel groups),
32-channel group tasks for wide depthwise, a packed-RGB first-layer
mode, weight streaming for FC-shaped layers, PC-driven task chaining,
the DPU BS-stream requantization, a FEATURE_GRAINS prefetch formula
that grows on narrow feature maps, and the element-wise (residual add)
unit configuration — including the RDMA surface-notch addressing that
band-split add tasks need — taken from vendor resnet18 and probe-model
captures.

## Components

| dir | contents |
|---|---|
| `module/` | out-of-tree build of the `rocket` kernel driver with RK3568 fixes (fork of the in-tree driver from 7.2-rc7) |
| `overlay/` | device-tree overlay enabling the NPU on ODROID-M1 |
| `mesa-patches/` | the Mesa branch as a patch series (also pushed as a branch, see below) |
| `tests/` | probe-layer test harness (`suite.py`), mobilenet comparison (`mob.py`), single-layer oracle (`run7.py`), vendor-capture replay tool (`replay.c`) |
| `models/` | pre-generated quantized single-layer .tflite probes |
| `capture/` | LD_PRELOAD shim used to capture vendor command streams for comparison (needs the vendor stack, only for further RE work) |

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

Debug knobs: `TEFLON_MAX_OPS`/`TEFLON_MIN_OPS` (delegate a window of graph
nodes), `RKT_DUMP=1` (print the emitted command stream), `RKT_WDUMP=<f>`
(dump the packed weight buffer), `ROCKET_DEBUG=dump_bos`, `RKT_MAXROWS`.

## Known limitations

- Residual ±1-per-layer rounding drift (the 15-bit OUT_CVT scale and
  the hardware rounding pipeline vs the CPU's int32 requant path; see
  the commit log for the precision-ceiling experiments).
- Average pooling is delegated for zero-padding cases only; softmax and
  reshape are not delegated.
- Only convolution/depthwise/add (+fused ReLU6 via output saturation)
  and quantized uint8 tensors; per-axis weight quantization is untested.

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

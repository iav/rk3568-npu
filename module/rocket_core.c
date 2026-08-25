// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#include "rocket_core.h"
#include "rocket_drv.h"
#include "rocket_device.h"
#include "rocket_job.h"

/* MAC-array clock (TF-A PVTPLL via SCMI id 2).  The vendor OPP table for
 * this rail runs to 1 GHz; 800 MHz is what the vendor DT assigns on
 * RK3568 (rk3568-rknpu-vendor.dts) at the same 0.85-0.9 V supply. */
static unsigned long rocket_scmi_rate = 800000000;
module_param_named(scmi_rate, rocket_scmi_rate, ulong, 0444);
MODULE_PARM_DESC(scmi_rate, "NPU SCMI/PVTPLL clock rate in Hz (default 800000000)");

int rocket_core_init(struct rocket_core *core)
{
	struct device *dev = core->dev;
	struct platform_device *pdev = to_platform_device(dev);
	u32 version;
	int err = 0;

	core->resets[0].id = "srst_a";
	core->resets[1].id = "srst_h";
	err = devm_reset_control_bulk_get_exclusive(&pdev->dev, ARRAY_SIZE(core->resets),
						    core->resets);
	if (err)
		return dev_err_probe(dev, err, "failed to get resets for core %d\n", core->index);

	/* TEST: the upstream driver leaves these NULL, which makes clk_bulk_get
	 * return clocks[0] four times instead of the four named clocks.
	 */
	core->clks[0].id = "aclk";
	core->clks[1].id = "hclk";
	core->clks[2].id = "npu";
	core->clks[3].id = "pclk";

	err = devm_clk_bulk_get(dev, ARRAY_SIZE(core->clks), core->clks);
	if (err)
		return dev_err_probe(dev, err, "failed to get clocks for core %d\n", core->index);

	core->pc_iomem = devm_platform_ioremap_resource_byname(pdev, "pc");
	if (IS_ERR(core->pc_iomem)) {
		dev_err(dev, "couldn't find PC registers %ld\n", PTR_ERR(core->pc_iomem));
		return PTR_ERR(core->pc_iomem);
	}

	core->cna_iomem = devm_platform_ioremap_resource_byname(pdev, "cna");
	if (IS_ERR(core->cna_iomem)) {
		dev_err(dev, "couldn't find CNA registers %ld\n", PTR_ERR(core->cna_iomem));
		return PTR_ERR(core->cna_iomem);
	}

	core->core_iomem = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(core->core_iomem)) {
		dev_err(dev, "couldn't find CORE registers %ld\n", PTR_ERR(core->core_iomem));
		return PTR_ERR(core->core_iomem);
	}

	dma_set_max_seg_size(dev, UINT_MAX);

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(core->rdev->variant->dma_bits));
	if (err)
		return err;

	/* TEST: power up the NPU rail; upstream rocket never touches npu-supply. */
	core->npu_supply = devm_regulator_get(dev, "npu");
	if (IS_ERR(core->npu_supply))
		return dev_err_probe(dev, PTR_ERR(core->npu_supply),
				     "failed to get npu-supply\n");

	err = regulator_set_voltage(core->npu_supply, 850000, 1000000);
	if (err)
		return dev_err_probe(dev, err, "failed to set npu-supply voltage\n");

	err = regulator_enable(core->npu_supply);
	if (err)
		return dev_err_probe(dev, err, "failed to enable npu-supply\n");

	dev_info(dev, "npu-supply enabled at %d uV\n",
		 regulator_get_voltage(core->npu_supply));

	/* TEST: the vendor DT lists <&scmi_clk 2> ("scmi_clk") among the NPU
	 * clocks and the BSP enables it; mainline never touches it
	 * (clk_scmi_npu sits at enable_count 0).  If the MAC array is clocked
	 * from the TF-A/PVTPLL branch this would explain CNA reading data
	 * while CORE/DPU never finish.  Enable it here.
	 */
	{
		struct of_phandle_args clkspec = {};

		clkspec.np = of_find_node_by_path("/firmware/scmi/protocol@14");
		clkspec.args_count = 1;
		clkspec.args[0] = 2;
		if (clkspec.np) {
			struct clk *scmi_clk = of_clk_get_from_provider(&clkspec);

			of_node_put(clkspec.np);
			if (!IS_ERR(scmi_clk)) {
				int cerr = clk_prepare_enable(scmi_clk);
				int rerr = clk_set_rate(scmi_clk, rocket_scmi_rate);

				dev_info(dev,
					 "TEST scmi npu clk: enable=%d set_rate=%d rate=%lu\n",
					 cerr, rerr, clk_get_rate(scmi_clk));
			} else {
				dev_info(dev, "TEST scmi npu clk: get failed %ld\n",
					 PTR_ERR(scmi_clk));
			}
		} else {
			dev_info(dev, "TEST scmi npu clk: no scmi node\n");
		}
	}

	/* TEST: map the NPU MMU register block (owned by rk_iommu, so plain
	 * ioremap, not a resource claim) for the AUTO_GATING bit31 workaround.
	 */
	core->mmu_iomem = ioremap(0xfde4b000, 0x40);
	/* TEST (iav RE, 2026-08-22): DPU (0x4000) + DPU_RDMA (0x5000) blocks are
	 * not in the DT reg entries; map them for S_POINTER pp maintenance. */
	core->dpu_iomem = ioremap(0xfde44000, 0x2000);
	core->top_iomem = ioremap(0xfde48000, 0x100); /* TEST: vendor top DMA counters */
	if (!core->mmu_iomem)
		dev_warn(dev, "TEST: could not map NPU MMU regs\n");

	core->iommu_group = iommu_group_get(dev);

	err = rocket_job_init(core);
	if (err) {
		iommu_group_put(core->iommu_group);
		core->iommu_group = NULL;
		return err;
	}

	pm_runtime_use_autosuspend(dev);

	/*
	 * As this NPU will be most often used as part of a media pipeline that
	 * ends presenting in a display, choose 50 ms (~3 frames at 60Hz) as an
	 * autosuspend delay as that will keep the device powered up while the
	 * pipeline is running.
	 */
	pm_runtime_set_autosuspend_delay(dev, 50);

	pm_runtime_enable(dev);

	err = pm_runtime_resume_and_get(dev);
	if (err) {
		rocket_core_fini(core);
		return err;
	}

	/* TEST: mimic the vendor driver, which resets the core before use. */
	rocket_core_reset(core);

	{
		u32 raw_version = rocket_pc_readl(core, VERSION);
		u32 raw_version_num = rocket_pc_readl(core, VERSION_NUM);

		dev_info(dev, "raw VERSION=0x%08x VERSION_NUM=0x%08x\n",
			 raw_version, raw_version_num);

		version = raw_version + (raw_version_num & 0xffff);
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(dev, "Rockchip NPU core %d version: %d\n", core->index, version);

	return 0;
}

void rocket_core_fini(struct rocket_core *core)
{
	/* TEST: release the cross-job attached domain */
	if (core->attached_domain) {
		iommu_detach_group(core->attached_domain->domain,
				   core->iommu_group);
		rocket_iommu_domain_put(core->attached_domain);
		core->attached_domain = NULL;
	}
	if (core->dpu_iomem) {
		iounmap(core->dpu_iomem);
		core->dpu_iomem = NULL;
	}
	if (core->mmu_iomem) {
		iounmap(core->mmu_iomem);
		core->mmu_iomem = NULL;
	}

	if (!IS_ERR_OR_NULL(core->npu_supply))
		regulator_disable(core->npu_supply);

	pm_runtime_dont_use_autosuspend(core->dev);
	pm_runtime_disable(core->dev);
	iommu_group_put(core->iommu_group);
	core->iommu_group = NULL;
	rocket_job_fini(core);
}

void rocket_core_reset(struct rocket_core *core)
{
	reset_control_bulk_assert(ARRAY_SIZE(core->resets), core->resets);

	udelay(10);

	reset_control_bulk_deassert(ARRAY_SIZE(core->resets), core->resets);

	/* The DPU lookup tables are gone with the reset. */
	core->lut_valid = false;
}

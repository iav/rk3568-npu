// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2019 Linaro, Ltd, Rob Herring <robh@kernel.org> */
/* Copyright 2019 Collabora ltd. */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <drm/drm_print.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include "include/uapi/drm/rocket_accel.h"
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "rocket_core.h"
#include "rocket_device.h"
#include "rocket_drv.h"
#include "rocket_job.h"
#include "rocket_registers.h"

#define JOB_TIMEOUT_MS 500

static struct rocket_job *
to_rocket_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct rocket_job, base);
}

static const char *rocket_fence_get_driver_name(struct dma_fence *fence)
{
	return "rocket";
}

static const char *rocket_fence_get_timeline_name(struct dma_fence *fence)
{
	return "rockchip-npu";
}

static const struct dma_fence_ops rocket_fence_ops = {
	.get_driver_name = rocket_fence_get_driver_name,
	.get_timeline_name = rocket_fence_get_timeline_name,
};

static struct dma_fence *rocket_fence_create(struct rocket_core *core)
{
	struct dma_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	dma_fence_init(fence, &rocket_fence_ops, &core->fence_lock,
		       core->fence_context, ++core->emit_seqno);

	return fence;
}

static int
rocket_copy_tasks(struct drm_device *dev,
		  struct drm_file *file_priv,
		  struct drm_rocket_job *job,
		  struct rocket_job *rjob)
{
	int ret = 0;

	if (job->task_struct_size < sizeof(struct drm_rocket_task))
		return -EINVAL;

	rjob->task_count = job->task_count;

	if (!rjob->task_count)
		return 0;

	rjob->tasks = kvmalloc_objs(*rjob->tasks, job->task_count);
	if (!rjob->tasks) {
		drm_dbg(dev, "Failed to allocate task array\n");
		return -ENOMEM;
	}

	for (int i = 0; i < rjob->task_count; i++) {
		struct drm_rocket_task task = {0};

		if (copy_from_user(&task,
				   u64_to_user_ptr(job->tasks) + i * job->task_struct_size,
				   sizeof(task))) {
			drm_dbg(dev, "Failed to copy incoming tasks\n");
			ret = -EFAULT;
			goto fail;
		}

		if (task.regcmd_count == 0) {
			drm_dbg(dev, "regcmd_count field in drm_rocket_task should be > 0.\n");
			ret = -EINVAL;
			goto fail;
		}

		rjob->tasks[i].regcmd = task.regcmd;
		rjob->tasks[i].regcmd_count = task.regcmd_count;
	}

	return 0;

fail:
	kvfree(rjob->tasks);
	return ret;
}

static int rocket_pc_pulse_keep;
module_param(rocket_pc_pulse_keep, int, 0644);
MODULE_PARM_DESC(rocket_pc_pulse_keep, "TEST: keep OP_EN=1 after starting a descriptor chain");

static int rocket_open_mask;
module_param(rocket_open_mask, int, 0644);
MODULE_PARM_DESC(rocket_open_mask, "TEST: unmask all interrupts (0x1ffff) on chained jobs to log every unit's done bits");

static int rocket_desc_walk;
module_param(rocket_desc_walk, int, 0644);

static int rocket_lut_settle_us = 50;
module_param(rocket_lut_settle_us, int, 0644);
MODULE_PARM_DESC(rocket_lut_settle_us, "settle time after loading the DPU lookup tables (us)");
MODULE_PARM_DESC(rocket_desc_walk, "TEST: program TASK_DMA_BASE_ADDR so the PC walks the task-descriptor array (vendor mode)");

/* Reset the core AND re-arm the NPU MMU.  reset_control wipes the MMU
 * behind rk_iommu's back (rk_iommu re-programs the DTE only on runtime
 * resume), so a bare rocket_core_reset() leaves every following DMA
 * pointing into the void.  rk_iommu commands: 2 stall, 4 ZAP_CACHE,
 * 0 ENABLE_PAGING, 3 unstall. */
static void rocket_core_reset_rearm(struct rocket_core *core)
{
	u32 dte = core->mmu_iomem ? readl(core->mmu_iomem + 0x00) : 0;

	rocket_core_reset(core);
	if (core->mmu_iomem && dte) {
		writel(2, core->mmu_iomem + 0x08);
		writel(dte, core->mmu_iomem + 0x00);
		writel(4, core->mmu_iomem + 0x08);
		writel(3, core->mmu_iomem + 0x1c);	/* INT_MASK */
		writel(0, core->mmu_iomem + 0x08);
		writel(3, core->mmu_iomem + 0x08);
		dev_dbg(core->dev, "core reset, mmu re-armed dte=%08x status=%08x\n",
			readl(core->mmu_iomem + 0x00),
			readl(core->mmu_iomem + 0x04));
	}
}

static void rocket_job_hw_submit(struct rocket_core *core, struct rocket_job *job)
{
	unsigned int scale = core->rdev->variant->pc_data_amount_scale;
	struct rocket_task *task;
	unsigned int extra_bit;

	/* Don't queue the job if a reset is in progress */
	if (atomic_read(&core->reset.pending))
		return;

	/* GO ! */

	/* TEST (iav RE, 2026-08-22): unit state before each submit --
	 * S_POINTER bit16 = executer engaged, TASK_STATUS = task counter. */
	dev_dbg(core->dev,
		"TEST pre-submit: cna_sp=%08x core_sp=%08x dpu_sp=%08x rdma_sp=%08x task_status=%08x raw=%08x mmu_dte=%08x mmu_status=%08x\n",
		rocket_cna_readl(core, S_POINTER),
		rocket_core_readl(core, S_POINTER),
		core->dpu_iomem ? readl(core->dpu_iomem + 0x4) : 0xdead,
		core->dpu_iomem ? readl(core->dpu_iomem + 0x1004) : 0xdead,
		rocket_pc_readl(core, TASK_STATUS),
		rocket_pc_readl(core, INTERRUPT_RAW_STATUS),
		core->mmu_iomem ? readl(core->mmu_iomem + 0x00) : 0xdead,
		core->mmu_iomem ? readl(core->mmu_iomem + 0x04) : 0xdead);

	/* TEST F (iav RE, 2026-08-22): a completed task leaves EXECUTER (bit16)
	 * on the other pp bank and the next single-task submit stalls
	 * (PP_CLEAR writes do not help).  Emulate what makes fresh processes
	 * work -- a full core reset -- but re-arm the NPU MMU afterwards by
	 * hand, because reset_control wipes it behind rk_iommu's back
	 * (rk_iommu re-programs the DTE only on runtime resume).
	 * rk_iommu commands: 1 ENABLE_PAGING, 6 FORCE_RESET, 8 ZAP_CACHE. */
	if (core->mmu_iomem && !job->task_desc_addr &&
	    (rocket_cna_readl(core, S_POINTER) & BIT(16)))
		rocket_core_reset_rearm(core);

	if (job->task_desc_addr) {
		/* Vendor PC task-DMA mode: the PC unit walks the descriptor
		 * array itself, configuring the next task into the free
		 * ping-pong bank while the current one runs.  One submit per
		 * job; the IRQ handler waits for the task counter to reach
		 * task_count.  (RK3568: pc_data_amount_scale=1,
		 * task_number_bits=12, PC_DATA_EXTRA_AMOUNT=4.) */
		unsigned int extra = 0x10000000 * core->index;

		task = &job->tasks[0];
		job->next_task_idx = 0;	/* reused as the done-task counter */

		/* DPU lookup tables for the job (SiLU and friends): the LUT is
		 * a single, non-banked resource behind LUT_ACCESS_CFG (0x4100:
		 * bit 17 write, bit 16 table, [9:0] address) / LUT_ACCESS_DATA
		 * (0x4104, auto-increment).  Loading it from the command stream
		 * is unreliable -- only the first DPU-only task of a chain lands
		 * its writes (RE-LOG Test 75) -- so userspace hands the contents
		 * over and they are written here, after any reset and right
		 * before the chain starts, while the unit is idle. */
		if (job->lut && core->dpu_iomem &&
		    (!core->lut_valid ||
		     memcmp(core->lut_cache, job->lut, sizeof(core->lut_cache)))) {
			int t, k;
			for (t = 0; t < 2; t++) {
				writel(0x20000 | (t << 16), core->dpu_iomem + 0x100);
				for (k = 0; k < 515; k++)
					writel(job->lut[t * 515 + k], core->dpu_iomem + 0x104);
			}
			/* Drain the posted writes and give the table SRAM time to
			 * settle before the PC starts: the first pixels of a chain
			 * started right behind the load read a half-written table
			 * (layer-c2f-add row-0 flakes, RE-LOG Test 76). */
			(void)readl(core->dpu_iomem + 0x100);
			udelay(rocket_lut_settle_us);
			memcpy(core->lut_cache, job->lut, sizeof(core->lut_cache));
			core->lut_valid = true;
		}

		rocket_pc_writel(core, BASE_ADDRESS, 0x1);
		/* The vendor commit path on RK356x does NOT touch the unit
		 * S_POINTERs -- the streams carry their own wake words. */
		(void)extra;

		rocket_pc_writel(core, BASE_ADDRESS, task->regcmd);
		rocket_pc_writel(core, REGISTER_AMOUNTS,
				 PC_REGISTER_AMOUNTS_PC_DATA_AMOUNT(
					 DIV_ROUND_UP(task->regcmd_count, scale) - 1));
		/* Vendor scheme (rknpu_job_subcore_commit_pc): unmask ONLY
		 * the last task's interrupt -- the PC applies the per-task
		 * masks from the descriptor array while walking the chain,
		 * and the CPU sees a single interrupt at the end.  An open
		 * 0x1ffff mask storms on bit 16 during the descriptor walk. */
		rocket_pc_writel(core, INTERRUPT_MASK,
				 rocket_open_mask ? 0x1ffff : job->last_int_mask);
		rocket_pc_writel(core, INTERRUPT_CLEAR, 0x1ffff);
		rocket_pc_writel(core, TASK_CON,
				 PC_TASK_CON_RESERVED_0(1) |
				 PC_TASK_CON_TASK_COUNT_CLEAR(1) |
				 PC_TASK_CON_TASK_PP_EN(1) |
				 PC_TASK_CON_TASK_NUMBER(job->task_count));
		/* The 4-word PC tails drive the regcmd chain, but the PC also
		 * fetches the 40-byte task-descriptor array: per-task int_mask
		 * there is how it learns that a PPU pool task completes on the
		 * 0xc00 bits instead of the DPU 0x300 -- without it the chain
		 * stalls after a pool chunk (homogeneous convolution chains
		 * happen to work with base 0, which the earlier capture showed).
		 * RKT_DESC_BASE0=1 in the environment of the *kernel* is not a
		 * thing; flip this line for experiments. */
		/* Descriptor walk (vendor mode): the PC fetches the 40-byte
		 * task descriptors itself and honors their per-task
		 * enable_mask/int_mask -- required for heterogeneous chains
		 * (PPU pool tasks).  base 0 keeps the tails-only mode. */
		rocket_pc_writel(core, TASK_DMA_BASE_ADDR,
				 rocket_desc_walk ?
					 lower_32_bits(job->task_desc_addr) : 0);
		/* Pulse OP_EN (1 then 0) to start the walker; leaving it at 1
		 * wedges the chain. */
		rocket_pc_writel(core, OPERATION_ENABLE, PC_OPERATION_ENABLE_OP_EN(1));
		if (!rocket_pc_pulse_keep)
			rocket_pc_writel(core, OPERATION_ENABLE, 0);

		dev_dbg(core->dev, "Submitted %d-task descriptor chain at 0x%llx\n",
			job->task_count, job->task_desc_addr);
		return;
	}

	task = &job->tasks[job->next_task_idx];
	job->next_task_idx++;

	rocket_pc_writel(core, BASE_ADDRESS, 0x1);

	 /* From rknpu, in the TRM this bit is marked as reserved */
	extra_bit = 0x10000000 * core->index;
	rocket_cna_writel(core, S_POINTER, CNA_S_POINTER_POINTER_PP_EN(1) |
					   CNA_S_POINTER_EXECUTER_PP_EN(1) |
					   CNA_S_POINTER_POINTER_PP_MODE(1) |
					   extra_bit);

	rocket_core_writel(core, S_POINTER, CORE_S_POINTER_POINTER_PP_EN(1) |
					    CORE_S_POINTER_EXECUTER_PP_EN(1) |
					    CORE_S_POINTER_POINTER_PP_MODE(1) |
					    extra_bit);

	rocket_pc_writel(core, BASE_ADDRESS, task->regcmd);
	rocket_pc_writel(core, REGISTER_AMOUNTS,
			 PC_REGISTER_AMOUNTS_PC_DATA_AMOUNT(
				 DIV_ROUND_UP(task->regcmd_count, scale) - 1));

	/* TEST: unmask every RK3568 interrupt bit (vendor RKNPU_INT_CLEAR is
	 * 0x1ffff) to see which pipeline stages actually signal completion.
	 */
	rocket_pc_writel(core, INTERRUPT_MASK, 0x1ffff);
	rocket_pc_writel(core, INTERRUPT_CLEAR, 0x1ffff);

	rocket_pc_writel(core, TASK_CON, PC_TASK_CON_RESERVED_0(1) |
					 PC_TASK_CON_TASK_COUNT_CLEAR(1) |
					 PC_TASK_CON_TASK_NUMBER(1) |
					 PC_TASK_CON_TASK_PP_EN(1));

	rocket_pc_writel(core, TASK_DMA_BASE_ADDR, PC_TASK_DMA_BASE_ADDR_DMA_BASE_ADDR(0x0));

	rocket_pc_writel(core, OPERATION_ENABLE, PC_OPERATION_ENABLE_OP_EN(1));

	dev_dbg(core->dev, "Submitted regcmd at 0x%llx to core %d", task->regcmd, core->index);
}

static int rocket_acquire_object_fences(struct drm_gem_object **bos,
					int bo_count,
					struct drm_sched_job *job,
					bool is_write)
{
	int i, ret;

	for (i = 0; i < bo_count; i++) {
		ret = dma_resv_reserve_fences(bos[i]->resv, 1);
		if (ret)
			return ret;

		ret = drm_sched_job_add_implicit_dependencies(job, bos[i],
							      is_write);
		if (ret)
			return ret;
	}

	return 0;
}

static void rocket_attach_object_fences(struct drm_gem_object **bos,
					int bo_count,
					struct dma_fence *fence)
{
	int i;

	for (i = 0; i < bo_count; i++)
		dma_resv_add_fence(bos[i]->resv, fence, DMA_RESV_USAGE_WRITE);
}

static int rocket_job_push(struct rocket_job *job)
{
	struct rocket_device *rdev = job->rdev;
	struct drm_gem_object **bos;
	struct ww_acquire_ctx acquire_ctx;
	int ret = 0;

	bos = kvmalloc_array(job->in_bo_count + job->out_bo_count, sizeof(void *),
			     GFP_KERNEL);
	memcpy(bos, job->in_bos, job->in_bo_count * sizeof(void *));
	memcpy(&bos[job->in_bo_count], job->out_bos, job->out_bo_count * sizeof(void *));

	ret = drm_gem_lock_reservations(bos, job->in_bo_count + job->out_bo_count, &acquire_ctx);
	if (ret)
		goto err;

	scoped_guard(mutex, &rdev->sched_lock) {
		drm_sched_job_arm(&job->base);

		job->inference_done_fence = dma_fence_get(&job->base.s_fence->finished);

		ret = rocket_acquire_object_fences(job->in_bos, job->in_bo_count, &job->base, false);
		if (ret)
			goto err_unlock;

		ret = rocket_acquire_object_fences(job->out_bos, job->out_bo_count, &job->base, true);
		if (ret)
			goto err_unlock;

		kref_get(&job->refcount); /* put by scheduler job completion */

		drm_sched_entity_push_job(&job->base);
	}

	rocket_attach_object_fences(job->out_bos, job->out_bo_count, job->inference_done_fence);

err_unlock:
	drm_gem_unlock_reservations(bos, job->in_bo_count + job->out_bo_count, &acquire_ctx);
err:
	kvfree(bos);

	return ret;
}

static void rocket_job_cleanup(struct kref *ref)
{
	struct rocket_job *job = container_of(ref, struct rocket_job,
						refcount);
	unsigned int i;

	rocket_iommu_domain_put(job->domain);

	dma_fence_put(job->done_fence);
	dma_fence_put(job->inference_done_fence);

	if (job->in_bos) {
		for (i = 0; i < job->in_bo_count; i++)
			drm_gem_object_put(job->in_bos[i]);

		kvfree(job->in_bos);
	}

	if (job->out_bos) {
		for (i = 0; i < job->out_bo_count; i++)
			drm_gem_object_put(job->out_bos[i]);

		kvfree(job->out_bos);
	}

	kvfree(job->tasks);
	kvfree(job->lut);

	kfree(job);
}

static void rocket_job_put(struct rocket_job *job)
{
	kref_put(&job->refcount, rocket_job_cleanup);
}

static void rocket_job_free(struct drm_sched_job *sched_job)
{
	struct rocket_job *job = to_rocket_job(sched_job);

	drm_sched_job_cleanup(sched_job);

	rocket_job_put(job);
}

static struct rocket_core *sched_to_core(struct rocket_device *rdev,
					 struct drm_gpu_scheduler *sched)
{
	unsigned int core;

	for (core = 0; core < rdev->num_cores; core++) {
		if (&rdev->cores[core].sched == sched)
			return &rdev->cores[core];
	}

	return NULL;
}

static struct dma_fence *rocket_job_run(struct drm_sched_job *sched_job)
{
	struct rocket_job *job = to_rocket_job(sched_job);
	struct rocket_device *rdev = job->rdev;
	struct rocket_core *core = sched_to_core(rdev, sched_job->sched);
	struct dma_fence *fence = NULL;
	int ret;

	if (unlikely(job->base.s_fence->finished.error))
		return NULL;

	/*
	 * Nothing to execute: can happen if the job has finished while
	 * we were resetting the NPU.
	 */
	if (job->next_task_idx == job->task_count)
		return NULL;

	fence = rocket_fence_create(core);
	if (IS_ERR(fence))
		return fence;

	if (job->done_fence)
		dma_fence_put(job->done_fence);
	job->done_fence = dma_fence_get(fence);

	ret = pm_runtime_get_sync(core->dev);
	if (ret < 0)
		return fence;

	/* TEST: keep the IOMMU domain attached across jobs (midgy RFC 5/9):
	 * the per-job detach/attach handshake on the idle NPU MMU is slow,
	 * noisy (stall timeouts, __iommu_group_set_core_domain WARNs) and
	 * resets MMU state such as AUTO_GATING on every job.
	 */
	if (core->attached_domain != job->domain) {
		if (core->attached_domain) {
			iommu_detach_group(core->attached_domain->domain,
					   core->iommu_group);
			rocket_iommu_domain_put(core->attached_domain);
			core->attached_domain = NULL;
		}
		ret = iommu_attach_group(job->domain->domain, core->iommu_group);
		if (ret < 0)
			return fence;
		kref_get(&job->domain->kref);
		core->attached_domain = job->domain;
	}

	/* TEST: the vendor rk_iommu_enable() always sets AUTO_GATING bit 31
	 * ("Workaround for iommu blocked, BIT(31) default to 1",
	 * DISABLE_FETCH_DTE_TIME_LIMIT) on this MMU; mainline rk_iommu never
	 * does.  Set it right after attach, before any NPU DMA.
	 */
	if (core->mmu_iomem) {
		u32 ag = readl(core->mmu_iomem + 0x24);

		if (!(ag & BIT(31)))
			writel(ag | BIT(31), core->mmu_iomem + 0x24);
	}

	scoped_guard(mutex, &core->job_lock) {
		core->in_flight_job = job;
		rocket_job_hw_submit(core, job);
	}

	return fence;
}

static void rocket_job_handle_irq(struct rocket_core *core)
{
	pm_runtime_mark_last_busy(core->dev);

	scoped_guard(mutex, &core->job_lock)
		if (core->in_flight_job && core->in_flight_job->task_desc_addr) {
			/* PC-chained job: the regcmd tails drive the chain; each
			 * finished task raises one of the two DPU done bits
			 * (ping-pong banks).  TASK_STATUS stays 0 on RK3568, so
			 * count the done BITS, and only clear what we saw --
			 * a task can finish between the read and the clear. */
			u32 st = rocket_pc_readl(core, INTERRUPT_RAW_STATUS);

			rocket_pc_writel(core, INTERRUPT_CLEAR, st);
			dev_dbg(core->dev, "chain irq st=%08x task_status=%08x\n",
				st, rocket_pc_readl(core, TASK_STATUS));
			/* Vendor scheme: the single unmasked interrupt is the
			 * last task's (per-task masks come from the descriptor
			 * array while the PC walks the chain). */
			if (!(st & core->in_flight_job->last_int_mask)) {
				rocket_pc_writel(core, INTERRUPT_MASK,
						 rocket_open_mask ?
							 0x1ffff :
							 core->in_flight_job->last_int_mask);
				return;
			}
			rocket_pc_writel(core, OPERATION_ENABLE, 0x0);
			dma_fence_signal(core->in_flight_job->done_fence);
			pm_runtime_put_autosuspend(core->dev);
			core->in_flight_job = NULL;
			return;
		}

	rocket_pc_writel(core, OPERATION_ENABLE, 0x0);
	rocket_pc_writel(core, INTERRUPT_CLEAR, 0x1ffff);

	scoped_guard(mutex, &core->job_lock)
		if (core->in_flight_job) {
			if (core->in_flight_job->next_task_idx < core->in_flight_job->task_count) {
				rocket_job_hw_submit(core, core->in_flight_job);
				return;
			}

			/* TEST: domain stays attached across jobs */
			dma_fence_signal(core->in_flight_job->done_fence);
			pm_runtime_put_autosuspend(core->dev);
			core->in_flight_job = NULL;
		}
}

static void
rocket_reset(struct rocket_core *core, struct drm_sched_job *bad)
{
	if (!atomic_read(&core->reset.pending))
		return;

	drm_sched_stop(&core->sched, bad);

	/*
	 * Remaining interrupts have been handled, but we might still have
	 * stuck jobs. Let's make sure the PM counters stay balanced by
	 * manually calling pm_runtime_put_noidle().
	 */
	scoped_guard(mutex, &core->job_lock) {
		if (core->in_flight_job)
			pm_runtime_put_noidle(core->dev);

		/* TEST: domain stays attached across jobs; reset the core
		 * first (midgy RFC 4/9) instead of stalling a wedged master.
		 */

		core->in_flight_job = NULL;
	}

	/* Proceed with reset now.  Re-arm the MMU too: a bare core reset
	 * leaves the NPU MMU disabled and every following job DMAs into the
	 * void (observed as an endless timeout series after one bad job). */
	rocket_core_reset_rearm(core);

	/* NPU has been reset, we can clear the reset pending bit. */
	atomic_set(&core->reset.pending, 0);

	/* Restart the scheduler */
	drm_sched_start(&core->sched, 0);
}

static enum drm_gpu_sched_stat rocket_job_timedout(struct drm_sched_job *sched_job)
{
	struct rocket_job *job = to_rocket_job(sched_job);
	struct rocket_device *rdev = job->rdev;
	struct rocket_core *core = sched_to_core(rdev, sched_job->sched);

	dev_err(core->dev, "NPU job timed out");
	{
		u32 pc[0x12];
		int i;

		for (i = 0; i < 0x12; i++)
			pc[i] = readl(core->pc_iomem + i * 4);
		dev_err(core->dev,
			"PC 00-44: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
			pc[0], pc[1], pc[2], pc[3], pc[4], pc[5], pc[6], pc[7],
			pc[8], pc[9], pc[10], pc[11], pc[12], pc[13], pc[14],
			pc[15], pc[16], pc[17]);
		if (core->top_iomem)
			dev_err(core->dev, "counters dt_wr=%x dt_rd=%x wt_rd=%x\n",
				readl(core->top_iomem + 0x34),
				readl(core->top_iomem + 0x38),
				readl(core->top_iomem + 0x3c));
		dev_err(core->dev, "cna_sp=%08x core_sp=%08x\n",
			rocket_cna_readl(core, S_POINTER),
			rocket_core_readl(core, S_POINTER));
	}

	atomic_set(&core->reset.pending, 1);
	rocket_reset(core, sched_job);

	return DRM_GPU_SCHED_STAT_RESET;
}

static void rocket_reset_work(struct work_struct *work)
{
	struct rocket_core *core;

	core = container_of(work, struct rocket_core, reset.work);
	rocket_reset(core, NULL);
}

static const struct drm_sched_backend_ops rocket_sched_ops = {
	.run_job = rocket_job_run,
	.timedout_job = rocket_job_timedout,
	.free_job = rocket_job_free
};

static irqreturn_t rocket_job_irq_handler_thread(int irq, void *data)
{
	struct rocket_core *core = data;

	rocket_job_handle_irq(core);

	return IRQ_HANDLED;
}

static irqreturn_t rocket_job_irq_handler(int irq, void *data)
{
	struct rocket_core *core = data;
	u32 raw_status;

	/* TEST: the line is shared with the IOMMU; never touch our MMIO when
	 * the NPU is runtime-suspended (doing so aborts and panics in IRQ
	 * context).  Same pattern as rk_iommu_irq().
	 */
	if (pm_runtime_get_if_in_use(core->dev) <= 0)
		return IRQ_NONE;

	raw_status = rocket_pc_readl(core, INTERRUPT_RAW_STATUS);

	/* TEST: on a DMA error, ask the NPU MMU what address it choked on.
	 * rk_iommu register offsets: STATUS 0x04, PAGE_FAULT_ADDR 0x0c,
	 * INT_RAWSTAT 0x14, DTE_ADDR 0x00.  Reading them is safe here: the
	 * NPU rail is up (pm_runtime_get_if_in_use above) and the block is
	 * inside it.
	 */
	if (raw_status & (PC_INTERRUPT_RAW_STATUS_DMA_READ_ERROR |
			  PC_INTERRUPT_RAW_STATUS_DMA_WRITE_ERROR) &&
	    core->mmu_iomem) {
		dev_err(core->dev,
			"TEST: dma error, mmu dte=0x%08x status=0x%08x fault_addr=0x%08x rawstat=0x%08x int_status=0x%08x\n",
			readl(core->mmu_iomem + 0x00),
			readl(core->mmu_iomem + 0x04),
			readl(core->mmu_iomem + 0x0c),
			readl(core->mmu_iomem + 0x14),
			readl(core->mmu_iomem + 0x20));
	}

	WARN_ON(raw_status & PC_INTERRUPT_RAW_STATUS_DMA_READ_ERROR);
	WARN_ON(raw_status & PC_INTERRUPT_RAW_STATUS_DMA_WRITE_ERROR);

	/* TEST: log every interrupt with the task counter; only DPU bits
	 * complete the job, anything else is cleared and execution continues.
	 */
	if (raw_status & 0x1ffff)
		dev_dbg(core->dev,
			 "irq raw=0x%08x status=0x%08x task_status=0x%08x\n",
			 raw_status, rocket_pc_readl(core, INTERRUPT_STATUS),
			 rocket_pc_readl(core, TASK_STATUS));
	/* Legacy stepping only: chained jobs rely on the hardware ping-pong. */
	if ((raw_status & 0x1ffff) && core->dpu_iomem &&
	    !(core->in_flight_job && core->in_flight_job->task_desc_addr)) {
		u32 sp = readl(core->dpu_iomem + 0x4);
		writel(sp & ~1u, core->dpu_iomem + 0x4); /* bank0 */
	}
	if (raw_status & 0x1ffff)
		dev_dbg(core->dev, "dpu bs_cfg=%08x mul=%08x fmt=%08x ow=%08x brdma=%08x bs_base=%08x n5024=%08x\n",
			 core->dpu_iomem ? readl(core->dpu_iomem + 0x40) : 0,
			 core->dpu_iomem ? readl(core->dpu_iomem + 0x48) : 0,
			 core->dpu_iomem ? readl(core->dpu_iomem + 0x24) : 0, /* cube W|H */
			 core->dpu_iomem ? readl(core->dpu_iomem + 0x50) : 0,
			 core->dpu_iomem ? readl(core->dpu_iomem + 0x101c) : 0,
			 core->dpu_iomem ? readl(core->dpu_iomem + 0x1020) : 0,
			 core->dpu_iomem ? readl(core->dpu_iomem + 0x1024) : 0);
	if (raw_status & 0x1ffff)
		dev_dbg(core->dev, "counters dt_wr=0x%x dt_rd=0x%x wt_rd=0x%x\n",
			 core->top_iomem ? readl(core->top_iomem + 0x34) : 0,
			 core->top_iomem ? readl(core->top_iomem + 0x38) : 0,
			 core->top_iomem ? readl(core->top_iomem + 0x3c) : 0);
	else
		dev_dbg(core->dev, "irq empty raw=0x%08x\n",
				     raw_status);

	if (!(raw_status & 0x1ffff)) {
		pm_runtime_put(core->dev);
		return IRQ_NONE;
	}

	/* Wake the thread on DPU done (0x300), PPU done candidates (0xc00)
	 * and bit 16 (storms in pool chains with an open mask -- possibly
	 * the RK3568 PPU done) -- a chained job whose last task is a PPU
	 * pool completes on one of these. */
	if (!(raw_status & (PC_INTERRUPT_RAW_STATUS_DPU_0 |
			    PC_INTERRUPT_RAW_STATUS_DPU_1 | 0xc00 | 0x10000))) {
		rocket_pc_writel(core, INTERRUPT_CLEAR, raw_status & 0x1ffff);
		pm_runtime_put(core->dev);
		return IRQ_HANDLED;
	}

	rocket_pc_writel(core, INTERRUPT_MASK, 0x0);

	pm_runtime_put(core->dev);
	return IRQ_WAKE_THREAD;
}

int rocket_job_init(struct rocket_core *core)
{
	struct drm_sched_init_args args = {
		.ops = &rocket_sched_ops,
		/* .num_rqs dropped from drm_sched_init_args in 7.2 */
		.credit_limit = 1,
		.timeout = msecs_to_jiffies(JOB_TIMEOUT_MS),
		.name = dev_name(core->dev),
		.dev = core->dev,
	};
	int ret;

	INIT_WORK(&core->reset.work, rocket_reset_work);
	spin_lock_init(&core->fence_lock);
	mutex_init(&core->job_lock);

	core->irq = platform_get_irq(to_platform_device(core->dev), 0);
	if (core->irq < 0)
		return core->irq;

	ret = devm_request_threaded_irq(core->dev, core->irq,
					rocket_job_irq_handler,
					rocket_job_irq_handler_thread,
					IRQF_SHARED, dev_name(core->dev),
					core);
	if (ret) {
		dev_err(core->dev, "failed to request job irq");
		return ret;
	}

	core->reset.wq = alloc_ordered_workqueue("rocket-reset-%d", 0, core->index);
	if (!core->reset.wq)
		return -ENOMEM;

	core->fence_context = dma_fence_context_alloc(1);

	args.timeout_wq = core->reset.wq;
	ret = drm_sched_init(&core->sched, &args);
	if (ret) {
		dev_err(core->dev, "Failed to create scheduler: %d.", ret);
		goto err_sched;
	}

	return 0;

err_sched:
	drm_sched_fini(&core->sched);

	destroy_workqueue(core->reset.wq);
	return ret;
}

void rocket_job_fini(struct rocket_core *core)
{
	drm_sched_fini(&core->sched);

	cancel_work_sync(&core->reset.work);
	destroy_workqueue(core->reset.wq);
}

int rocket_job_open(struct rocket_file_priv *rocket_priv)
{
	struct rocket_device *rdev = rocket_priv->rdev;
	struct drm_gpu_scheduler **scheds = kmalloc_objs(*scheds,
							 rdev->num_cores);
	unsigned int core;
	int ret;

	for (core = 0; core < rdev->num_cores; core++)
		scheds[core] = &rdev->cores[core].sched;

	ret = drm_sched_entity_init(&rocket_priv->sched_entity,
				    DRM_SCHED_PRIORITY_NORMAL,
				    scheds,
				    rdev->num_cores, NULL);
	if (WARN_ON(ret))
		return ret;

	return 0;
}

void rocket_job_close(struct rocket_file_priv *rocket_priv)
{
	struct drm_sched_entity *entity = &rocket_priv->sched_entity;

	kfree(entity->sched_list);
	drm_sched_entity_destroy(entity);
}

int rocket_job_is_idle(struct rocket_core *core)
{
	/* If there are any jobs in this HW queue, we're not idle */
	if (atomic_read(&core->sched.credit_count))
		return false;

	return true;
}

static int rocket_ioctl_submit_job(struct drm_device *dev, struct drm_file *file,
				   struct drm_rocket_job *job)
{
	struct rocket_device *rdev = to_rocket_device(dev);
	struct rocket_file_priv *file_priv = file->driver_priv;
	struct rocket_job *rjob = NULL;
	int ret = 0;

	if (job->task_count == 0)
		return -EINVAL;

	rjob = kzalloc_obj(*rjob);
	if (!rjob)
		return -ENOMEM;

	kref_init(&rjob->refcount);

	rjob->rdev = rdev;

	ret = drm_sched_job_init(&rjob->base,
				 &file_priv->sched_entity,
				 1, NULL, file->client_id);
	if (ret)
		goto out_put_job;

	rjob->task_desc_addr = job->task_desc_addr;
	rjob->last_int_mask = job->last_int_mask ?: 0x300;

	if (job->lut_count) {
		if (job->lut_count != 1030) {
			ret = -EINVAL;
			goto out_cleanup_job;
		}
		rjob->lut = kvmalloc_array(job->lut_count, sizeof(u16), GFP_KERNEL);
		if (!rjob->lut) {
			ret = -ENOMEM;
			goto out_cleanup_job;
		}
		if (copy_from_user(rjob->lut, u64_to_user_ptr(job->lut_data),
				   job->lut_count * sizeof(u16))) {
			ret = -EFAULT;
			goto out_cleanup_job;
		}
		rjob->lut_count = job->lut_count;
	}

	ret = rocket_copy_tasks(dev, file, job, rjob);
	if (ret)
		goto out_cleanup_job;

	ret = drm_gem_objects_lookup(file, u64_to_user_ptr(job->in_bo_handles),
				     job->in_bo_handle_count, &rjob->in_bos);
	if (ret)
		goto out_cleanup_job;

	rjob->in_bo_count = job->in_bo_handle_count;

	ret = drm_gem_objects_lookup(file, u64_to_user_ptr(job->out_bo_handles),
				     job->out_bo_handle_count, &rjob->out_bos);
	if (ret)
		goto out_cleanup_job;

	rjob->out_bo_count = job->out_bo_handle_count;

	rjob->domain = rocket_iommu_domain_get(file_priv);

	ret = rocket_job_push(rjob);
	if (ret)
		goto out_cleanup_job;

out_cleanup_job:
	if (ret)
		drm_sched_job_cleanup(&rjob->base);
out_put_job:
	rocket_job_put(rjob);

	return ret;
}

int rocket_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_rocket_submit *args = data;
	struct drm_rocket_job *jobs;
	int ret = 0;
	unsigned int i = 0;

	if (args->job_count == 0)
		return 0;

	if (args->job_struct_size < sizeof(struct drm_rocket_job)) {
		drm_dbg(dev, "job_struct_size field in drm_rocket_submit struct is too small.\n");
		return -EINVAL;
	}

	if (args->reserved != 0) {
		drm_dbg(dev, "Reserved field in drm_rocket_submit struct should be 0.\n");
		return -EINVAL;
	}

	jobs = kvmalloc_objs(*jobs, args->job_count);
	if (!jobs) {
		drm_dbg(dev, "Failed to allocate incoming job array\n");
		return -ENOMEM;
	}

	for (i = 0; i < args->job_count; i++) {
		if (copy_from_user(&jobs[i],
				   u64_to_user_ptr(args->jobs) + i * args->job_struct_size,
				   sizeof(*jobs))) {
			ret = -EFAULT;
			drm_dbg(dev, "Failed to copy incoming job array\n");
			goto exit;
		}
	}


	for (i = 0; i < args->job_count; i++)
		rocket_ioctl_submit_job(dev, file, &jobs[i]);

exit:
	kvfree(jobs);

	return ret;
}

/* replay.c — submit a captured vendor rknpu task through the rocket driver.
 *
 * Usage: replay <base_dir> <state_bo02> <gold_bo02> <regcmd_off> <count> <cmp_off> <cmp_len> [save]
 *   capture_dir     e.g. vendor-capture/rknpu_replay_f128
 *   task_regcmd_off offset of the task regcmd inside bo01 (0x429d40 for task 0)
 *   regcmd_count    64-bit words for PC_DATA_AMOUNT (137 for task 0)
 *   cmp_off/cmp_len region of bo02 to compare against bo02.post.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include "rocket_accel.h"

struct cap_bo { uint64_t old_dma, size; void *data; uint32_t handle; uint64_t new_dma; void *map; };

static void *slurp(const char *path, uint64_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "no %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
	void *b = malloc(*len);
	if (fread(b, 1, *len, f) != *len) exit(1);
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	if (argc < 8) { fprintf(stderr, "usage: %s base_dir state_bo02 gold_bo02 regcmd_off count cmp_off cmp_len [save]\n", argv[0]); return 1; }
	const char *dir = argv[1];
	const char *state_path = argv[2];
	const char *gold_path = argv[3];
	uint64_t regcmd_off = strtoull(argv[4], NULL, 0);
	uint32_t count = strtoul(argv[5], NULL, 0);
	uint64_t cmp_off = strtoull(argv[6], NULL, 0);
	uint64_t cmp_len = strtoull(argv[7], NULL, 0);
	char p[256];

	/* captured layout (meta.txt of this capture set) */
	struct cap_bo bos[4] = {
		{ 0xffbc7000, 0, NULL, 0, 0, NULL }, /* bo01 blob: weights+regcmd+bs */
		{ 0xffa1a000, 0, NULL, 0, 0, NULL }, /* bo02 intermediate */
		{ 0xff9f5000, 0, NULL, 0, 0, NULL }, /* bo03 input */
		{ 0xff9f4000, 0, NULL, 0, 0, NULL }, /* bo04 */
	};
	const char *names[4] = { "bo01", "bo02", "bo03", "bo04" };

	int fd = open("/dev/accel/accel0", O_RDWR);
	if (fd < 0) { perror("accel0"); return 1; }

	for (int i = 0; i < 4; i++) {
		if (i == 1)
			snprintf(p, sizeof(p), "%s", state_path);
		else
			snprintf(p, sizeof(p), "%s/%s.bin", dir, names[i]);
		bos[i].data = slurp(p, &bos[i].size);
		struct drm_rocket_create_bo cb = { .size = (uint32_t)bos[i].size };
		if (ioctl(fd, DRM_IOCTL_ROCKET_CREATE_BO, &cb)) { perror("create"); return 1; }
		bos[i].handle = cb.handle;
		bos[i].new_dma = cb.dma_address;
		bos[i].map = mmap(NULL, bos[i].size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, cb.offset);
		if (bos[i].map == MAP_FAILED) { perror("mmap"); return 1; }
		printf("%s size=%llu old=%#llx new=%#llx\n", names[i],
		       (unsigned long long)bos[i].size,
		       (unsigned long long)bos[i].old_dma,
		       (unsigned long long)bos[i].new_dma);
	}

	/* copy contents; patch every address-looking value in the whole blob
	 * command region (from regcmd start to end of blob) */
	for (int i = 0; i < 4; i++)
		memcpy(bos[i].map, bos[i].data, bos[i].size);

	uint64_t *w = (uint64_t *)((uint8_t *)bos[0].map + 0x429d40);
	uint64_t nw = (bos[0].size - 0x429d40) / 8;
	uint64_t patched = 0;
	for (uint64_t k = 0; k < nw; k++) {
		uint64_t word = w[k];
		if (!word) continue;
		uint64_t val = (word >> 16) & 0xffffffffull;
		for (int i = 0; i < 4; i++) {
			if (val >= bos[i].old_dma && val < bos[i].old_dma + bos[i].size) {
				uint64_t nv = bos[i].new_dma + (val - bos[i].old_dma);
				word = (word & 0xffff00000000ffffull) | (nv << 16);
				w[k] = word;
				patched++;
				break;
			}
		}
	}
	printf("patched %llu address words\n", (unsigned long long)patched);

	for (int i = 0; i < 4; i++) {
		struct drm_rocket_fini_bo fb = { .handle = bos[i].handle };
		if (ioctl(fd, DRM_IOCTL_ROCKET_FINI_BO, &fb)) { perror("fini"); return 1; }
	}

	uint32_t in_handles[3] = { bos[0].handle, bos[2].handle, bos[3].handle };
	uint32_t out_handles[1] = { bos[1].handle };
	struct drm_rocket_task task = {
		.regcmd = (uint32_t)(bos[0].new_dma + regcmd_off),
		.regcmd_count = count,
	};
	struct drm_rocket_job job = {
		.tasks = (uint64_t)(uintptr_t)&task,
		.task_count = 1,
		.task_struct_size = sizeof(task),
		.in_bo_handles = (uint64_t)(uintptr_t)in_handles,
		.in_bo_handle_count = 3,
		.out_bo_handles = (uint64_t)(uintptr_t)out_handles,
		.out_bo_handle_count = 1,
	};
	struct drm_rocket_submit sub = {
		.jobs = (uint64_t)(uintptr_t)&job,
		.job_count = 1,
		.job_struct_size = sizeof(job),
	};
	int sr = ioctl(fd, DRM_IOCTL_ROCKET_SUBMIT, &sub);
	printf("submit ret=%d errno=%d\n", sr, sr ? errno : 0);
	if (sr) return 1;

	struct drm_rocket_prep_bo pb = { .handle = bos[1].handle, .timeout_ns = 3000000000ll };
	if (ioctl(fd, DRM_IOCTL_ROCKET_PREP_BO, &pb)) perror("prep(wait)");

	/* golden */
	uint64_t glen; uint8_t *gold = slurp(gold_path, &glen);
	uint8_t *got = (uint8_t *)bos[1].map;
	uint64_t diff = 0, first = (uint64_t)-1;
	for (uint64_t k = cmp_off; k < cmp_off + cmp_len && k < glen; k++) {
		if (got[k] != gold[k]) { if (first == (uint64_t)-1) first = k; diff++; }
	}
	printf("compare [%#llx +%#llx]: %llu bytes differ (%.2f%%)\n",
	       (unsigned long long)cmp_off, (unsigned long long)cmp_len,
	       (unsigned long long)diff, 100.0 * diff / (double)cmp_len);
	if (first != (uint64_t)-1) {
		printf("first diff at %#llx\n", (unsigned long long)first);
		for (int i = 0; i < 32; i += 8)
			printf("  got  %02x %02x %02x %02x %02x %02x %02x %02x  gold %02x %02x %02x %02x %02x %02x %02x %02x\n",
			       got[first+i],got[first+i+1],got[first+i+2],got[first+i+3],got[first+i+4],got[first+i+5],got[first+i+6],got[first+i+7],
			       gold[first+i],gold[first+i+1],gold[first+i+2],gold[first+i+3],gold[first+i+4],gold[first+i+5],gold[first+i+6],gold[first+i+7]);
	} else {
		printf("EXACT MATCH\n");
	}
	if (argc >= 9) {
		FILE *o = fopen(argv[8], "wb");
		if (o) { fwrite(got + cmp_off, 1, cmp_len, o); fclose(o); printf("saved %s\n", argv[8]); }
	}
	return 0;
}

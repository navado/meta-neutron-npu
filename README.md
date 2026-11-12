# meta-neutron-npu

A Yocto layer, plus tests, that fixes NPU memory allocation in the NXP Neutron driver (`drivers/staging/neutron`)
on i.MX95 boards.

## The problem

ONNX Runtime's Neutron execution provider allocates a 2 GiB buffer for each session. The stock driver allocates
every Neutron buffer as one physically contiguous block from the Neutron CMA pool (`DMA_ATTR_FORCE_CONTIGUOUS`).
Linux lends that pool to page cache and other movable pages. Once it has done so, a 2 GiB contiguous range is
rarely free, especially while the system is writing (container images loading, logs, downloads). The allocation
stalls, then fails with ENOMEM:

    NEUTRON_IOCTL_BUFFER_CREATE failed!: Cannot allocate memory

ONNX Runtime then silently runs the whole model on the CPU until the next reboot. On our UCM-iMX95 (8 GB, 4 GiB
Neutron pool) with a container stack running, 6 of 7 model loads ended up on the CPU. During heavy disk writes,
1 of 5 2 GiB allocations succeeded.

## The fix

The Neutron sits behind the SoC's SMMU (`iommus = <&smmu 0xd>`), so it doesn't need physically contiguous memory.
With the patch, each buffer is built from 2 MiB chunks of the same Neutron CMA pool:

- **Each chunk is an easy allocation.** Only a 2 MiB range has to be migrated, so a 2 GiB buffer succeeds where a
  2 GiB block fails.
- **The memory stays in the NPU's reservation.** Chunks the pool can't supply come from ordinary memory, capped at
  `fallback_max_mib` (256 MiB) per buffer.
- **Inference runs as with a contiguous buffer.** The chunks are mapped at one size-aligned IOVA range, so the SMMU
  uses 2 MiB block entries.
- **Nothing changes for user space.** The device address and the ioctl/mmap ABI are the same; ONNX Runtime, the
  TFLite delegate and the Neutron runtime work unchanged.

The same patch also fixes:
- **A cache-sync race that loses data with several NPU users:** `neutron_memory_sync()` toggled the device-wide
  `dev->dma_coherent`, so one user's toggle made another skip its cache maintenance.
- **A CACHE_SYNC range check** that set -EINVAL but still synced past the end of the buffer.
- **Missing buffer references:** a use-after-free when a buffer fd is closed during a job, and an oops on a bad
  `buf_id`.
- **A double free** when `dma_buf_fd()` fails.

Results on our board:

| | Stock driver | Patched |
|---|---|---|
| Model loads that got the NPU, container stack running | 1 of 7 | 7 of 7 |
| Model loaded during first boot (container images loading) | CPU fallback | NPU |
| 2 GiB allocation during heavy disk writes | 1 of 5 | 5 of 5 |
| 4 processes using the NPU at once, data check | data lost in every process | clean |
| Generated text (LLM, greedy) | reference | byte-identical |

Module parameters (`/sys/module/neutron/parameters/`, writable at runtime, read per allocation):

| Parameter | Default | |
|---|---|---|
| `iommu_noncontig` | Y | N = the stock contiguous allocation |
| `cma_chunks` | Y | take chunks from the Neutron CMA pool |
| `chunk_order` | 9 | chunk size, 2^order pages (9 = 2 MiB) |
| `fallback_max_mib` | 256 | ordinary memory allowed per buffer when the pool runs short |
| `cma_whole` | N | try one contiguous block first, chunks if that fails |
| `trace_alloc` | N | log each buffer's placement to the kernel log |

Caveat: a chunked buffer exported as a dma-buf to a device without an IOMMU that needs contiguous memory (on
i.MX95: DPU, VPU, ISI) is refused by that importer. The Mali GPU (own MMU) imports chunked buffers correctly.

## Use the layer

Add the layer to your build next to the BSP. With kas:

```yaml
repos:
  meta-neutron-npu:
    url: https://github.com/yey-boats/meta-neutron-npu.git
    branch: main
```

Or with `bitbake-layers add-layer meta-neutron-npu`.

It appends the patch to whichever kernel recipe your BSP uses, for i.MX95 machines only (`mx95-nxp-bsp`):

| BSP layer in your build | Kernel recipe | Patch used by default |
|---|---|---|
| meta-imx (`fsl-bsp-release`), lf-6.18.20-2.0.0 | `linux-imx` 6.18 | `patches/...-lf-6.18.20.patch` |
| Compulab `meta-bsp-imx95` (`linux-compulab_v6.18.20`) | `linux-compulab` 6.18 | `patches/...-lf-6.18.20.patch` |

If your `linux-imx` follows NXP's `lf-6.18.y` branch past the 2026-07/08 mailbox rework, set
`NEUTRON_NPU_PATCH = "0001-staging-neutron-2MiB-CMA-chunks-lf-6.18.y.patch"` in `local.conf`.

The patch needs the driver built in (`CONFIG_NEUTRON=y`), as in NXP's `imx_v8_defconfig` and Compulab's config.
It uses CMA and DMA helpers that the kernel doesn't export to modules.

**Tested:**
- `lf-6.18.20` patch: on a Compulab UCM-iMX95 (`linux-compulab_v6.18.20`, 251a43e9 and d4c617bb), in a flashed
  image.
- `lf-6.18.y` patch: compile-tested only, against NXP `lf-6.18.y` 6e694eda40aa.

The patch is also in [compulab-yokneam/linux-compulab#16](https://github.com/compulab-yokneam/linux-compulab/pull/16).

## Tests

`tests/` checks the driver directly through `/dev/neutron0`. No model, no ONNX Runtime and no NPU job are needed,
and it runs on the stock driver too, where it shows the failures.

    cd tests && ./run_tests.sh              # corner cache writeback; needs gcc and passwordless sudo
    ./run_tests.sh corner                   # one phase
    gcc -O2 -Wall -o neutron_test neutron_test.c && ./neutron_test corner

| Phase | Checks |
|---|---|
| `corner` | sizes, mmap, CACHE_SYNC ranges, new buffers zeroed, fd lifetime, 4 concurrent processes with a data check, 300-buffer leak rounds |
| `cache` | 2 GiB allocation after filling the page cache (5 reps) |
| `writeback` | 2 GiB allocation during heavy buffered writes (5 reps); the stock driver usually fails here |
| `exhaust` | opt-in, disruptive: allocate until failure; no OOM kill expected |

On a patched kernel every phase runs in both modes (`iommu_noncontig=Y` and `N`), so one boot gives an A/B
comparison. Each step's kernel log is saved and checked for oopses, warnings, OOM kills and SMMU faults.
`gpu_test.c` (optional) imports a Neutron buffer into the Mali GPU through `cl_arm_import_memory` and checks the
data. It needs the Mali OpenCL library and headers.

## License

- Patches: GPL-2.0-only, like the kernel.
- `tests/neutron_uapi.h`: the driver's UAPI header, GPL-2.0+ WITH Linux-syscall-note, Copyright NXP.
- Everything else: MIT (see `LICENSE`).

Author: Boris Sorochkin <sbornava@gmail.com>, for [yey.boats](https://github.com/yey-boats).

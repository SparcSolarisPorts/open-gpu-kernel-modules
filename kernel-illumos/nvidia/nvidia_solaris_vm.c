/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Kernel-space mappings of physical ranges for Oracle Solaris, in place of the
 * gfxp_*_kernel_space() helpers of illumos' misc/gfx_private, which Solaris
 * does not have.  Virtual address space comes from heap_arena and is mapped
 * with hat_devload(), as gfx_private does.
 *
 * Endianness: the GPU is little-endian and this CPU is big-endian.  Every
 * mapping made here asks the MMU to byte-swap (HAT_STRUCTURE_LE), so a native
 * 16/32/64-bit load or store sees the numeric value of a little-endian
 * register or structure.  Byte-wise data copied through such a mapping with
 * wider accesses comes out swapped; this is the one place to change if that
 * turns out to be the wrong default (NV_SOL_HAT_ENDIAN).
 */

#include "nv-illumos.h"
#include "nvidia_mem.h"

#include "nv-px-ranges.h"

#include <sys/vmem.h>
#include <vm/hat.h>
#include <vm/as.h>
#include <vm/seg_kmem.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/atomic.h>
#include <sys/systm.h>

#define NV_SOL_HAT_ENDIAN       HAT_STRUCTURE_LE

static uint_t
nv_sol_hat_attr(uint32_t mode)
{
    uint_t attr = PROT_READ | PROT_WRITE | NV_SOL_HAT_ENDIAN;

    switch (mode)
    {
        case GFXP_MEMORY_CACHED:
            attr |= HAT_LOADCACHING_OK;
            break;
        case GFXP_MEMORY_WRITECOMBINED:
            attr |= HAT_MERGING_OK;
            break;
        case GFXP_MEMORY_UNCACHED:
        default:
            attr |= HAT_STRICTORDER;
            break;
    }

    return attr;
}

/*
 * One-time diagnostic (sparcv9 bring-up): find out what HAT_STRUCTURE_LE does
 * to RAM.  A private page is mapped a second time with nv_sol_hat_attr(), a
 * 64-bit value, a 32-bit value and two memcpy() calls are stored through that
 * mapping, and the page is read back bytewise through its normal kernel
 * mapping.  If the mapping swaps, the 64-bit value reads back as 08 07 .. 01.
 * The memcpy rows show what the kernel memcpy does through the same mapping,
 * which is what portMemCopy() does for firmware data.  Remove once settled.
 */
static void
nv_sol_vm_selftest(void)
{
    static volatile uint32_t done;
    uint8_t src[16];
    caddr_t buf, page, va;
    pfn_t pfn;
    int i;

    if (atomic_cas_32(&done, 0, 1) != 0)
        return;

    buf = kmem_zalloc(2 * MMU_PAGESIZE, KM_SLEEP);
    page = (caddr_t)(((uintptr_t)buf + MMU_PAGESIZE - 1) &
        ~((uintptr_t)MMU_PAGESIZE - 1));
    pfn = hat_getpfnum(kas.a_hat, page);
    va = (caddr_t)vmem_alloc(heap_arena, MMU_PAGESIZE, VM_SLEEP);

    hat_devload(kas.a_hat, va, MMU_PAGESIZE, pfn,
        nv_sol_hat_attr(GFXP_MEMORY_CACHED), HAT_LOAD_LOCK | HAT_LOAD_NOCONSIST);

    for (i = 0; i < 16; i++)
        src[i] = (uint8_t)i;

    *(volatile uint64_t *)(void *)va = 0x0102030405060708ULL;
    *(volatile uint32_t *)(void *)(va + 8) = 0x090a0b0cU;
    (void) memcpy(va + 16, src, 16);
    (void) memcpy(va + 32, src, 13);

    cmn_err(CE_NOTE, "nvidia: RAM map self-test (HAT_STRUCTURE_LE), memory bytes:");
    cmn_err(CE_NOTE, "nvidia:   u64 0x0102030405060708 + u32 0x090a0b0c: "
        "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        page[0] & 0xff, page[1] & 0xff, page[2] & 0xff, page[3] & 0xff,
        page[4] & 0xff, page[5] & 0xff, page[6] & 0xff, page[7] & 0xff,
        page[8] & 0xff, page[9] & 0xff, page[10] & 0xff, page[11] & 0xff);
    cmn_err(CE_NOTE, "nvidia:   memcpy of 00..0f, 16 bytes: "
        "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        page[16] & 0xff, page[17] & 0xff, page[18] & 0xff, page[19] & 0xff,
        page[20] & 0xff, page[21] & 0xff, page[22] & 0xff, page[23] & 0xff,
        page[24] & 0xff, page[25] & 0xff, page[26] & 0xff, page[27] & 0xff,
        page[28] & 0xff, page[29] & 0xff, page[30] & 0xff, page[31] & 0xff);
    cmn_err(CE_NOTE, "nvidia:   memcpy of 00..0c, 13 bytes: "
        "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        page[32] & 0xff, page[33] & 0xff, page[34] & 0xff, page[35] & 0xff,
        page[36] & 0xff, page[37] & 0xff, page[38] & 0xff, page[39] & 0xff,
        page[40] & 0xff, page[41] & 0xff, page[42] & 0xff, page[43] & 0xff,
        page[44] & 0xff, page[45] & 0xff, page[46] & 0xff, page[47] & 0xff);

    hat_unload(kas.a_hat, va, MMU_PAGESIZE, HAT_UNLOAD_UNLOCK);
    vmem_free(heap_arena, va, MMU_PAGESIZE);
    kmem_free(buf, 2 * MMU_PAGESIZE);
}

caddr_t
gfxp_alloc_kernel_space(size_t size)
{
    return (caddr_t)vmem_alloc(heap_arena, size, VM_SLEEP);
}

void
gfxp_free_kernel_space(caddr_t va, size_t size)
{
    vmem_free(heap_arena, va, size);
}

/* pa and size are multiples of MMU_PAGESIZE; va is page aligned. */
void
gfxp_load_kernel_space(uint64_t pa, size_t size, uint32_t mode, caddr_t va)
{
    nv_sol_vm_selftest();

    /*
     * The pages belong to the DDI allocator that handed them out, so do not
     * add this mapping to their mapping lists (HAT_LOAD_NOCONSIST).
     */
    hat_devload(kas.a_hat, va, size, (pfn_t)mmu_btop(pa),
        nv_sol_hat_attr(mode), HAT_LOAD_LOCK | HAT_LOAD_NOCONSIST);
}

void
gfxp_unload_kernel_space(caddr_t va, size_t size)
{
    hat_unload(kas.a_hat, va, size, HAT_UNLOAD_UNLOCK);
}

/*
 * Bus to CPU address translation.
 *
 * "assigned-addresses" holds PCI bus addresses, and each px root complex has
 * its own bus address space.  RM wants the CPU physical address of a BAR, so
 * the bus address is translated through the "ranges" of the px node above the
 * device.  The range must lie inside one memory window; otherwise the BAR is
 * rejected rather than mapped at a guess.
 */
int
nv_sol_bus_to_cpu(dev_info_t *dip, NvU64 bus, NvU64 size, NvU64 *cpu)
{
    dev_info_t *p;
    int *cells;
    uint_t nints;
    int rc;

    for (p = ddi_get_parent(dip); p != NULL; p = ddi_get_parent(p))
    {
        if (strcmp(ddi_driver_name(p), "px") == 0)
            break;
    }
    if (p == NULL)
        return -1;

    if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, p, DDI_PROP_DONTPASS,
            "ranges", &cells, &nints) != DDI_PROP_SUCCESS)
        return -1;

    {
        uint64_t translated = 0;     /* NvU64 and uint64_t differ on LP64 */

        rc = nv_px_ranges_translate(cells, nints, bus, size, &translated);
        if (rc == 0)
            *cpu = translated;
    }
    ddi_prop_free(cells);
    return rc;
}

/*
 * BAR registry and kernel mappings of BARs.
 *
 * RM maps BARs by CPU physical address, possibly before the device is on
 * nv_illumos_devices, so the BARs get their own registry.  A BAR is mapped
 * with ddi_regs_map_setup() on the matching register set, which lets the px
 * nexus pick the right window and set the access attributes, instead of
 * building a mapping from an address computed here.
 */
typedef struct nv_sol_bar_reg {
    struct nv_sol_bar_reg  *next;
    nv_illumos_state_t     *nvis;
} nv_sol_bar_reg_t;

typedef struct nv_sol_bar_map {
    struct nv_sol_bar_map  *next;
    caddr_t                 va;
    ddi_acc_handle_t        handle;
} nv_sol_bar_map_t;

static kmutex_t             nv_sol_lock;
static nv_sol_bar_reg_t    *nv_sol_regs;
static nv_sol_bar_map_t    *nv_sol_maps;

void
nv_sol_vm_init(void)
{
    mutex_init(&nv_sol_lock, NULL, MUTEX_DRIVER, NULL);
}

void
nv_sol_vm_fini(void)
{
    mutex_destroy(&nv_sol_lock);
}

void
nv_sol_bar_register(nv_illumos_state_t *nvis)
{
    nv_sol_bar_reg_t *r = kmem_zalloc(sizeof (*r), KM_SLEEP);

    r->nvis = nvis;
    mutex_enter(&nv_sol_lock);
    r->next = nv_sol_regs;
    nv_sol_regs = r;
    mutex_exit(&nv_sol_lock);
}

/* Safe to call for a device that was never registered. */
void
nv_sol_bar_unregister(nv_illumos_state_t *nvis)
{
    nv_sol_bar_reg_t **pp, *r = NULL;

    mutex_enter(&nv_sol_lock);
    for (pp = &nv_sol_regs; *pp != NULL; pp = &(*pp)->next)
    {
        if ((*pp)->nvis == nvis)
        {
            r = *pp;
            *pp = r->next;
            break;
        }
    }
    mutex_exit(&nv_sol_lock);

    if (r != NULL)
        kmem_free(r, sizeof (*r));
}

/*
 * Map [start, start + size) of a registered BAR.  Anything that is not inside
 * a BAR is refused: a physical address that is neither memory nor a mapped BAR
 * is fatal to touch on this platform.
 */
caddr_t
nv_sol_map_bar(NvU64 start, NvU64 size, NvBool merging)
{
    ddi_device_acc_attr_t attr;
    nv_sol_bar_reg_t *r;
    nv_sol_bar_map_t *m;
    dev_info_t *dip = NULL;
    NvU32 cfg_offset = 0;
    NvU64 bar_base = 0;
    ddi_acc_handle_t handle;
    caddr_t va;
    uint_t rnumber;
    uint_t i;

    if (size == 0 || start + size < start)
        return NULL;

    mutex_enter(&nv_sol_lock);
    for (r = nv_sol_regs; r != NULL && dip == NULL; r = r->next)
    {
        nv_state_t *nv = NV_STATE_PTR(r->nvis);

        for (i = 0; i < NV_GPU_NUM_BARS; i++)
        {
            nv_aperture_t *ap = &nv->bars[i];

            if (ap->size != 0 && start >= ap->cpu_address &&
                start + size <= ap->cpu_address + ap->size)
            {
                dip = r->nvis->dip;
                cfg_offset = ap->offset;
                bar_base = ap->cpu_address;
                break;
            }
        }
    }
    mutex_exit(&nv_sol_lock);

    if (dip == NULL)
        return NULL;

    if (nv_bar_rnumber(dip, cfg_offset, &rnumber) != DDI_SUCCESS)
        return NULL;

    /* Registers and BAR memory are little-endian; see the file comment. */
    attr.devacc_attr_version = DDI_DEVICE_ATTR_V0;
    attr.devacc_attr_endian_flags = DDI_STRUCTURE_LE_ACC;
    attr.devacc_attr_dataorder =
        merging ? DDI_MERGING_OK_ACC : DDI_STRICTORDER_ACC;

    if (ddi_regs_map_setup(dip, rnumber, &va, (offset_t)(start - bar_base),
            (offset_t)size, &attr, &handle) != DDI_SUCCESS)
        return NULL;

    m = kmem_zalloc(sizeof (*m), KM_SLEEP);
    m->va = va;
    m->handle = handle;
    mutex_enter(&nv_sol_lock);
    m->next = nv_sol_maps;
    nv_sol_maps = m;
    mutex_exit(&nv_sol_lock);

    return va;
}

void
nv_sol_unmap_bar(void *addr)
{
    nv_sol_bar_map_t **pp, *m = NULL;

    mutex_enter(&nv_sol_lock);
    for (pp = &nv_sol_maps; *pp != NULL; pp = &(*pp)->next)
    {
        if ((*pp)->va == (caddr_t)addr)
        {
            m = *pp;
            *pp = m->next;
            break;
        }
    }
    mutex_exit(&nv_sol_lock);

    if (m == NULL)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: os_unmap_kernel_space: %p is not a BAR mapping\n", addr);
        return;
    }

    ddi_regs_map_free(&m->handle);
    kmem_free(m, sizeof (*m));
}

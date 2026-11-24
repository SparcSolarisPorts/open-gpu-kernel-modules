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

#ifndef _NV_PX_RANGES_H_
#define _NV_PX_RANGES_H_

/*
 * Bus-to-CPU address translation through the "ranges" property of a sun4v px
 * PCIe root complex.
 *
 * A PCI BAR address in "assigned-addresses" is a bus address, and every root
 * complex has its own bus address space.  The CPU reaches it through a window
 * that the px node describes, one entry of seven 32-bit cells each:
 *
 *   child:  phys.hi  phys.mid  phys.lo     (PCI address, space code in phys.hi)
 *   parent: addr.hi  addr.lo               (CPU physical address)
 *   size:   size.hi  size.lo
 *
 * This header has no kernel dependencies so the arithmetic can be tested
 * outside the kernel.
 */

#include <sys/types.h>

#define NV_PX_RANGE_CELLS       7

/* Space code is bits 25:24 of phys.hi: 0 config, 1 I/O, 2 mem32, 3 mem64. */
#define NV_PX_RANGE_SPACE(hi)   (((uint32_t)(hi) >> 24) & 0x3U)

/*
 * Translate the bus range [bus, bus + size) to a CPU physical address.
 * Returns 0 and sets *cpu on success, or -1 if no memory window of the root
 * complex covers the whole range.  The entries are the raw cells of the
 * property, nints of them.
 */
static inline int
nv_px_ranges_translate(const int *cells, uint_t nints, uint64_t bus,
    uint64_t size, uint64_t *cpu)
{
    uint_t i, nent;

    if (cells == NULL || size == 0 || (nints % NV_PX_RANGE_CELLS) != 0)
        return -1;

    nent = nints / NV_PX_RANGE_CELLS;
    for (i = 0; i < nent; i++)
    {
        const uint32_t *e = (const uint32_t *)&cells[i * NV_PX_RANGE_CELLS];
        uint32_t space = NV_PX_RANGE_SPACE(e[0]);
        uint64_t cbase, pbase, csize;

        /* Only memory windows apply to BARs. */
        if (space != 2 && space != 3)
            continue;

        cbase = ((uint64_t)e[1] << 32) | e[2];
        pbase = ((uint64_t)e[3] << 32) | e[4];
        csize = ((uint64_t)e[5] << 32) | e[6];

        if (csize == 0 || bus < cbase)
            continue;

        /* written to avoid overflow of cbase + csize and bus + size */
        if (bus - cbase >= csize || size > csize - (bus - cbase))
            continue;

        *cpu = pbase + (bus - cbase);
        return 0;
    }

    return -1;
}

#endif /* _NV_PX_RANGES_H_ */

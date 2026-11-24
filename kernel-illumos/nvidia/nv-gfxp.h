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

#ifndef _NV_GFXP_H_
#define _NV_GFXP_H_

/*
 * illumos provides the gfxp_* helpers (kernel-space mapping, umem cookies,
 * the boot framebuffer console) in misc/gfx_private.  Oracle Solaris 11.4 has
 * neither the header nor the module.
 *
 * On Solaris this header declares only the types and constants that the rest
 * of the layer needs.  The gfxp_* functions themselves are deliberately not
 * declared, so that every call that still needs a Solaris replacement fails
 * to compile instead of loading with an unresolved symbol.
 */
#if defined(NV_SOLARIS)

typedef void *gfxp_fb_softc_ptr_t;

#define GFXP_MEMORY_CACHED          0
#define GFXP_MEMORY_UNCACHED        1
#define GFXP_MEMORY_WRITECOMBINED   2

#else

#include <sys/gfx_private.h>

#endif /* NV_SOLARIS */

#endif /* _NV_GFXP_H_ */

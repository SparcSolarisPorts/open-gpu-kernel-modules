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

#ifndef _NV_SOLARIS_COMPAT_H_
#define _NV_SOLARIS_COMPAT_H_

/*
 * Small differences between the illumos kernel interfaces this layer was
 * written against and Oracle Solaris 11.4.  Everything here is compiled only
 * with NV_SOLARIS defined, so the illumos build is unchanged.
 */
#if defined(NV_SOLARIS)

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/systm.h>
#include <sys/varargs.h>
#include <sys/poll.h>
#include <sys/taskq.h>

/*
 * illumos dev_err(dip, level, fmt, ...): a cmn_err() that names the device.
 * The leading '!', '?' and '^' routing characters of fmt are honoured.
 */
static void
nv_sol_dev_err(dev_info_t *dip, int level, const char *fmt, ...)
{
    char buf[256];
    char prefix = '\0';
    va_list ap;

    if (fmt != NULL && (fmt[0] == '!' || fmt[0] == '?' || fmt[0] == '^'))
    {
        prefix = fmt[0];
        fmt++;
    }

    va_start(ap, fmt);
    (void) vsnprintf(buf, sizeof (buf), fmt, ap);
    va_end(ap);

    if (dip == NULL)
    {
        switch (prefix)
        {
            case '!': cmn_err(level, "!nvidia: %s", buf); break;
            case '?': cmn_err(level, "?nvidia: %s", buf); break;
            case '^': cmn_err(level, "^nvidia: %s", buf); break;
            default:  cmn_err(level, "nvidia: %s", buf); break;
        }
        return;
    }

    switch (prefix)
    {
        case '!': cmn_err(level, "!%s%d: %s", ddi_driver_name(dip),
                      ddi_get_instance(dip), buf); break;
        case '?': cmn_err(level, "?%s%d: %s", ddi_driver_name(dip),
                      ddi_get_instance(dip), buf); break;
        case '^': cmn_err(level, "^%s%d: %s", ddi_driver_name(dip),
                      ddi_get_instance(dip), buf); break;
        default:  cmn_err(level, "%s%d: %s", ddi_driver_name(dip),
                      ddi_get_instance(dip), buf); break;
    }
}

#define dev_err                 nv_sol_dev_err

#ifndef CTASSERT
#define CTASSERT(x)             _Static_assert((x), #x)
#endif

/* Edge-triggered poll is an illumos extension; level-triggered is correct. */
#ifndef POLLET
#define POLLET                  0
#endif

/* taskq_dispatch() returns 0 when it fails to dispatch. */
#ifndef TASKQID_INVALID
#define TASKQID_INVALID         ((taskqid_t)0)
#endif

/*
 * Solaris has no taskq_dispatch_ent(), whose entry is preallocated so that it
 * cannot fail.  Callers that must recover from a failed TQ_NOSLEEP dispatch
 * (the interrupt bottom halves) call taskq_dispatch() directly.
 */
#define taskq_dispatch_ent(tq, func, arg, flags, ent) \
    ((void) taskq_dispatch((tq), (func), (arg), (flags)))

#endif /* NV_SOLARIS */

#endif /* _NV_SOLARIS_COMPAT_H_ */

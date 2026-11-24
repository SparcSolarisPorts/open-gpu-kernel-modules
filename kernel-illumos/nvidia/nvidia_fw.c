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
 * GSP firmware images live in /kernel/firmware/nvidia/<version>/, the
 * illumos counterpart of /lib/firmware/nvidia/<version>/ on Linux.
 */
#define NV_FIRMWARE_FOR_NAME(name)  NV_VERSION_STRING "/" name ".bin"

#include "nv-illumos.h"
#include "nv-firmware.h"

#if !defined(NV_SOLARIS)
#include <sys/firmload.h>
#endif

typedef struct nv_firmware_s {
    void   *data;
    size_t  size;
} nv_firmware_t;

#if defined(NV_SOLARIS)

#include <sys/kobj.h>

/* Solaris has no firmload(9F); images are read with the kobj file helpers. */
#define NV_SOL_FIRMWARE_DIR     "/kernel/firmware/nvidia/"
#define NV_SOL_READ_CHUNK       (1U << 20)

/*
 * Read all of path into a new buffer.  Returns 0 or an errno value; free the
 * buffer with kmem_free(*bufp, *sizep).
 */
int
nv_sol_read_file(const char *path, void **bufp, size_t *sizep)
{
    struct _buf *kf;
    uint64_t fsize = 0;
    char *buf;
    size_t done = 0;

    kf = kobj_open_file((char *)path);
    if (kf == (struct _buf *)-1)
        return ENOENT;

    if (kobj_get_filesize(kf, &fsize) != 0 || fsize == 0 || fsize > NV_U32_MAX)
    {
        kobj_close_file(kf);
        return EINVAL;
    }

    buf = kmem_alloc((size_t)fsize, KM_NOSLEEP);
    if (buf == NULL)
    {
        kobj_close_file(kf);
        return ENOMEM;
    }

    while (done < (size_t)fsize)
    {
        size_t chunk = MIN((size_t)fsize - done, (size_t)NV_SOL_READ_CHUNK);
        int n = kobj_read_file(kf, buf + done, (unsigned)chunk,
            (unsigned)done);

        if (n <= 0)
        {
            kmem_free(buf, (size_t)fsize);
            kobj_close_file(kf);
            return EIO;
        }
        done += (size_t)n;
    }

    kobj_close_file(kf);
    *bufp = buf;
    *sizep = (size_t)fsize;
    return 0;
}

const void* NV_API_CALL nv_get_firmware(
    nv_state_t *nv,
    nv_firmware_type_t fw_type,
    nv_firmware_chip_family_t fw_chip_family,
    const void **fw_buf,
    NvU32 *fw_size
)
{
    const char *name = nv_firmware_for_chip_family(fw_type, fw_chip_family);
    char path[256];
    nv_firmware_t *fw;
    int err;

    if (name[0] == '\0')
        return NULL;

    (void) snprintf(path, sizeof (path), "%s%s", NV_SOL_FIRMWARE_DIR, name);

    fw = kmem_zalloc(sizeof (*fw), KM_SLEEP);
    err = nv_sol_read_file(path, &fw->data, &fw->size);
    if (err != 0)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
            "failed to read firmware %s (error %d)\n", path, err);
        kmem_free(fw, sizeof (*fw));
        return NULL;
    }

    *fw_buf = fw->data;
    *fw_size = (NvU32)fw->size;

    return fw;
}

#else /* !NV_SOLARIS */

const void* NV_API_CALL nv_get_firmware(
    nv_state_t *nv,
    nv_firmware_type_t fw_type,
    nv_firmware_chip_family_t fw_chip_family,
    const void **fw_buf,
    NvU32 *fw_size
)
{
    const char *name = nv_firmware_for_chip_family(fw_type, fw_chip_family);
    firmware_handle_t fh;
    nv_firmware_t *fw;
    off_t size;
    int err;

    if (name[0] == '\0')
        return NULL;

    err = firmware_open(NV_ILLUMOS_DRIVER_NAME, name, &fh);
    if (err != 0)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
            "failed to open firmware %s/%s (error %d)\n",
            NV_ILLUMOS_DRIVER_NAME, name, err);
        return NULL;
    }

    size = firmware_get_size(fh);
    if (size <= 0 || size > NV_U32_MAX)
    {
        (void) firmware_close(fh);
        return NULL;
    }

    fw = kmem_zalloc(sizeof (*fw), KM_SLEEP);
    fw->size = (size_t)size;
    fw->data = kmem_alloc(fw->size, KM_NOSLEEP | KM_NORMALPRI);
    if (fw->data == NULL)
    {
        (void) firmware_close(fh);
        kmem_free(fw, sizeof (*fw));
        return NULL;
    }

    err = firmware_read(fh, 0, fw->data, fw->size);
    (void) firmware_close(fh);
    if (err != 0)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
            "failed to read firmware %s/%s (error %d)\n",
            NV_ILLUMOS_DRIVER_NAME, name, err);
        kmem_free(fw->data, fw->size);
        kmem_free(fw, sizeof (*fw));
        return NULL;
    }

    *fw_buf = fw->data;
    *fw_size = (NvU32)fw->size;

    return fw;
}

#endif /* NV_SOLARIS */

void NV_API_CALL nv_put_firmware(const void *fw_handle)
{
    nv_firmware_t *fw = (nv_firmware_t *)fw_handle;

    if (fw == NULL)
        return;

    kmem_free(fw->data, fw->size);
    kmem_free(fw, sizeof (*fw));
}

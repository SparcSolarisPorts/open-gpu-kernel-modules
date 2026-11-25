/*
 * nvkmsprobe: smallest client of /dev/nvidia-modeset (NVKMS). It allocates
 * an NVKMS device for GPU 0, queries its first disp and frees it again. It
 * does not take modeset ownership and does not change what the display shows.
 *
 * Allocating the device makes NVKMS allocate the display objects and the core
 * channel, so this is the first thing that exercises the push buffer and
 * notifier byte swaps and the display allocations that cross the GSP.
 *
 * Build (from the top of the repository, on the target):
 *
 *   gcc -m64 -Wall -o /tmp/nvkmsprobe kernel-illumos/nvkmsprobe.c \
 *     -Isrc/nvidia-modeset/interface -Isrc/common/unix/common/inc \
 *     -Isrc/common/sdk/nvidia/inc -Isrc/common/sdk/nvidia/inc/hw \
 *     -Isrc/common/inc -Ikernel-open/nvidia-modeset
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__sun)
#include <stropts.h>
#else
#include <sys/ioctl.h>
#endif

#include <nvtypes.h>
#include "nvkms-api.h"
#include "nvkms-ioctl.h"
#include "nvUnixVersion.h"

/* Same layout as _IOWR('m', 0, struct NvKmsIoctlParams) on Solaris and Linux. */
#define NVKP_IOWR \
    (0xc0000000U | ((unsigned int)sizeof (struct NvKmsIoctlParams) << 16) | \
     ((unsigned int)NVKMS_IOCTL_MAGIC << 8) | (unsigned int)NVKMS_IOCTL_CMD)

static int nfail;

static int
nvkms_call(int fd, NvU32 cmd, void *params, size_t size, const char *what)
{
    struct NvKmsIoctlParams p;

    memset(&p, 0, sizeof (p));
    p.cmd = cmd;
    p.size = (NvU32)size;
    p.address = (NvU64)(NvUPtr)params;

    if (ioctl(fd, (int)NVKP_IOWR, &p) != 0) {
        printf("%-22s ioctl failed: errno %d (%s)\n", what, errno,
            strerror(errno));
        nfail++;
        return (-1);
    }
    return (0);
}

int
main(void)
{
    static struct NvKmsAllocDeviceParams alloc;
    static struct NvKmsQueryDispParams disp;
    struct NvKmsFreeDeviceParams fr;
    int fd;
    unsigned int i;

    fd = open("/dev/nvidia-modeset", O_RDWR);
    if (fd < 0) {
        perror("open /dev/nvidia-modeset");
        return (2);
    }
    printf("opened /dev/nvidia-modeset (fd %d)\n", fd);
    printf("sizeof NvKmsIoctlParams %u, AllocDeviceParams %u, QueryDispParams %u\n",
        (unsigned int)sizeof (struct NvKmsIoctlParams),
        (unsigned int)sizeof (alloc), (unsigned int)sizeof (disp));

    memset(&alloc, 0, sizeof (alloc));
    strncpy(alloc.request.versionString, NV_VERSION_STRING,
        sizeof (alloc.request.versionString) - 1);
    alloc.request.deviceId.rmDeviceId = 0;
    alloc.request.no3d = NV_TRUE;
    alloc.request.enableConsoleHotplugHandling = NV_TRUE;

    if (nvkms_call(fd, NVKMS_IOCTL_ALLOC_DEVICE, &alloc, sizeof (alloc),
        "alloc device") != 0) {
        close(fd);
        return (1);
    }
    printf("%-22s status %d (0 = success)\n", "alloc device",
        (int)alloc.reply.status);
    if (alloc.reply.status != NVKMS_ALLOC_DEVICE_STATUS_SUCCESS) {
        nfail++;
        close(fd);
        printf("done, %d failure(s)\n", nfail);
        return (1);
    }
    printf("  deviceHandle 0x%08x subDeviceMask 0x%x numHeads %u numDisps %u\n",
        (unsigned int)alloc.reply.deviceHandle,
        (unsigned int)alloc.reply.subDeviceMask,
        (unsigned int)alloc.reply.numHeads,
        (unsigned int)alloc.reply.numDisps);

    for (i = 0; i < alloc.reply.numDisps && i < NVKMS_MAX_SUBDEVICES; i++) {
        memset(&disp, 0, sizeof (disp));
        disp.request.deviceHandle = alloc.reply.deviceHandle;
        disp.request.dispHandle = alloc.reply.dispHandles[i];
        if (nvkms_call(fd, NVKMS_IOCTL_QUERY_DISP, &disp, sizeof (disp),
            "query disp") == 0) {
            printf("  disp %u handle 0x%08x: validDpys 0x%08x bootDpys 0x%08x "
                "connectors %u\n", i, (unsigned int)alloc.reply.dispHandles[i],
                (unsigned int)nvDpyIdListToNvU32(disp.reply.validDpys),
                (unsigned int)nvDpyIdListToNvU32(disp.reply.bootDpys),
                (unsigned int)disp.reply.numConnectors);
            printf("  gpuString \"%.*s\"\n", (int)sizeof (disp.reply.gpuString),
                disp.reply.gpuString);
        }
    }

    memset(&fr, 0, sizeof (fr));
    fr.request.deviceHandle = alloc.reply.deviceHandle;
    (void) nvkms_call(fd, NVKMS_IOCTL_FREE_DEVICE, &fr, sizeof (fr),
        "free device");

    close(fd);
    printf("done, %d failure(s)\n", nfail);
    return (nfail != 0);
}

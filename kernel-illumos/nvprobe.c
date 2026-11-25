/*
 * nvprobe: smallest useful user-space client of the RM ioctl path of the
 * illumos/Solaris nvidia driver. It opens /dev/nvidiactl and makes the calls
 * every RM client makes first: version query, root client allocation, GPU id
 * queries, free. It does not attach or initialise a GPU.
 *
 * Build (from the top of the repository, on the target):
 *
 *   gcc -m64 -Wall -o /tmp/nvprobe kernel-illumos/nvprobe.c \
 *     -Isrc/common/sdk/nvidia/inc -Isrc/common/sdk/nvidia/inc/hw \
 *     -Isrc/nvidia/arch/nvalloc/unix/include \
 *     -Isrc/nvidia/arch/nvalloc/common/inc -Isrc/common/inc
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
#include <nvstatus.h>
#include <nvos.h>
#include <nv-ioctl.h>
#include <nv_escape.h>
#include <class/cl0000.h>
#include <ctrl/ctrl0000/ctrl0000gpu.h>

/* Same layout as the driver's NV_IOC_* macros (and Linux _IOWR). */
#define NVP_IOWR(nr, size)                                          \
    (0xc0000000U | ((unsigned int)(size) << 16) |                   \
     ((unsigned int)NV_IOCTL_MAGIC << 8) | (unsigned int)(nr))

static int nfail;

static int
rm_ioctl_call(int fd, unsigned int nr, void *p, size_t size, const char *what)
{
    int rc = ioctl(fd, (int)NVP_IOWR(nr, size), p);
    int e = errno;

    if (rc != 0) {
        printf("%-26s ioctl failed: errno %d (%s)\n", what, e, strerror(e));
        nfail++;
    }
    return (rc);
}

static int
rm_control(int fd, NvHandle hClient, NvHandle hObject, NvU32 cmd,
    void *params, NvU32 size, const char *what)
{
    NVOS54_PARAMETERS c;

    memset(&c, 0, sizeof (c));
    c.hClient = hClient;
    c.hObject = hObject;
    c.cmd = cmd;
    c.params = NV_PTR_TO_NvP64(params);
    c.paramsSize = size;
    if (rm_ioctl_call(fd, NV_ESC_RM_CONTROL, &c, sizeof (c), what) != 0)
        return (-1);
    printf("%-26s rm status 0x%08x\n", what, (unsigned int)c.status);
    if (c.status != NV_OK)
        nfail++;
    return (c.status == NV_OK ? 0 : -1);
}

int
main(void)
{
    int fd;
    nv_ioctl_rm_api_version_t ver;
    NV0000_ALLOC_PARAMETERS ap;
    NVOS21_PARAMETERS a;
    NVOS00_PARAMETERS f;
    NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS att;
    NV0000_CTRL_GPU_GET_PROBED_IDS_PARAMS probed;
    NvHandle hClient;
    unsigned int i;

    fd = open("/dev/nvidiactl", O_RDWR);
    if (fd < 0) {
        perror("open /dev/nvidiactl");
        return (2);
    }
    printf("opened /dev/nvidiactl (fd %d)\n", fd);

    memset(&ver, 0, sizeof (ver));
    ver.cmd = NV_RM_API_VERSION_CMD_QUERY;
    if (rm_ioctl_call(fd, NV_ESC_CHECK_VERSION_STR, &ver, sizeof (ver),
        "version query") == 0)
        printf("%-26s reply %u, RM version \"%.*s\"\n", "version query",
            (unsigned int)ver.reply, NV_RM_API_VERSION_STRING_LENGTH,
            ver.versionString);

    memset(&ap, 0, sizeof (ap));
    ap.processID = (NvU32)getpid();
    memset(&a, 0, sizeof (a));
    a.hClass = NV01_ROOT;
    a.pAllocParms = NV_PTR_TO_NvP64(&ap);
    a.paramsSize = sizeof (ap);
    if (rm_ioctl_call(fd, NV_ESC_RM_ALLOC, &a, sizeof (a),
        "alloc root client") != 0) {
        close(fd);
        return (1);
    }
    printf("%-26s rm status 0x%08x, hObjectNew 0x%08x, params.hClient 0x%08x\n",
        "alloc root client", (unsigned int)a.status,
        (unsigned int)a.hObjectNew, (unsigned int)ap.hClient);
    hClient = (ap.hClient != 0) ? ap.hClient : a.hObjectNew;
    if (a.status != NV_OK || hClient == 0) {
        printf("no client handle; stopping\n");
        close(fd);
        return (1);
    }

    memset(&probed, 0, sizeof (probed));
    if (rm_control(fd, hClient, hClient, NV0000_CTRL_CMD_GPU_GET_PROBED_IDS,
        &probed, sizeof (probed), "get probed gpu ids") == 0) {
        for (i = 0; i < NV0000_CTRL_GPU_MAX_PROBED_GPUS; i++) {
            if (probed.gpuIds[i] == NV0000_CTRL_GPU_INVALID_ID)
                break;
            printf("  probed gpu id 0x%08x flags 0x%08x\n",
                (unsigned int)probed.gpuIds[i],
                (unsigned int)probed.gpuFlags[i]);
        }
    }

    memset(&att, 0, sizeof (att));
    if (rm_control(fd, hClient, hClient, NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS,
        &att, sizeof (att), "get attached gpu ids") == 0) {
        for (i = 0; i < NV0000_CTRL_GPU_MAX_ATTACHED_GPUS; i++) {
            if (att.gpuIds[i] == NV0000_CTRL_GPU_INVALID_ID)
                break;
            printf("  attached gpu id 0x%08x\n", (unsigned int)att.gpuIds[i]);
        }
    }

    memset(&f, 0, sizeof (f));
    f.hRoot = hClient;
    f.hObjectParent = hClient;
    f.hObjectOld = hClient;
    if (rm_ioctl_call(fd, NV_ESC_RM_FREE, &f, sizeof (f),
        "free root client") == 0) {
        printf("%-26s rm status 0x%08x\n", "free root client",
            (unsigned int)f.status);
        if (f.status != NV_OK)
            nfail++;
    }

    close(fd);
    printf("done, %d failure(s)\n", nfail);
    return (nfail != 0);
}

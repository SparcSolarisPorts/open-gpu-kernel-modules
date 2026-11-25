/*
 * nvprobe: smallest useful user-space client of the RM ioctl path of the
 * illumos/Solaris nvidia driver. It opens /dev/nvidiactl and makes the calls
 * every RM client makes first: version query, root client allocation, GPU id
 * queries. Stage 2 then attaches the GPU, allocates a device and a subdevice
 * (both go through the GSP) and reads the GPU UUID. Finally it frees the
 * client, which frees everything below it.
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
#include <class/cl0080.h>
#include <class/cl2080.h>
#include <ctrl/ctrl0000/ctrl0000gpu.h>
#include <ctrl/ctrl2080/ctrl2080gpu.h>

#define NVP_H_DEVICE    0xde000001U
#define NVP_H_SUBDEVICE 0xde000002U

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

static int
rm_alloc(int fd, NvHandle hClient, NvHandle hParent, NvHandle hNew,
    NvU32 hClass, void *params, NvU32 size, const char *what)
{
    NVOS21_PARAMETERS a;

    memset(&a, 0, sizeof (a));
    a.hRoot = hClient;
    a.hObjectParent = hParent;
    a.hObjectNew = hNew;
    a.hClass = hClass;
    a.pAllocParms = NV_PTR_TO_NvP64(params);
    a.paramsSize = size;
    if (rm_ioctl_call(fd, NV_ESC_RM_ALLOC, &a, sizeof (a), what) != 0)
        return (-1);
    printf("%-26s rm status 0x%08x\n", what, (unsigned int)a.status);
    if (a.status != NV_OK)
        nfail++;
    return (a.status == NV_OK ? 0 : -1);
}

/*
 * Stage 2: attach the GPU for this client, allocate a device and a
 * subdevice (both are forwarded to the GSP) and read the GPU UUID.
 */
static void
stage2(int fd, NvHandle hClient, NvU32 gpuId)
{
    NV0000_CTRL_GPU_ATTACH_IDS_PARAMS at;
    NV0080_ALLOC_PARAMETERS dev;
    NV2080_ALLOC_PARAMETERS sub;
    NV2080_CTRL_GPU_GET_GID_INFO_PARAMS gid;
    unsigned int i;

    memset(&at, 0, sizeof (at));
    at.gpuIds[0] = gpuId;
    for (i = 1; i < NV0000_CTRL_GPU_MAX_PROBED_GPUS; i++)
        at.gpuIds[i] = NV0000_CTRL_GPU_INVALID_ID;
    if (rm_control(fd, hClient, hClient, NV0000_CTRL_CMD_GPU_ATTACH_IDS,
        &at, sizeof (at), "attach gpu id") != 0) {
        printf("  failedId 0x%08x\n", (unsigned int)at.failedId);
        return;
    }

    memset(&dev, 0, sizeof (dev));
    dev.deviceId = 0;
    if (rm_alloc(fd, hClient, hClient, NVP_H_DEVICE, NV01_DEVICE_0,
        &dev, sizeof (dev), "alloc device") != 0)
        return;

    memset(&sub, 0, sizeof (sub));
    sub.subDeviceId = 0;
    if (rm_alloc(fd, hClient, NVP_H_DEVICE, NVP_H_SUBDEVICE, NV20_SUBDEVICE_0,
        &sub, sizeof (sub), "alloc subdevice") != 0)
        return;

    memset(&gid, 0, sizeof (gid));
    gid.index = 0;
    gid.flags = NV2080_GPU_CMD_GPU_GET_GID_FLAGS_FORMAT_BINARY |
        NV2080_GPU_CMD_GPU_GET_GID_FLAGS_TYPE_SHA1;
    if (rm_control(fd, hClient, NVP_H_SUBDEVICE,
        NV2080_CTRL_CMD_GPU_GET_GID_INFO, &gid, sizeof (gid),
        "get gpu uuid") == 0) {
        printf("  uuid length %u: GPU-", (unsigned int)gid.length);
        for (i = 0; i < gid.length && i < sizeof (gid.data); i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                printf("-");
            printf("%02x", gid.data[i]);
        }
        printf("\n");
    }
}

int
main(void)
{
    int fd;
    int gpufd = -1;
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

    if (probed.gpuIds[0] != NV0000_CTRL_GPU_INVALID_ID) {
        /*
         * RM only lets a process allocate a device if the process holds the
         * GPU node open (osIsGpuAccessible). Keep it open until the client
         * is freed.
         */
        gpufd = open("/dev/nvidia0", O_RDWR);
        if (gpufd < 0) {
            perror("open /dev/nvidia0");
            nfail++;
        } else {
            printf("opened /dev/nvidia0 (fd %d)\n", gpufd);
            stage2(fd, hClient, probed.gpuIds[0]);
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

    if (gpufd >= 0)
        close(gpufd);
    close(fd);
    printf("done, %d failure(s)\n", nfail);
    return (nfail != 0);
}

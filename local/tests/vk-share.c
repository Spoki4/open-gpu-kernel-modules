// vk-share: vidmem buffers held by two processes, as a window buffer is held
// by its application and by the compositor. The parent allocates N
// device-local buffers exportable as opaque fds, the child imports every one
// of them (through a socketpair), so each has a second holder in another RM
// client (DupCount 2): what the driver's walks and checks find shared.
//
// Build: ./build.sh tools.
// Usage: vk-share [--count N] [--mb M] [--hold S]
//   --count N  buffers (default 200)
//   --mb M     size of each in MB (default 2)
//   --hold S   seconds both hold them (default 60), then exit
// Output: "START exported=N mb=M" (parent), "IMPORTED n" (child), "RESULT ..." at the end.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define MAX_BUFS 4096

static VkDevice dev;
static uint32_t mem_type = UINT32_MAX;

static int setup(VkInstance *inst)
{
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk-share",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkPhysicalDevice pds[16], pd = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t n = 16, i;
    float prio = 1.0f;
    const char *ext[] = { "VK_KHR_external_memory_fd" };

    if (vkCreateInstance(&ici, NULL, inst) != VK_SUCCESS)
        return -1;
    vkEnumeratePhysicalDevices(*inst, &n, pds);
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.vendorID == 0x10de) {
            pd = pds[i];
            break;
        }
    }
    if (pd == VK_NULL_HANDLE)
        return -1;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mem_type = i;
            break;
        }
    }
    if (mem_type == UINT32_MAX)
        return -1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext };
    return (vkCreateDevice(pd, &dci, NULL, &dev) == VK_SUCCESS) ? 0 : -1;
}

static int send_fd(int sock, int fd)
{
    char c = 0, buf[CMSG_SPACE(sizeof(int))];
    struct iovec io = { .iov_base = &c, .iov_len = 1 };
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof(buf) };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);

    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    return (sendmsg(sock, &m, 0) == 1) ? 0 : -1;
}

static int recv_fd(int sock)
{
    char c, buf[CMSG_SPACE(sizeof(int))];
    struct iovec io = { .iov_base = &c, .iov_len = 1 };
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof(buf) };
    struct cmsghdr *cm;
    int fd = -1;

    if (recvmsg(sock, &m, 0) != 1)
        return -1;
    cm = CMSG_FIRSTHDR(&m);
    if ((cm == NULL) || (cm->cmsg_type != SCM_RIGHTS))
        return -1;
    memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    return fd;
}

int main(int argc, char **argv)
{
    unsigned count = 200, mb = 2, hold = 60, i, done = 0;
    int sv[2];
    pid_t child;
    VkInstance inst;
    static VkDeviceMemory mem[MAX_BUFS];

    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--count") && a + 1 < argc)
            count = (unsigned)strtoul(argv[++a], NULL, 0);
        else if (!strcmp(argv[a], "--mb") && a + 1 < argc)
            mb = (unsigned)strtoul(argv[++a], NULL, 0);
        else if (!strcmp(argv[a], "--hold") && a + 1 < argc)
            hold = (unsigned)strtoul(argv[++a], NULL, 0);
        else {
            fprintf(stderr, "usage: %s [--count N] [--mb M] [--hold S]\n", argv[0]);
            return 2;
        }
    }
    if (count > MAX_BUFS)
        count = MAX_BUFS;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    child = fork();
    if (child < 0)
        return 2;
    if (child == 0) {
        // The second holder: imports what the parent sends.
        close(sv[0]);
        if (setup(&inst) != 0) {
            printf("ERROR child vulkan\n");
            return 2;
        }
        for (i = 0; i < count; i++) {
            int fd = recv_fd(sv[1]);
            VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd };
            VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                                         .allocationSize = (VkDeviceSize)mb << 20, .memoryTypeIndex = mem_type };

            if (fd < 0)
                break;
            if (vkAllocateMemory(dev, &mai, NULL, &mem[i]) != VK_SUCCESS) {
                close(fd);
                break;
            }
            done++;
        }
        printf("IMPORTED %u\n", done);
        sleep(hold);
        for (i = 0; i < done; i++)
            vkFreeMemory(dev, mem[i], NULL);
        return done == count ? 0 : 1;
    }
    close(sv[1]);
    if (setup(&inst) != 0) {
        printf("ERROR parent vulkan\n");
        kill(child, SIGKILL);
        return 2;
    }
    PFN_vkGetMemoryFdKHR getFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    for (i = 0; (getFd != NULL) && (i < count); i++) {
        VkExportMemoryAllocateInfo exp = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exp,
                                     .allocationSize = (VkDeviceSize)mb << 20, .memoryTypeIndex = mem_type };
        VkMemoryGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                     .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        int fd = -1;

        if (vkAllocateMemory(dev, &mai, NULL, &mem[i]) != VK_SUCCESS)
            break;
        gfi.memory = mem[i];
        if ((getFd(dev, &gfi, &fd) != VK_SUCCESS) || (send_fd(sv[0], fd) != 0))
            break;
        close(fd);
        done++;
    }
    printf("START exported=%u mb=%u\n", done, mb);
    shutdown(sv[0], SHUT_WR);
    sleep(hold);
    for (i = 0; i < done; i++)
        vkFreeMemory(dev, mem[i], NULL);
    int st = 0;
    waitpid(child, &st, 0);
    printf("RESULT exported=%u child_exit=%d\n", done, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return (WIFEXITED(st) && (WEXITSTATUS(st) == 0) && (done == count)) ? 0 : 1;
}

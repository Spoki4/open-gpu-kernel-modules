// nvt-idle for the Windows benchmark guest: the background "app" of
// bench.py --bg-fill (nvt-idle-a/b on Linux: nvt-game --idle). Allocates
// --mb MB of device-local Vulkan memory in --chunk-mb buffers, fills each
// once on the GPU, then sits idle with no window until killed: memory the
// game can take only if the OS moves it out. Built in the guest by
// winbench.py setup (csc from .NET Framework; vulkan-1.dll comes with the
// NVIDIA driver). Prints "allocated=N" when done, into --log PATH if given
// (Start-Process -RedirectStandardOutput would hand the child the job
// runner's own output handle: the job could never be cleaned up).
using System;
using System.Runtime.InteropServices;
using System.Threading;

static class NvtIdle
{
    const string VK = "vulkan-1.dll";

    [StructLayout(LayoutKind.Sequential)] struct AppInfo { public int sType; public IntPtr pNext, name; public uint appVer; public IntPtr engine; public uint engVer, api; }
    [StructLayout(LayoutKind.Sequential)] struct InstInfo { public int sType; public IntPtr pNext; public uint flags; public IntPtr app; public uint nl; public IntPtr l; public uint ne; public IntPtr e; }
    [StructLayout(LayoutKind.Sequential)] struct QueueInfo { public int sType; public IntPtr pNext; public uint flags, family, count; public IntPtr prios; }
    [StructLayout(LayoutKind.Sequential)] struct DevInfo { public int sType; public IntPtr pNext; public uint flags, nq; public IntPtr q; public uint nl; public IntPtr l; public uint ne; public IntPtr e, feats; }
    [StructLayout(LayoutKind.Sequential)] struct BufInfo { public int sType; public IntPtr pNext; public uint flags; public ulong size; public uint usage, sharing, nfam; public IntPtr fams; }
    [StructLayout(LayoutKind.Sequential)] struct MemReq { public ulong size, align; public uint bits; }
    [StructLayout(LayoutKind.Sequential)] struct AllocInfo { public int sType; public IntPtr pNext; public ulong size; public uint type; }
    [StructLayout(LayoutKind.Sequential)] struct PoolInfo { public int sType; public IntPtr pNext; public uint flags, family; }
    [StructLayout(LayoutKind.Sequential)] struct CmdInfo { public int sType; public IntPtr pNext; public ulong pool; public uint level, count; }
    [StructLayout(LayoutKind.Sequential)] struct BeginInfo { public int sType; public IntPtr pNext; public uint flags; public IntPtr inh; }
    [StructLayout(LayoutKind.Sequential)] struct SubmitInfo { public int sType; public IntPtr pNext; public uint nw; public IntPtr w, stages; public uint nc; public IntPtr cmds; public uint ns; public IntPtr s; }

    [DllImport(VK)] static extern int vkCreateInstance(ref InstInfo i, IntPtr a, out IntPtr inst);
    [DllImport(VK)] static extern int vkEnumeratePhysicalDevices(IntPtr inst, ref uint n, IntPtr[] pds);
    [DllImport(VK)] static extern void vkGetPhysicalDeviceProperties(IntPtr pd, byte[] props);
    [DllImport(VK)] static extern void vkGetPhysicalDeviceMemoryProperties(IntPtr pd, byte[] props);
    [DllImport(VK)] static extern int vkCreateDevice(IntPtr pd, ref DevInfo i, IntPtr a, out IntPtr dev);
    [DllImport(VK)] static extern void vkGetDeviceQueue(IntPtr dev, uint fam, uint idx, out IntPtr q);
    [DllImport(VK)] static extern int vkCreateBuffer(IntPtr dev, ref BufInfo i, IntPtr a, out ulong buf);
    [DllImport(VK)] static extern void vkGetBufferMemoryRequirements(IntPtr dev, ulong buf, out MemReq r);
    [DllImport(VK)] static extern int vkAllocateMemory(IntPtr dev, ref AllocInfo i, IntPtr a, out ulong mem);
    [DllImport(VK)] static extern int vkBindBufferMemory(IntPtr dev, ulong buf, ulong mem, ulong off);
    [DllImport(VK)] static extern int vkCreateCommandPool(IntPtr dev, ref PoolInfo i, IntPtr a, out ulong pool);
    [DllImport(VK)] static extern int vkAllocateCommandBuffers(IntPtr dev, ref CmdInfo i, out IntPtr cmd);
    [DllImport(VK)] static extern int vkBeginCommandBuffer(IntPtr cmd, ref BeginInfo i);
    [DllImport(VK)] static extern void vkCmdFillBuffer(IntPtr cmd, ulong buf, ulong off, ulong size, uint data);
    [DllImport(VK)] static extern int vkEndCommandBuffer(IntPtr cmd);
    [DllImport(VK)] static extern int vkQueueSubmit(IntPtr q, uint n, ref SubmitInfo s, ulong fence);
    [DllImport(VK)] static extern int vkQueueWaitIdle(IntPtr q);

    static void Check(int r, string what)
    {
        if (r != 0) { Console.Error.WriteLine(what + ": VkResult " + r); Environment.Exit(1); }
    }

    static int Main(string[] args)
    {
        ulong mb = 1024, chunkMb = 128;
        for (int i = 0; i + 1 < args.Length; i += 2) {
            if (args[i] == "--mb") mb = ulong.Parse(args[i + 1]);
            else if (args[i] == "--chunk-mb") chunkMb = ulong.Parse(args[i + 1]);
            else if (args[i] == "--log") {
                var w = new System.IO.StreamWriter(args[i + 1]) { AutoFlush = true };
                Console.SetOut(w);
                Console.SetError(w);
            }
        }

        var app = new AppInfo { sType = 0, name = Marshal.StringToHGlobalAnsi("nvt-idle"), api = (1u << 22) | (1u << 12) };
        var appH = GCHandle.Alloc(app, GCHandleType.Pinned);
        var ii = new InstInfo { sType = 1, app = appH.AddrOfPinnedObject() };
        IntPtr inst;
        Check(vkCreateInstance(ref ii, IntPtr.Zero, out inst), "vkCreateInstance");

        uint n = 16;
        var pds = new IntPtr[16];
        vkEnumeratePhysicalDevices(inst, ref n, pds);
        IntPtr pd = IntPtr.Zero;
        var props = new byte[1024];
        for (int i = 0; i < n; i++) {
            vkGetPhysicalDeviceProperties(pds[i], props);
            if (BitConverter.ToUInt32(props, 8) == 0x10de) { pd = pds[i]; break; }
        }
        if (pd == IntPtr.Zero) { Console.Error.WriteLine("no NVIDIA Vulkan device"); return 2; }

        // VkPhysicalDeviceMemoryProperties: count, 32 x {flags, heap}, ...
        var mp = new byte[600];
        vkGetPhysicalDeviceMemoryProperties(pd, mp);
        uint ntypes = BitConverter.ToUInt32(mp, 0);

        // Family 0 on NVIDIA is graphics+compute, as in nvt-game.
        var prio = GCHandle.Alloc(new float[] { 1.0f }, GCHandleType.Pinned);
        var qi = new QueueInfo { sType = 2, family = 0, count = 1, prios = prio.AddrOfPinnedObject() };
        var qiH = GCHandle.Alloc(qi, GCHandleType.Pinned);
        var di = new DevInfo { sType = 3, nq = 1, q = qiH.AddrOfPinnedObject() };
        IntPtr dev, queue;
        Check(vkCreateDevice(pd, ref di, IntPtr.Zero, out dev), "vkCreateDevice");
        vkGetDeviceQueue(dev, 0, 0, out queue);

        var pi = new PoolInfo { sType = 39, family = 0 };
        ulong pool;
        Check(vkCreateCommandPool(dev, ref pi, IntPtr.Zero, out pool), "vkCreateCommandPool");
        var ci = new CmdInfo { sType = 40, pool = pool, level = 0, count = 1 };
        IntPtr cmd;
        Check(vkAllocateCommandBuffers(dev, ref ci, out cmd), "vkAllocateCommandBuffers");
        var bi = new BeginInfo { sType = 42, flags = 1 };
        Check(vkBeginCommandBuffer(cmd, ref bi), "vkBeginCommandBuffer");

        ulong done = 0;
        uint k = 0;
        while (done < mb) {
            ulong c = Math.Min(chunkMb, mb - done);
            var bci = new BufInfo { sType = 12, size = c << 20, usage = 0x2 /* TRANSFER_DST */ };
            ulong buf, mem;
            Check(vkCreateBuffer(dev, ref bci, IntPtr.Zero, out buf), "vkCreateBuffer");
            MemReq req;
            vkGetBufferMemoryRequirements(dev, buf, out req);
            uint type = uint.MaxValue;
            for (uint t = 0; t < ntypes; t++) {
                uint flags = BitConverter.ToUInt32(mp, 4 + (int)t * 8);
                // DEVICE_LOCAL, not HOST_VISIBLE: plain VRAM, not the BAR.
                if ((req.bits & (1u << (int)t)) != 0 && (flags & 1) != 0 && (flags & 2) == 0) { type = t; break; }
            }
            if (type == uint.MaxValue) { Console.Error.WriteLine("no device-local memory type"); return 2; }
            var ai = new AllocInfo { sType = 5, size = req.size, type = type };
            int r = vkAllocateMemory(dev, ref ai, IntPtr.Zero, out mem);
            if (r != 0) { Console.WriteLine("stop: vkAllocateMemory " + r + " at " + done + " MB"); break; }
            Check(vkBindBufferMemory(dev, buf, mem, 0), "vkBindBufferMemory");
            vkCmdFillBuffer(cmd, buf, 0, ulong.MaxValue, 0x5a5a0000u + k++);
            done += c;
        }
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        var cmds = GCHandle.Alloc(new IntPtr[] { cmd }, GCHandleType.Pinned);
        var si = new SubmitInfo { sType = 4, nc = 1, cmds = cmds.AddrOfPinnedObject() };
        Check(vkQueueSubmit(queue, 1, ref si, 0), "vkQueueSubmit");
        Check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        Console.WriteLine("allocated=" + done);
        Console.Out.Flush();
        Thread.Sleep(Timeout.Infinite);
        return 0;
    }
}

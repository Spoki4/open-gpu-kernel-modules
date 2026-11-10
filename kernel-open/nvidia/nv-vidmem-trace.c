/*
 * Local patch (not upstream): RM's vidmem trace events (vidmem_trace.h) as
 * Linux tracepoints, see nv-vidmem-trace.h.
 */

#define  __NO_VERSION__
#include "nv-linux.h"
#include "os-interface.h"

#define CREATE_TRACE_POINTS
#include "nv-vidmem-trace.h"

// Event types, as in src/nvidia/inc/kernel/mem_mgr/vidmem_trace.h.
#define NV_VMT_MOVE     1
#define NV_VMT_PAUSE    2
#define NV_VMT_RESERVE  3
#define NV_VMT_MAKEROOM 4
#define NV_VMT_TICK     5
#define NV_VMT_PROC     6
#define NV_VMT_LOCK     7
#define NV_VMT_BUDGET   8
#define NV_VMT_FRAG     9

NvBool NV_API_CALL os_vidmem_trace_on(NvU32 type)
{
    switch (type)
    {
        case NV_VMT_MOVE:     return trace_vidmem_move_enabled();
        case NV_VMT_PAUSE:    return trace_vidmem_pause_enabled();
        case NV_VMT_RESERVE:  return trace_vidmem_reserve_enabled();
        case NV_VMT_MAKEROOM: return trace_vidmem_makeroom_enabled();
        case NV_VMT_TICK:     return trace_vidmem_tick_enabled();
        case NV_VMT_PROC:     return trace_vidmem_proc_enabled();
        case NV_VMT_LOCK:     return trace_rm_lock_enabled();
        case NV_VMT_BUDGET:   return trace_vidmem_budget_enabled();
        case NV_VMT_FRAG:     return trace_vidmem_frag_enabled();
        default:              return NV_FALSE;
    }
}

// Name of process pid (the thread group leader's comm), "-" if gone.
static void nv_vidmem_trace_comm(NvU32 pid, char *comm)
{
    struct task_struct *task;

    strscpy(comm, "-", TASK_COMM_LEN);
    if (pid == 0)
        return;
    if (pid == current->tgid)
    {
        memcpy(comm, current->group_leader->comm, TASK_COMM_LEN);
        return;
    }
    rcu_read_lock();
    task = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
    if (task != NULL)
        memcpy(comm, task->group_leader->comm, TASK_COMM_LEN);
    rcu_read_unlock();
    comm[TASK_COMM_LEN - 1] = '\0';
}

void NV_API_CALL os_vidmem_trace(NvU32 type, NvU32 pid, const NvU64 *a)
{
    char comm[TASK_COMM_LEN];

    if (!os_vidmem_trace_on(type))
        return;
    memset(comm, 0, sizeof(comm));
    if (type != NV_VMT_LOCK && type != NV_VMT_TICK)
        nv_vidmem_trace_comm(pid, comm);

    switch (type)
    {
        case NV_VMT_MOVE:     trace_vidmem_move(pid, comm, a);     break;
        case NV_VMT_PAUSE:    trace_vidmem_pause(pid, comm, a);    break;
        case NV_VMT_RESERVE:  trace_vidmem_reserve(pid, comm, a);  break;
        case NV_VMT_MAKEROOM: trace_vidmem_makeroom(pid, comm, a); break;
        case NV_VMT_TICK:     trace_vidmem_tick(pid, comm, a);     break;
        case NV_VMT_PROC:     trace_vidmem_proc(pid, comm, a);     break;
        case NV_VMT_LOCK:     trace_rm_lock(pid, comm, a);         break;
        case NV_VMT_BUDGET:   trace_vidmem_budget(pid, comm, a);   break;
        case NV_VMT_FRAG:     trace_vidmem_frag(pid, comm, a);     break;
    }
}

//
// Window state changes in the compositor's hints. A snapshot may arrive in
// several writes (shell printf writes line by line), so only states of the
// listed processes are compared; a process missing from a write is not
// reported as gone.
//
#define NV_VMT_HINT_MAX 512

static DEFINE_SPINLOCK(nv_vmt_hint_lock);
static NvU32 nv_vmt_hint_pid[NV_VMT_HINT_MAX];
static NvU8  nv_vmt_hint_state[NV_VMT_HINT_MAX];
static NvU32 nv_vmt_hint_count;

void nv_vidmem_trace_hints(const NvU32 *pids, const NvU8 *states, NvU32 count);  // nv-procfs.c

void nv_vidmem_trace_hints(const NvU32 *pids, const NvU8 *states, NvU32 count)
{
    unsigned long flags;
    NvU32 i, j;

    if (!trace_vidmem_hint_enabled())
        return;

    spin_lock_irqsave(&nv_vmt_hint_lock, flags);
    for (i = 0; i < count; i++)
    {
        NvU32 old = 0;

        for (j = 0; j < nv_vmt_hint_count; j++)
        {
            if (nv_vmt_hint_pid[j] == pids[i])
                break;
        }
        if (j < nv_vmt_hint_count)
        {
            old = nv_vmt_hint_state[j];
        }
        else if (nv_vmt_hint_count < NV_VMT_HINT_MAX)
        {
            nv_vmt_hint_pid[nv_vmt_hint_count++] = pids[i];
        }
        else
        {
            // Full: start over (exited processes pile up otherwise).
            nv_vmt_hint_count = 0;
            j = 0;
            nv_vmt_hint_pid[nv_vmt_hint_count++] = pids[i];
        }
        nv_vmt_hint_state[j] = states[i];
        if (old != states[i])
        {
            char comm[TASK_COMM_LEN];

            nv_vidmem_trace_comm(pids[i], comm);
            trace_vidmem_hint(pids[i], comm, old, states[i]);
        }
    }
    spin_unlock_irqrestore(&nv_vmt_hint_lock, flags);
}

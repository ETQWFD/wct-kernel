/* wcteos 进程表：内核内的真实任务登记（不是写死的演示行）
   内核组件在初始化/运行时注册、改状态、累加 CPU tick；任务管理器直接读这张表。 */
#ifndef WCT_PROC_C
#define WCT_PROC_C

#define PROC_MAX 16

typedef struct {
    int used;
    int pid;
    char name[24];
    char type[16];
    char state[12];
    UINT64 ticks;       /* 该任务累计占用的 TSC 周期 */
    UINT32 mem_kb;
} WCT_PROC;

static WCT_PROC proc_tab[PROC_MAX];
static int proc_next_pid = 1;

static void proc_cpy(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

int proc_spawn(const char *name, const char *type, const char *state, UINT32 mem_kb)
{
    for (int i = 0; i < PROC_MAX; ++i) {
        if (proc_tab[i].used) continue;
        WCT_PROC *p = &proc_tab[i];
        p->used = 1;
        p->pid = proc_next_pid++;
        proc_cpy(p->name, name, sizeof(p->name));
        proc_cpy(p->type, type, sizeof(p->type));
        proc_cpy(p->state, state, sizeof(p->state));
        p->ticks = 0;
        p->mem_kb = mem_kb;
        return p->pid;
    }
    return -1;
}

int proc_total(void)
{
    int n = 0;
    for (int i = 0; i < PROC_MAX; ++i) if (proc_tab[i].used) ++n;
    return n;
}

const WCT_PROC *proc_at(int idx)
{
    int n = 0;
    for (int i = 0; i < PROC_MAX; ++i) {
        if (!proc_tab[i].used) continue;
        if (n == idx) return &proc_tab[i];
        ++n;
    }
    return 0;
}

void proc_set_state(int pid, const char *state)
{
    for (int i = 0; i < PROC_MAX; ++i)
        if (proc_tab[i].used && proc_tab[i].pid == pid) proc_cpy(proc_tab[i].state, state, sizeof(proc_tab[i].state));
}

void proc_add_ticks(int pid, UINT64 t)
{
    for (int i = 0; i < PROC_MAX; ++i)
        if (proc_tab[i].used && proc_tab[i].pid == pid) proc_tab[i].ticks += t;
}

void proc_kill(int pid)
{
    for (int i = 0; i < PROC_MAX; ++i)
        if (proc_tab[i].used && proc_tab[i].pid == pid) { proc_tab[i].used = 0; return; }
}

/* 由内核在启动时登记真实存在的组件 */
static int proc_pid_kernel, proc_pid_shell, proc_pid_gui, proc_pid_disk, proc_pid_pci, proc_pid_net, proc_pid_audio;

void proc_init(void)
{
    for (int i = 0; i < PROC_MAX; ++i) proc_tab[i].used = 0;
    proc_next_pid = 1;
    proc_pid_kernel = proc_spawn("wct-kernel", "kernel", "running", 4096);
    proc_pid_disk   = proc_spawn("ata-pio",    "driver", "ready",   64);
    proc_pid_pci    = proc_spawn("pci-cfg",    "driver", "ready",   32);
    proc_pid_net    = proc_spawn("e1000",      "driver", "ready",   128);
    proc_pid_audio  = proc_spawn("hda",        "driver", "ready",   64);
    proc_pid_shell  = proc_spawn("shell-tty1", "service","running", 256);
    proc_pid_gui    = proc_spawn("desktop-gui","gui",    "running", 3072);
}

#endif

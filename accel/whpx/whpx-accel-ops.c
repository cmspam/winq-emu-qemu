/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX)
 *
 * Copyright Microsoft Corp. 2017
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "system/kvm_int.h"
#include "qemu/main-loop.h"
#include "accel/accel-cpu-ops.h"
#include "system/cpus.h"
#include "qemu/guest-random.h"

#include "system/whpx.h"
#include "system/whpx-internal.h"
#include "system/whpx-all.h"
#include "system/whpx-accel-ops.h"

/*
 * vCPU thread affinity with P-core/E-core awareness.
 *
 * On hybrid CPUs (Intel Alder Lake+), pin vCPU threads to P-cores first
 * for maximum single-thread performance.  Uses GetSystemCpuSetInformation
 * to detect core efficiency classes.
 */
typedef struct {
    int *pcores;
    int *ecores;
    int num_pcores;
    int num_ecores;
    int total;
    bool detected;
} WhpxCoreTopology;

static WhpxCoreTopology core_topology;

typedef BOOL (WINAPI *pGetSystemCpuSetInformation)(
    PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);

static void whpx_detect_core_topology(void)
{
    if (core_topology.detected) {
        return;
    }
    core_topology.detected = true;

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (!kernel32) goto fallback;

    pGetSystemCpuSetInformation getCpuSet = (pGetSystemCpuSetInformation)
        GetProcAddress(kernel32, "GetSystemCpuSetInformation");
    if (!getCpuSet) goto fallback;

    ULONG buflen = 0;
    getCpuSet(NULL, 0, &buflen, GetCurrentProcess(), 0);
    if (buflen == 0) goto fallback;

    BYTE *buf = g_malloc(buflen);
    if (!getCpuSet((PSYSTEM_CPU_SET_INFORMATION)buf, buflen, &buflen,
                   GetCurrentProcess(), 0)) {
        g_free(buf);
        goto fallback;
    }

    /* Count cores by efficiency class */
    int num_cpus = 0;
    BYTE max_efficiency = 0;
    BYTE *ptr = buf;
    while (ptr < buf + buflen) {
        SYSTEM_CPU_SET_INFORMATION *info = (SYSTEM_CPU_SET_INFORMATION *)ptr;
        if (info->Type == CpuSetInformation) {
            num_cpus++;
            if (info->CpuSet.EfficiencyClass > max_efficiency)
                max_efficiency = info->CpuSet.EfficiencyClass;
        }
        ptr += info->Size;
    }

    core_topology.pcores = g_malloc(num_cpus * sizeof(int));
    core_topology.ecores = g_malloc(num_cpus * sizeof(int));
    core_topology.num_pcores = 0;
    core_topology.num_ecores = 0;

    /* Higher EfficiencyClass = more performant (P-core) */
    ptr = buf;
    while (ptr < buf + buflen) {
        SYSTEM_CPU_SET_INFORMATION *info = (SYSTEM_CPU_SET_INFORMATION *)ptr;
        if (info->Type == CpuSetInformation) {
            int lp = info->CpuSet.LogicalProcessorIndex;
            if (max_efficiency > 0 &&
                info->CpuSet.EfficiencyClass == max_efficiency) {
                core_topology.pcores[core_topology.num_pcores++] = lp;
            } else if (max_efficiency > 0) {
                core_topology.ecores[core_topology.num_ecores++] = lp;
            } else {
                /* Non-hybrid: all cores are equal, treat as P-cores */
                core_topology.pcores[core_topology.num_pcores++] = lp;
            }
        }
        ptr += info->Size;
    }
    core_topology.total = num_cpus;
    g_free(buf);
    return;

fallback:
    /* Non-hybrid or detection failed — sequential assignment */
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int n = si.dwNumberOfProcessors;
        core_topology.pcores = g_malloc(n * sizeof(int));
        core_topology.ecores = NULL;
        core_topology.num_pcores = n;
        core_topology.num_ecores = 0;
        core_topology.total = n;
        for (int i = 0; i < n; i++)
            core_topology.pcores[i] = i;
    }
}

static void whpx_pin_vcpu_thread(int vcpu_index)
{
    whpx_detect_core_topology();

    int target_lp;
    if (vcpu_index < core_topology.num_pcores) {
        target_lp = core_topology.pcores[vcpu_index];
    } else if (core_topology.num_ecores > 0) {
        int ecore_idx = (vcpu_index - core_topology.num_pcores)
                        % core_topology.num_ecores;
        target_lp = core_topology.ecores[ecore_idx];
    } else {
        target_lp = core_topology.pcores[vcpu_index % core_topology.num_pcores];
    }

    HANDLE thread = GetCurrentThread();
    DWORD_PTR mask = (DWORD_PTR)1 << target_lp;
    SetThreadAffinityMask(thread, mask);
}

static void *whpx_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    int r;

    rcu_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();

    /* Pin vCPU to a specific core (P-cores first on hybrid CPUs) */
    whpx_pin_vcpu_thread(cpu->cpu_index);
    current_cpu = cpu;

    r = whpx_init_vcpu(cpu);
    if (r < 0) {
        fprintf(stderr, "whpx_init_vcpu failed: %s\n", strerror(-r));
        exit(1);
    }

    /* signal CPU creation */
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        qemu_process_cpu_events(cpu);

        if (cpu_can_run(cpu)) {
            r = whpx_vcpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }
    } while (!cpu->unplug || cpu_can_run(cpu));

    whpx_destroy_vcpu(cpu);
    cpu_thread_signal_destroyed(cpu);
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}

static void whpx_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/WHPX",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, whpx_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static void whpx_kick_vcpu_thread(CPUState *cpu)
{
    if (!qemu_cpu_is_self(cpu)) {
        whpx_vcpu_kick(cpu);
    }
}

static bool whpx_vcpu_thread_is_idle(CPUState *cpu)
{
    return !whpx_irqchip_in_kernel();
}

static bool whpx_supports_guest_debug(void)
{
    return whpx_arch_supports_guest_debug();
}


static void whpx_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = whpx_start_vcpu_thread;
    ops->kick_vcpu_thread = whpx_kick_vcpu_thread;
    ops->cpu_thread_is_idle = whpx_vcpu_thread_is_idle;
    ops->handle_interrupt = generic_handle_interrupt;
    ops->supports_guest_debug = whpx_supports_guest_debug;

    ops->synchronize_post_reset = whpx_cpu_synchronize_post_reset;
    ops->synchronize_post_init = whpx_cpu_synchronize_post_init;
    ops->synchronize_state = whpx_cpu_synchronize_state;
    ops->synchronize_pre_loadvm = whpx_cpu_synchronize_pre_loadvm;
}

static const TypeInfo whpx_accel_ops_type = {
    .name = ACCEL_OPS_NAME("whpx"),

    .parent = TYPE_ACCEL_OPS,
    .class_init = whpx_accel_ops_class_init,
    .abstract = true,
};

static void whpx_accel_ops_register_types(void)
{
    type_register_static(&whpx_accel_ops_type);
}
type_init(whpx_accel_ops_register_types);

/*
 * x86 WHPX CPU type initialization
 *
 * Copyright 2024 QEMU Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "host-cpu.h"
#include "qapi/error.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "system/whpx.h"
#include <winerror.h>
#include "system/whpx-all.h"
#include "accel/accel-cpu-target.h"

/*
 * Query supported CPUID values for WHPX.
 *
 * WHPX does not provide a CPUID query API like KVM's
 * KVM_GET_SUPPORTED_CPUID. Instead, we read CPUID directly from
 * the host, which is what the hypervisor will expose to the guest
 * (modulo features it explicitly hides). This matches how crosvm
 * handles WHPX CPUID.
 */
uint32_t whpx_get_supported_cpuid(uint32_t func, uint32_t idx,
                                  int reg)
{
    uint32_t eax, ebx, ecx, edx;

    host_cpuid(func, idx, &eax, &ebx, &ecx, &edx);

    switch (reg) {
    case R_EAX:
        return eax;
    case R_EBX:
        return ebx;
    case R_ECX:
        return ecx;
    case R_EDX:
        return edx;
    default:
        return 0;
    }
}

static void whpx_cpu_max_instance_init(X86CPU *cpu)
{
    CPUX86State *env = &cpu->env;

    env->cpuid_min_level =
        whpx_get_supported_cpuid(0x0, 0, R_EAX);
    env->cpuid_min_xlevel =
        whpx_get_supported_cpuid(0x80000000, 0, R_EAX);
    env->cpuid_min_xlevel2 =
        whpx_get_supported_cpuid(0xC0000000, 0, R_EAX);
}

static void whpx_cpu_xsave_init(void)
{
    static bool first = true;
    uint32_t eax, ebx, ecx, edx;
    int i;

    if (!first) {
        return;
    }
    first = false;

    /* x87 and SSE states are in the legacy region of the XSAVE area. */
    x86_ext_save_areas[XSTATE_FP_BIT].offset = 0;
    x86_ext_save_areas[XSTATE_SSE_BIT].offset = 0;

    for (i = XSTATE_SSE_BIT + 1; i < XSAVE_STATE_AREA_COUNT; i++) {
        ExtSaveArea *esa = &x86_ext_save_areas[i];

        if (esa->size) {
            host_cpuid(0xd, i, &eax, &ebx, &ecx, &edx);
            if (eax != 0) {
                assert(esa->size == eax);
                esa->offset = ebx;
                esa->ecx = ecx;
            }
        }
    }
}

void whpx_cpu_instance_init(CPUState *cs)
{
    X86CPU *cpu = X86_CPU(cs);
    X86CPUClass *xcc = X86_CPU_GET_CLASS(cpu);

    host_cpu_instance_init(cpu);

    if (xcc->max_features) {
        whpx_cpu_max_instance_init(cpu);
    }

    whpx_cpu_xsave_init();
}

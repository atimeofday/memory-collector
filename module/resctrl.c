#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/workqueue.h>
#include <linux/resctrl.h>
#include <asm/resctrl.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/atomic.h>

#include "resctrl.h"

MODULE_LICENSE("GPL");

#ifndef RESCTRL_RESERVED_RMID
#define RESCTRL_RESERVED_RMID 0
#endif

#define RMID_VAL_ERROR BIT_ULL(63)
#define RMID_VAL_UNAVAIL BIT_ULL(62)

/* Structure to pass RMID to IPI function */
struct ipi_rmid_args {
    u32 rmid;
    int status;
};
/*
 * IPI function to write RMID to MSR
 * Called on each CPU by on_each_cpu()
 */
static void ipi_write_rmid(void *info)
{
    struct ipi_rmid_args *args = info;
    u32 closid = 0;

    // if we're not on CPU 2, don't do anything
    if (smp_processor_id() != 2) {
        args->status = 0;
        return;
    }
    
    if (wrmsr_safe(MSR_IA32_PQR_ASSOC, args->rmid, closid) != 0) {
        args->status = -EIO;
    } else {
        args->status = 0;
    }
}

int resctrl_init_cpu(struct rdt_state *rdt_state)
{
    int cpu = smp_processor_id();
    unsigned int eax, ebx, ecx, edx;
    int ret = 0;

    pr_debug("Memory Collector: Starting enumerate_cpuid on CPU %d\n", cpu);

    memset(rdt_state, 0, sizeof(struct rdt_state));

    if (!boot_cpu_has( X86_FEATURE_CQM_LLC)) {
        pr_debug("Memory Collector: CPU does not support QoS monitoring\n");
        return -ENODEV;
    }

    pr_debug("Memory Collector: Checking CPUID.0x7.0 for RDT support\n");
    cpuid_count(0x7, 0, &eax, &ebx, &ecx, &edx);
    if (!(ebx & (1 << 12))) {
        pr_debug("Memory Collector: RDT monitoring not supported (CPUID.0x7.0:EBX.12)\n");
        return -ENODEV;
    }

    pr_debug("Memory Collector: Checking CPUID.0xF.0 for L3 monitoring\n");
    cpuid_count(0xF, 0, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1 << 1))) {
        pr_debug("Memory Collector: L3 monitoring not supported (CPUID.0xF.0:EDX.1)\n");
        return -ENODEV;
    }

    pr_debug("Memory Collector: Checking CPUID.0xF.1 for L3 occupancy monitoring\n");
    cpuid_count(0xF, 1, &eax, &ebx, &ecx, &edx);
    rdt_state->supports_llc_occupancy = (edx & (1 << 0));
    rdt_state->supports_mbm_total = (edx & (1 << 1));
    rdt_state->supports_mbm_local = (edx & (1 << 2));
    rdt_state->max_rmid = ecx;
    rdt_state->counter_width = (eax & 0xFF) + 24;
    rdt_state->has_overflow_bit = (eax & (1 << 8));
    rdt_state->supports_non_cpu_agent_cache = (eax & (1 << 8));
    rdt_state->supports_non_cpu_agent_mbm = (eax & (1 << 10));

    pr_info("Memory Collector: capabilities of core %d: llc_occupancy: %d, mbm_total: %d, mbm_local: %d, max_rmid: %d, counter_width: %d, has_overflow_bit: %d, supports_non_cpu_agent_cache: %d, supports_non_cpu_agent_mbm: %d\n", 
             cpu, rdt_state->supports_llc_occupancy, rdt_state->supports_mbm_total, rdt_state->supports_mbm_local, rdt_state->max_rmid, rdt_state->counter_width, rdt_state->has_overflow_bit, rdt_state->supports_non_cpu_agent_cache, rdt_state->supports_non_cpu_agent_mbm);
    pr_debug("Memory Collector: enumerate_cpuid completed successfully on CPU %d\n", cpu);
    return ret;
}

int read_rmid_mbm(u32 rmid, u64 *val)
{
    int err;
    
    err = wrmsr_safe(MSR_IA32_QM_EVTSEL, 
                     rmid,
                     QOS_L3_MBM_TOTAL_EVENT_ID);
    if (err)
        return err;

    err = rdmsrl_safe(MSR_IA32_QM_CTR, val);
    if (err)
        return err;
    
    if (*val & RMID_VAL_ERROR)
        return -EIO;
    if (*val & RMID_VAL_UNAVAIL) 
        return -EINVAL;
        
    return 0;
} 
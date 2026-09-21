/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _INTEL_HFI_H
#define _INTEL_HFI_H

#include <linux/sched.h>

#define HFI_ITD_CLASS_SCALAR    0   /* Non-vectorized integer/FP */
#define HFI_ITD_CLASS_VECTOR    1   /* SSE/AVX vectorized */
#define HFI_ITD_CLASS_VNNI      2   /* AVX-VNNI / DL Boost */
#define HFI_ITD_CLASS_SPIN      3   /* Spin-wait / IO-bound */

#define NR_HFI_ITD_CLASSES      4   /* Hardware classids 0-3 */

/* Stability: require 4 consecutive identical ticks before updating ipcc */
#define ITD_CLASS_STABILITY_TICKS  4

/* MSRs do ITD Thread Director */
#define MSR_IA32_HW_FEEDBACK_THREAD_CONFIG  0x17D4
#define HW_FEEDBACK_THREAD_CONFIG_ENABLE_BIT BIT(0)

/* Hardware writes the IPC class here (bits 7:0 = classid, bit 63 = valid) */
#define MSR_IA32_HW_FEEDBACK_CHAR           0x17D2



#if defined(CONFIG_INTEL_HFI_THERMAL)
void __init intel_hfi_init(void);
void intel_hfi_online(unsigned int cpu);
void intel_hfi_offline(unsigned int cpu);
void intel_hfi_process_event(__u64 pkg_therm_status_msr_val);
#else
static inline void intel_hfi_init(void) { }
static inline void intel_hfi_online(unsigned int cpu) { }
static inline void intel_hfi_offline(unsigned int cpu) { }
static inline void intel_hfi_process_event(__u64 pkg_therm_status_msr_val) { }
#endif /* CONFIG_INTEL_HFI_THERMAL */

#endif /* _INTEL_HFI_H */

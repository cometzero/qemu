/*
 * libqemu
 *
 * Copyright (c) 2019 Luc Michel <luc.michel@greensocs.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpregs.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/armv7m_nvic.h"
#include "system/kvm.h"
#include "target/arm/kvm_arm.h"
#include "hw/arm/boot.h"
#include "hw/arm/virt.h"
#include "system/reset.h"
#include "target/arm/arm-powerctl.h"
#include "target/arm/multiprocessing.h"

#include "arm.h"

void libqemu_cpu_arm_set_cp15_cbar(Object *obj, uint64_t cbar)
{
    ARMCPU *cpu = ARM_CPU(obj);
    CPUARMState *env = &cpu->env;

    if (arm_feature(env, ARM_FEATURE_CBAR_RO)) {
        /* XXX discarding const qualifier here to avoid modifying QEMU upstream code */
        uint32_t id = ENCODE_CP_REG(15, 0, 1, 15, 0, 4, 0);
        ARMCPRegInfo *cbar_info = (ARMCPRegInfo *) get_arm_cp_reginfo(cpu->cp_regs, id);
        assert(cbar_info);
        cbar_info->resetvalue = cbar;
    } else {
        env->cp15.c15_config_base_address = cbar;
    }

#if 0
    if (arm_feature(env, ARM_FEATURE_AARCH64)) {
        /* For AArch64, also update the 32 bits view */
        uint32_t id = ENCODE_AA64_CP_REG(0, 15, 3, 3, 1, 0);
        ARMCPRegInfo *cbar_info = (ARMCPRegInfo *) get_arm_cp_reginfo(cpu->cp_regs, id);
        assert(cbar_info);
        cbar_info->resetvalue = cbar;
    }
#endif
}


void libqemu_cpu_aarch64_set_aarch64_mode(Object *obj, bool aarch64_mode)
{
    CPUARMState *cpu = &ARM_CPU(obj)->env;

    cpu->aarch64 = aarch64_mode;
}

void libqemu_cpu_arm_add_nvic_link(Object *obj)
{
    ARMCPU *cpu = ARM_CPU(obj);
    CPUARMState *env = &cpu->env;

    object_property_add_link(obj, "nvic", TYPE_NVIC, (Object **) &env->nvic,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
}

void libqemu_arm_nvic_add_cpu_link(Object *obj)
{
    NVICState *nvic = NVIC(obj);

    object_property_add_link(obj, "cpu", TYPE_ARM_CPU, (Object **) &nvic->cpu,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
}

uint64_t libqemu_cpu_arm_get_exclusive_addr(const Object *obj)
{
    const ARMCPU *cpu = ARM_CPU(obj);
    const CPUARMState *env = &cpu->env;

    return env->exclusive_addr;
}

uint64_t libqemu_cpu_arm_get_exclusive_val(const Object *obj)
{
    const ARMCPU *cpu = ARM_CPU(obj);
    const CPUARMState *env = &cpu->env;

    return env->exclusive_val;
}

void libqemu_cpu_arm_set_exclusive_val(Object *obj, uint64_t val)
{
    ARMCPU *cpu = ARM_CPU(obj);
    CPUARMState *env = &cpu->env;

    env->exclusive_val = val;
}

void libqemu_cpu_arm_set_power_state(Object *obj, bool powered_on)
{
    CPUState *cs = CPU(obj);
    ARMCPU *cpu = ARM_CPU(obj);

    cpu->power_state = powered_on ? PSCI_ON : PSCI_OFF;
    cs->halted = powered_on ? 0 : 1;
}

int libqemu_cpu_arm_get_power_state(Object *obj)
{
    ARMCPU *cpu = ARM_CPU(obj);

    return cpu->power_state;
}

int libqemu_cpu_arm_power_on_and_reset(Object *obj)
{
    ARMCPU *cpu = ARM_CPU(obj);

    return arm_set_cpu_on_and_reset(arm_cpu_mp_affinity(cpu));
}

/*
 * libqemu_cpu_arm_post_init() must be called after the CPUs have been realized
 * and the GIC has been created.
 *
 * Extracted from virt_cpu_post_init(), this is essentialy doing the minimum
 * required by KVM for the "none" machine.
 */
void libqemu_cpu_arm_post_init(Object *obj)
{
    CPUState *cpu = CPU(obj);
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;

    bool pmu = object_property_get_bool(obj, "pmu", NULL);

    if (kvm_enabled()) {
        if (pmu) {
            assert(arm_feature(env, ARM_FEATURE_PMU));
            if (kvm_irqchip_in_kernel()) {
                kvm_arm_pmu_set_irq(arm_cpu, VIRTUAL_PMU_IRQ);
            }
            kvm_arm_pmu_init(arm_cpu);
        }
    }
}

void libqemu_cpu_arm_register_reset(Object *cpu)
{
    qemu_register_reset(do_cpu_reset, ARM_CPU(cpu));
}

uint64_t libqemu_cpu_arm_v7m_get_state(Object *obj, int field)
{
    enum {
        V7M_STATE_R0 = 0,
        V7M_STATE_R1,
        V7M_STATE_R2,
        V7M_STATE_R3,
        V7M_STATE_R4,
        V7M_STATE_R5,
        V7M_STATE_R6,
        V7M_STATE_R7,
        V7M_STATE_R8,
        V7M_STATE_R9,
        V7M_STATE_R10,
        V7M_STATE_R11,
        V7M_STATE_R12,
        V7M_STATE_SP,
        V7M_STATE_LR,
        V7M_STATE_PC,
        V7M_STATE_XPSR,
        V7M_STATE_EXCEPTION,
        V7M_STATE_CPU_EXCEPTION_INDEX,
        V7M_STATE_SECURE,
        V7M_STATE_CFSR_NS,
        V7M_STATE_CFSR_S,
        V7M_STATE_HFSR,
        V7M_STATE_DFSR,
        V7M_STATE_SFSR,
        V7M_STATE_MMFAR_NS,
        V7M_STATE_MMFAR_S,
        V7M_STATE_BFAR,
        V7M_STATE_SFAR,
        V7M_STATE_AIRCR,
        V7M_STATE_VTOR_NS,
        V7M_STATE_VTOR_S,
        V7M_STATE_CONTROL_NS,
        V7M_STATE_CONTROL_S,
        V7M_STATE_PRIMASK_NS,
        V7M_STATE_PRIMASK_S,
        V7M_STATE_FAULTMASK_NS,
        V7M_STATE_FAULTMASK_S,
        V7M_STATE_BASEPRI_NS,
        V7M_STATE_BASEPRI_S,
        V7M_STATE_OTHER_SP,
        V7M_STATE_OTHER_SS_MSP,
        V7M_STATE_OTHER_SS_PSP,
        V7M_STATE_MSPLIM_NS,
        V7M_STATE_MSPLIM_S,
        V7M_STATE_PSPLIM_NS,
        V7M_STATE_PSPLIM_S,
    };
    CPUState *cs = CPU(obj);
    ARMCPU *cpu = ARM_CPU(obj);
    CPUARMState *env = &cpu->env;

    if (!arm_feature(env, ARM_FEATURE_M)) {
        return 0;
    }

    switch (field) {
    case V7M_STATE_R0:
        return env->regs[0];
    case V7M_STATE_R1:
        return env->regs[1];
    case V7M_STATE_R2:
        return env->regs[2];
    case V7M_STATE_R3:
        return env->regs[3];
    case V7M_STATE_R4:
        return env->regs[4];
    case V7M_STATE_R5:
        return env->regs[5];
    case V7M_STATE_R6:
        return env->regs[6];
    case V7M_STATE_R7:
        return env->regs[7];
    case V7M_STATE_R8:
        return env->regs[8];
    case V7M_STATE_R9:
        return env->regs[9];
    case V7M_STATE_R10:
        return env->regs[10];
    case V7M_STATE_R11:
        return env->regs[11];
    case V7M_STATE_R12:
        return env->regs[12];
    case V7M_STATE_SP:
        return env->regs[13];
    case V7M_STATE_LR:
        return env->regs[14];
    case V7M_STATE_PC:
        return env->regs[15];
    case V7M_STATE_XPSR:
        return xpsr_read(env);
    case V7M_STATE_EXCEPTION:
        return env->v7m.exception;
    case V7M_STATE_CPU_EXCEPTION_INDEX:
        return cs->exception_index;
    case V7M_STATE_SECURE:
        return env->v7m.secure;
    case V7M_STATE_CFSR_NS:
        return env->v7m.cfsr[M_REG_NS];
    case V7M_STATE_CFSR_S:
        return env->v7m.cfsr[M_REG_S];
    case V7M_STATE_HFSR:
        return env->v7m.hfsr;
    case V7M_STATE_DFSR:
        return env->v7m.dfsr;
    case V7M_STATE_SFSR:
        return env->v7m.sfsr;
    case V7M_STATE_MMFAR_NS:
        return env->v7m.mmfar[M_REG_NS];
    case V7M_STATE_MMFAR_S:
        return env->v7m.mmfar[M_REG_S];
    case V7M_STATE_BFAR:
        return env->v7m.bfar;
    case V7M_STATE_SFAR:
        return env->v7m.sfar;
    case V7M_STATE_AIRCR:
        return env->v7m.aircr;
    case V7M_STATE_VTOR_NS:
        return env->v7m.vecbase[M_REG_NS];
    case V7M_STATE_VTOR_S:
        return env->v7m.vecbase[M_REG_S];
    case V7M_STATE_CONTROL_NS:
        return env->v7m.control[M_REG_NS];
    case V7M_STATE_CONTROL_S:
        return env->v7m.control[M_REG_S];
    case V7M_STATE_PRIMASK_NS:
        return env->v7m.primask[M_REG_NS];
    case V7M_STATE_PRIMASK_S:
        return env->v7m.primask[M_REG_S];
    case V7M_STATE_FAULTMASK_NS:
        return env->v7m.faultmask[M_REG_NS];
    case V7M_STATE_FAULTMASK_S:
        return env->v7m.faultmask[M_REG_S];
    case V7M_STATE_BASEPRI_NS:
        return env->v7m.basepri[M_REG_NS];
    case V7M_STATE_BASEPRI_S:
        return env->v7m.basepri[M_REG_S];
    case V7M_STATE_OTHER_SP:
        return env->v7m.other_sp;
    case V7M_STATE_OTHER_SS_MSP:
        return env->v7m.other_ss_msp;
    case V7M_STATE_OTHER_SS_PSP:
        return env->v7m.other_ss_psp;
    case V7M_STATE_MSPLIM_NS:
        return env->v7m.msplim[M_REG_NS];
    case V7M_STATE_MSPLIM_S:
        return env->v7m.msplim[M_REG_S];
    case V7M_STATE_PSPLIM_NS:
        return env->v7m.psplim[M_REG_NS];
    case V7M_STATE_PSPLIM_S:
        return env->v7m.psplim[M_REG_S];
    default:
        return 0;
    }
}

bool libqemu_cpu_arm_v7m_set_state(Object *obj, int field, uint64_t value)
{
    enum {
        V7M_STATE_R0 = 0,
        V7M_STATE_R1,
        V7M_STATE_R2,
        V7M_STATE_R3,
        V7M_STATE_R4,
        V7M_STATE_R5,
        V7M_STATE_R6,
        V7M_STATE_R7,
        V7M_STATE_R8,
        V7M_STATE_R9,
        V7M_STATE_R10,
        V7M_STATE_R11,
        V7M_STATE_R12,
        V7M_STATE_SP,
        V7M_STATE_LR,
        V7M_STATE_PC,
    };
    CPUState *cs = CPU(obj);
    ARMCPU *cpu = ARM_CPU(obj);
    CPUARMState *env = &cpu->env;

    if (!arm_feature(env, ARM_FEATURE_M)) {
        return false;
    }

    if (field >= V7M_STATE_R0 && field <= V7M_STATE_LR) {
        env->regs[field] = (uint32_t)value;
        return true;
    }

    if (field == V7M_STATE_PC) {
        CPUClass *cc = CPU_GET_CLASS(cs);

        if (cc->set_pc == NULL) {
            env->regs[15] = (uint32_t)(value & ~1u);
            env->thumb = value & 1u;
        } else {
            cc->set_pc(cs, value);
        }
        return true;
    }

    return false;
}

uint64_t libqemu_cpu_arm_aarch64_get_state(Object *obj, int field)
{
    enum {
        AARCH64_STATE_IS_A64 = 0,
        AARCH64_STATE_PC,
        AARCH64_STATE_SP,
        AARCH64_STATE_LR,
        AARCH64_STATE_PSTATE,
        AARCH64_STATE_CURRENT_EL,
        AARCH64_STATE_CPU_EXCEPTION_INDEX,
        AARCH64_STATE_X0,
        AARCH64_STATE_X1,
        AARCH64_STATE_X2,
        AARCH64_STATE_X3,
        AARCH64_STATE_X4,
        AARCH64_STATE_X5,
        AARCH64_STATE_X6,
        AARCH64_STATE_X7,
        AARCH64_STATE_X29,
        AARCH64_STATE_EXCEPTION_SYNDROME,
        AARCH64_STATE_EXCEPTION_VADDRESS,
        AARCH64_STATE_ESR_EL3,
        AARCH64_STATE_FAR_EL3,
        AARCH64_STATE_ELR_EL3,
        AARCH64_STATE_SP_EL0,
        AARCH64_STATE_SP_EL3,
    };
    CPUState *cs = CPU(obj);
    ARMCPU *cpu = ARM_CPU(obj);
    CPUARMState *env = &cpu->env;

    if (!arm_feature(env, ARM_FEATURE_AARCH64)) {
        return 0;
    }

    switch (field) {
    case AARCH64_STATE_IS_A64:
        return is_a64(env);
    case AARCH64_STATE_PC:
        return env->pc;
    case AARCH64_STATE_SP:
        return env->xregs[31];
    case AARCH64_STATE_LR:
        return env->xregs[30];
    case AARCH64_STATE_PSTATE:
        return pstate_read(env);
    case AARCH64_STATE_CURRENT_EL:
        return (pstate_read(env) & 0xf) >> 2;
    case AARCH64_STATE_CPU_EXCEPTION_INDEX:
        return cs->exception_index;
    case AARCH64_STATE_X0:
        return env->xregs[0];
    case AARCH64_STATE_X1:
        return env->xregs[1];
    case AARCH64_STATE_X2:
        return env->xregs[2];
    case AARCH64_STATE_X3:
        return env->xregs[3];
    case AARCH64_STATE_X4:
        return env->xregs[4];
    case AARCH64_STATE_X5:
        return env->xregs[5];
    case AARCH64_STATE_X6:
        return env->xregs[6];
    case AARCH64_STATE_X7:
        return env->xregs[7];
    case AARCH64_STATE_X29:
        return env->xregs[29];
    case AARCH64_STATE_EXCEPTION_SYNDROME:
        return env->exception.syndrome;
    case AARCH64_STATE_EXCEPTION_VADDRESS:
        return env->exception.vaddress;
    case AARCH64_STATE_ESR_EL3:
        return env->cp15.esr_el[3];
    case AARCH64_STATE_FAR_EL3:
        return env->cp15.far_el[3];
    case AARCH64_STATE_ELR_EL3:
        return env->elr_el[3];
    case AARCH64_STATE_SP_EL0:
        return env->sp_el[0];
    case AARCH64_STATE_SP_EL3:
        return env->sp_el[3];
    default:
        return 0;
    }
}

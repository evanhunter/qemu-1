/*
 * Cortex-M System emulation.
 *
 * Copyright (c) 2014 Liviu Ionescu
 *
 * This code is licensed under the GPL.
 */

#include "config.h"
#include "sysemu/sysemu.h"
#include "hw/arm/cortexm.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "hw/arm/arm.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"
#include "qemu/error-report.h"
#include "sysemu/qtest.h"
#include "hw/loader.h"
#include "elf.h"

/* Redefined from armv7m.c */
#define TYPE_BITBAND "ARM,bitband-memory"

static void cortexm_reset(void *opaque);

static void cortexm_bitband_init(void)
{
    DeviceState *dev;

    dev = qdev_create(NULL, TYPE_BITBAND);
    qdev_prop_set_uint32(dev, "base", 0x20000000);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x22000000);

    dev = qdev_create(NULL, TYPE_BITBAND);
    qdev_prop_set_uint32(dev, "base", 0x40000000);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x42000000);
}


/* Common Cortex-M core initialisation routine.  */
qemu_irq *cortex_m_core_init(cortex_m_core_info *cm_info, MachineState *machine)
{
    int flash_size_kb = cm_info->flash_size_kb;
    int sram_size_kb = cm_info->sram_size_kb;
    
#if defined(CONFIG_VERBOSE)
    if (verbosity_level > 0) {
        const char *cmdline;
        const char *kernel_filename = machine->kernel_filename;
        const char *cpu_model = machine->cpu_model;
        
        printf("Device: '%s' (%s", cm_info->device_name, cpu_model);
        if (cm_info->has_mpu) {
            printf(", MPU");
        }
        if (cm_info->has_fpu) {
            printf(", FPU");
        }
        printf("), Flash: %d KB, RAM: %d KB.\n", flash_size_kb, sram_size_kb);
        if (kernel_filename){
            printf("Image: '%s'.\n", kernel_filename);
        }
        
        cmdline =  semihosting_cmdline;
        if (cmdline == NULL) {
            cmdline = machine->kernel_cmdline;
        }
        
        if (cmdline != NULL) {
            printf("Command line: '%s' (%d bytes).\n", cmdline,
                   (int)strlen(cmdline));
        }
    }
#endif
    
    MemoryRegion *system_memory = cm_info->system_memory;
    if (system_memory == NULL) {
        system_memory = get_system_memory();
    }

    int flash_size = flash_size_kb * 1024;
    int sram_size = sram_size_kb * 1024;
    ARMCPU *cpu;
    CPUARMState *env;
    DeviceState *nvic;
    /* FIXME: make this local state.  */
    static qemu_irq pic[64];
    static struct arm_boot_info cortexm_binfo;
    int image_size;
    uint64_t entry;
    uint64_t lowaddr;
    int i;
    int big_endian;
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MemoryRegion *flash = g_new(MemoryRegion, 1);
    MemoryRegion *hack = g_new(MemoryRegion, 1);
    
    const char *kernel_filename = machine->kernel_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *cpu_model = machine->cpu_model;
    
    if (cpu_model == NULL) {
        cpu_model = "cortex-m3";
    }
    cpu = cpu_arm_init(cpu_model);
    if (cpu == NULL) {
        fprintf(stderr, "Unable to find CPU definition %s\n", cpu_model);
        exit(1);
    }
    env = &cpu->env;
    
#if 0
    /* > 32Mb SRAM gets complicated because it overlaps the bitband area.
     We don't have proper commandline options, so allocate half of memory
     as SRAM, up to a maximum of 32Mb, and the rest as code.  */
    if (ram_size > (512 + 32) * 1024 * 1024)
    ram_size = (512 + 32) * 1024 * 1024;
    sram_size = (ram_size / 2) & TARGET_PAGE_MASK;
    if (sram_size > 32 * 1024 * 1024)
    sram_size = 32 * 1024 * 1024;
    code_size = ram_size - sram_size;
#endif
    
    /* Flash programming is done via the SCU, so pretend it is ROM.  */
    memory_region_init_ram(flash, NULL, "armv7m.flash", flash_size,
                           &error_abort);
    vmstate_register_ram_global(flash);
    memory_region_set_readonly(flash, true);
    memory_region_add_subregion(system_memory, 0, flash);
    memory_region_init_ram(sram, NULL, "armv7m.sram", sram_size, &error_abort);
    vmstate_register_ram_global(sram);
    memory_region_add_subregion(system_memory, 0x20000000, sram);
    cortexm_bitband_init();
    
    nvic = qdev_create(NULL, "armv7m_nvic");
    env->nvic = nvic;
    qdev_init_nofail(nvic);
    sysbus_connect_irq(SYS_BUS_DEVICE(nvic), 0,
                       qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
    for (i = 0; i < 64; i++) {
        pic[i] = qdev_get_gpio_in(nvic, i);
    }
    
#ifdef TARGET_WORDS_BIGENDIAN
    big_endian = 1;
#else
    big_endian = 0;
#endif
    
    if (!kernel_filename && !qtest_enabled() && !with_gdb) {
        fprintf(stderr, "Guest image must be specified (using -kernel)\n");
        exit(1);
    }
    
    /* Fill-in a minimalistic boot info, required for semihosting.  */
    cortexm_binfo.kernel_cmdline = kernel_cmdline;
    cortexm_binfo.kernel_filename = machine->kernel_cmdline;
    
    env->boot_info = &cortexm_binfo;
    
    if (kernel_filename) {
        image_size = load_elf(kernel_filename, NULL, NULL, &entry, &lowaddr,
                              NULL, big_endian, ELF_MACHINE, 1);
        if (image_size < 0) {
            image_size = load_image_targphys(kernel_filename, 0, flash_size);
            lowaddr = 0;
        }
        if (image_size < 0) {
            error_report("Could not load kernel '%s'", kernel_filename);
            exit(1);
        }
    }
    
    /* Hack to map an additional page of ram at the top of the address
     space.  This stops qemu complaining about executing code outside RAM
     when returning from an exception.  */
    memory_region_init_ram(hack, NULL, "armv7m.hack", 0x1000, &error_abort);
    vmstate_register_ram_global(hack);
    memory_region_add_subregion(system_memory, 0xfffff000, hack);
    
    qemu_register_reset(cortexm_reset, cpu);
    
    /* Assume 8000000 Hz */
    /* TODO: compute according to board clock & pll settings */
    system_clock_scale = 80;
    
    return pic;
}


/* Cortex-M0 initialisation routine.  */
qemu_irq *cortex_m0_core_init(cortex_m_core_info *cm_info, MachineState *machine)
{
    machine->cpu_model = "cortex-m0";
    cm_info->has_mpu = false;
    cm_info->has_fpu = false;
    cm_info->fpu_type = CORTEX_M_FPU_TYPE_NONE;
    return cortex_m_core_init(cm_info, machine);
}

/* Cortex-M0+ initialisation routine.  */
qemu_irq *cortex_m0p_core_init(cortex_m_core_info *cm_info, MachineState *machine)
{
    machine->cpu_model = "cortex-m0p";
    cm_info->has_mpu = false;
    cm_info->has_fpu = false;
    cm_info->fpu_type = CORTEX_M_FPU_TYPE_NONE;
    return cortex_m_core_init(cm_info, machine);
}

/* Cortex-M3 initialisation routine.  */
qemu_irq *cortex_m3_core_init(cortex_m_core_info *cm_info, MachineState *machine)
{
    machine->cpu_model = "cortex-m3";
    cm_info->has_fpu = false;
    cm_info->fpu_type = CORTEX_M_FPU_TYPE_NONE;
    return cortex_m_core_init(cm_info, machine);
}

/* Cortex-M4 initialisation routine.  */
qemu_irq *cortex_m4_core_init(cortex_m_core_info *cm_info, MachineState *machine)
{
    machine->cpu_model = "cortex-m4";
    cm_info->fpu_type = CORTEX_M_FPU_TYPE_FPV4_SP_D16;
    return cortex_m_core_init(cm_info, machine);
}

/* Cortex-M7 initialisation routine.  */
qemu_irq *cortex_m7_core_init(cortex_m_core_info *cm_info, MachineState *machine)
{
    machine->cpu_model = "cortex-m7";
    cm_info->fpu_type = CORTEX_M_FPU_TYPE_FPV5_SP_D16;
    return cortex_m_core_init(cm_info, machine);
}

static void cortexm_reset(void *opaque)
{
    ARMCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
}

/* -------------------------------------------------------------------------- */


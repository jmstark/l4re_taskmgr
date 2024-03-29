#
# arch/l4/kernel/arch-x86/Makefile
#

obj-y		:= dispatch.o entry.o ioport.o \
		   ldt.o pci-dma.o pci-nommu.o process.o process_$(BITS).o ptrace.o reboot.o \
		   setup.o signal.o time.o traps.o e820.o \
		   unimpl.o topology.o alternative.o tsc.o tsc_msr.o \
		   tls.o step.o rtc.o io_delay.o memory.o \
		   kdebugfs.o x86_init.o i8259.o

#extra-$(CONFIG_X86_64)	+= head_$(BITS).o
obj-$(CONFIG_X86_64)	+= head_$(BITS).o


obj-y				+= i387.o xsave.o
obj-y				+= dumpstack_$(BITS).o
obj-y				+= irq.o irq_$(BITS).o irq_l4.o irqinit.o
obj-y				+= dumpstack.o nmi.o
obj-y				+= hw_breakpoint.o
obj-y				+= jump_label.o
obj-y				+= pci-iommu_table.o
obj-y				+= resource.o
obj-y				+= head$(BITS).o
obj-y				+= probe_roms.o
obj-y			+= syscall_$(BITS).o
obj-$(CONFIG_X86_64)	+= vsyscall_64.o
obj-$(CONFIG_X86_64)	+= vsyscall_emu_64.o
obj-$(CONFIG_SYSFS)	+= ksysfs.o
obj-$(CONFIG_X86_64)            += sys_x86_64.o #x8664_ksyms_64.o
obj-y				+= quirks.o

obj-$(CONFIG_PREEMPT)	+= preempt.o

obj-$(CONFIG_STACKTRACE)	+= stacktrace.o
obj-y				+= cpu/
obj-$(CONFIG_ACPI)		+= acpi.o
obj-$(CONFIG_MODULES)		+= module.o
obj-$(CONFIG_KGDB)		+= kgdb.o
obj-y				+= kprobes/
obj-$(CONFIG_AMD_NB)		+= amd_nb.o
obj-$(CONFIG_PCI)               += early-quirks.o
obj-y				+= apic/
obj-$(CONFIG_X86_MPPARSE)	+= mpparse.o
obj-$(CONFIG_SMP)		+= smp.o smpboot.o setup_percpu.o
obj-$(CONFIG_HPET_TIMER)	+= hpet.o

obj-$(CONFIG_X86_CHECK_BIOS_CORRUPTION) += check.o

obj-$(CONFIG_SWIOTLB)			+= pci-swiotlb.o
obj-$(CONFIG_OF)			+= devicetree.o
obj-$(CONFIG_UPROBES)			+= uprobes.o
obj-y					+= sysfb.o
obj-$(CONFIG_X86_SYSFB)			+= sysfb_simplefb.o
obj-$(CONFIG_EFI)			+= sysfb_efi.o

obj-$(CONFIG_PERF_EVENTS)		+= perf_regs.o
obj-$(CONFIG_X86_TSC)			+= trace_clock.o

###
# 64 bit specific files
ifeq ($(CONFIG_X86_64),y)
	obj-y			+= vsmp_64.o
endif

# -------------------------------------------
b		:= ../../../x86/kernel
alternative-y	:= $(b)/alternative.o
ldt-y		:= $(b)/ldt.o
module-y	:= $(b)/module.o
pci-nommu-y	:= $(b)/pci-nommu.o
pci-dma-y	:= $(b)/pci-dma.o
topology-y	:= $(b)/topology.o
amd_nb-y	+= $(b)/amd_nb.o
stacktrace-y	+= $(b)/stacktrace.o
tls-y		+= $(b)/tls.o
step-y		+= $(b)/step.o
ds-y		+= $(b)/ds.o
io_delay-y	+= $(b)/io_delay.o
setup_percpu-y  += $(b)/setup_percpu.o
probe_roms-y	+= $(b)/probe_roms.o
early-quirks-y	+= $(b)/early-quirks.o
kdebugfs-y	+= $(b)/kdebugfs.o
dumpstack_$(BITS)-y	+= $(b)/dumpstack_$(BITS).o
dumpstack-y	+= $(b)/dumpstack.o
hw_breakpoint-y += $(b)/hw_breakpoint.o
i8259-y         += $(b)/i8259.o
jump_label-y    += $(b)/jump_label.o
resource-y      += $(b)/resource.o
time-y          += $(b)/time.o
pci-iommu_table-y += $(b)/pci-iommu_table.o
syscall_$(BITS)-y += $(b)/syscall_$(BITS).o
#vsyscall_64-y   += $(b)/vsyscall_64.o
vread_tsc_64-y	+= $(b)/vread_tsc_64.o
hpet-y          += $(b)/hpet.o
quirks-y        += $(b)/quirks.o
sys_x86_64-y	+= $(b)/sys_x86_64.o
pci-swiotlb-y	+= $(b)/pci-swiotlb.o
devicetree-y	+= $(b)/devicetree.o
vsmp_64-y	+= $(b)/vsmp_64.o
x8664_ksyms_64-y += $(b)/x8664_ksyms_64.o
vsyscall_emu_64-y += $(b)/vsyscall_emu_64.o
nmi-y           += $(b)/nmi.o
perf_regs-y     += $(b)/perf_regs.o
trace_clock-y   += $(b)/trace_clock.o
sysfb-y         += $(b)/sysfb.o
preempt-y       += $(b)/preempt.o
ksysfs-y	+= $(b)/ksysfs.o
tsc_msr-y	+= $(b)/tsc_msr.o

# we have our own versions of destroy_context and init_new_context,
# so rename the original one
CFLAGS_ldt.o    += -Ddestroy_context=destroy_context_origarch
CFLAGS_ldt.o    += -Dinit_new_context=init_new_context_origarch

# syscall_table_32.S
KBUILD_AFLAGS	+= -Iarch/x86/kernel
# headers
KBUILD_CFLAGS	+= -Iarch/x86/kernel

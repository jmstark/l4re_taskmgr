#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/module.h>

#include <asm/uaccess.h>

#include <asm/l4lxapi/irq.h>

#include <asm/generic/irq.h>
#include <asm/generic/task.h>
#include <asm/generic/stack_id.h>

union irq_ctx {
	struct thread_info	tinfo;
	u32			stack[THREAD_SIZE/sizeof(u32)];
};

static union irq_ctx *softirq_ctx;

static char softirq_stack[THREAD_SIZE]
		__attribute__((__aligned__(THREAD_SIZE)));

static void l4x_init_softirq_stack(void)
{
	softirq_ctx = (union irq_ctx *)softirq_stack;
	softirq_ctx->tinfo.task			= NULL;
	softirq_ctx->tinfo.exec_domain		= NULL;
	softirq_ctx->tinfo.cpu			= 0;
	softirq_ctx->tinfo.addr_limit		= MAKE_MM_SEG(0);
}

#ifdef CONFIG_HOTPLUG_CPU
void irq_force_complete_move(int irq)
{
	// tbd?
}
#endif

void __init l4x_init_IRQ(void)
{
	int i;

	l4lx_irq_init();
	l4x_init_softirq_stack();

	for (i = 0; i < L4X_IRQS_V_DYN_BASE; i++) {
		l4x_alloc_irq_desc_data(i);
		irq_set_chip_and_handler(i, &l4x_irq_io_chip, handle_edge_eoi_irq);
	}
	for (i = L4X_IRQS_V_DYN_BASE; i < L4X_IRQS_V_STATIC_BASE; i++) {
		l4x_alloc_irq_desc_data(i);
		irq_set_chip_and_handler(i, &l4x_irq_plain_chip, handle_edge_eoi_irq);
	}

	/* from native_init_IRQ() */
#ifdef CONFIG_X86_32
	irq_ctx_init(smp_processor_id());
#endif
}

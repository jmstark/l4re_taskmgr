/*
 * l4 interrupt implementation.
 */
#include <asm/io.h>

#include <linux/sched.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/irq.h>

#include <l4/sys/irq.h>
#include <l4/sys/icu.h>

#include <asm/api/config.h>
#include <asm/api/macros.h>

#include <asm/l4lxapi/irq.h>
#include <asm/l4lxapi/thread.h>
#include <asm/l4lxapi/misc.h>

#include <asm/generic/io.h>
#include <asm/generic/sched.h>
#include <asm/generic/setup.h>
#include <asm/generic/task.h>
#include <asm/generic/do_irq.h>
#include <asm/generic/irq.h>
#include <asm/generic/cap_alloc.h>
#include <asm/generic/stack_id.h>

#include <l4/re/c/namespace.h>
#include <l4/log/log.h>

#define d_printk(format, args...)  LOG_printf(format , ## args)
//#define dd_printk(format, args...) LOG_printf(format , ## args)
#define dd_printk(format, args...) do { } while (0)

static char irq_prio[NR_IRQS] =
   /*  0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f */
     { 1, 0,15, 6, 5, 4, 3, 2,14,13,12,11,10, 9, 8, 7};

static char irq_disable_cmd_state[NR_IRQS]; /* Make this a bitmask?! */

#ifdef CONFIG_SMP
static unsigned     irq_cpus[NR_IRQS];

static inline void set_irq_cpu(unsigned irq, unsigned cpu)
{
	irq_cpus[irq] = cpu;
}
static inline unsigned get_irq_cpu(unsigned irq)
{
	return irq_cpus[irq];
}
#else
static inline void set_irq_cpu(unsigned irq, unsigned cpu) { (void)irq; (void)cpu; }
static inline unsigned get_irq_cpu(unsigned irq) { (void)irq; return 0; }
#endif

/*
 * Return the priority of an interrupt thread.
 */
int l4lx_irq_prio_get(unsigned int irq)
{
	if (irq < NR_IRQS)
		return CONFIG_L4_PRIO_IRQ_BASE + irq_prio[irq];

	enter_kdebug("l4lx_irq_prio_get: wrong IRQ!");
	return -1;
}

static inline int attach_to_irq(struct irq_desc *desc)
{
	long ret;
	struct l4x_irq_desc_private *p = irq_desc_get_chip_data(desc);

	if ((ret  = l4_error(l4_irq_attach(p->c.irq_cap, irq_desc_get_irq_data(desc)->irq << 2,
	                                   l4x_cap_current()))))
		dd_printk("%s: can't attach to irq %u: %ld\n",
		          __func__, desc->irq, ret);

	return !ret;
}

enum irq_cmds {
	CMD_IRQ_ENABLE  = 1,
	CMD_IRQ_DISABLE = 2,
	CMD_IRQ_UPDATE  = 3,
};

static void attach_to_interrupt(unsigned irq)
{
	struct l4x_irq_desc_private *p = irq_get_chip_data(irq);
	if (!p->enabled)
		p->enabled = attach_to_irq(irq_to_desc(irq));

	dd_printk("attaching to irq %d\n", irq);
}

static void detach_from_interrupt(struct irq_desc *desc, int irq)
{
	struct l4x_irq_desc_private *p = irq_desc_get_chip_data(desc);
	if (l4_error(l4_irq_detach(p->c.irq_cap)))
		dd_printk("%02d: Unable to detach from IRQ\n",
		          irq_desc_get_irq_data(desc)->irq);

	dd_printk("detaching from irq %d (cap: %lx)\n",
	          irq_desc_get_irq_data(desc)->irq, p->c.irq_cap);

	p->enabled = 0;
	irq_disable_cmd_state[irq_desc_get_irq_data(desc)->irq] = 0;
}

/*
 * Wait for an interrupt to arrive
 */
static inline void wait_for_irq_message(unsigned irq)
{
	l4_umword_t cmd;
	l4_umword_t src_id;
	int err;
	l4_msgtag_t tag;
	l4_utcb_t *utcb = l4_utcb();
	struct irq_desc *desc = irq_to_desc(irq);
	struct l4x_irq_desc_private *p = irq_desc_get_chip_data(desc);

	while (1) {
		if (unlikely(p->enabled && irq_disable_cmd_state[irq]))
			detach_from_interrupt(desc, irq);

		tag = l4_irq_wait_u(p->c.irq_cap, &src_id, L4_IPC_NEVER, utcb);
		err = l4_ipc_error(tag, utcb);

		if (unlikely(err)) {
			/* IPC error */
			LOG_printf("%s: IRQ %u (" PRINTF_L4TASK_FORM ") "
			           "receive failed, error = 0x%d\n",
			           __func__, irq, PRINTF_L4TASK_ARG(p->c.irq_cap), err);
			enter_kdebug("receive from intr failed");
		} else if (likely(src_id)) {
			/* Interrupt coming! */
			if (likely(p->enabled))
				break;
			d_printk("Invalid message to IRQ thread %d\n", irq);
		} else {
			cmd = l4_msgtag_label(tag);

			/* Non-IRQ message, handle */

			dd_printk("irq-messsage: %d: cmd=%ld\n", irq, cmd);

			if (cmd == CMD_IRQ_ENABLE) {
				if (!p->enabled)
					attach_to_interrupt(irq);
			} else if (cmd == CMD_IRQ_DISABLE) {
				if (p->enabled
				    && irq_disable_cmd_state[irq])
					detach_from_interrupt(desc, irq);
			} else if (cmd == CMD_IRQ_UPDATE) {
				l4x_prepare_irq_thread(current_thread_info(),
				                       get_irq_cpu(irq));
			} else
				LOG_printf("Unknown cmd: %lu (enabled: %d)\n",
				           cmd, p->enabled);
		}
	}
} /* wait_for_irq_message */


/*
 * IRQ thread, here we sit in a loop waiting to handle
 * incoming interrupts
 */
static L4_CV void irq_thread(void *data)
{
	unsigned irq = *(unsigned *)data;
	struct thread_info *ctx = current_thread_info();
	struct l4x_irq_desc_private *p = irq_get_chip_data(irq);
	unsigned state;

	l4x_prepare_irq_thread(current_thread_info(), get_irq_cpu(irq));
	p->enabled = attach_to_irq(irq_to_desc(irq));

	dd_printk("%s: Started IRQ thread for IRQ %d\n", __func__, irq);
	LOG_printf("%s: Started IRQ thread for IRQ %d\n", __func__, irq);

	/*
	 * initialization complete -- now wait for irq messages and handle
	 * them appropriately
	 */

	for (;;) {
		state = 0;
		wait_for_irq_message(irq);
		if (state)
			LOG_printf("nesting with irq %d\n", state);
		state = irq;

		l4x_do_IRQ(irq, ctx);
	}
} /* irq_thread */

/* ############################################################# */

/*
 * Common stuff.
 */

static void send_msg(unsigned int irq, enum irq_cmds cmd)
{
	int error;
	l4_msgtag_t tag = l4_msgtag(cmd, 0, 0, 0);
	struct l4x_irq_desc_private *p = irq_get_chip_data(irq);

	/* XXX: range checking */

	/* Disabling is not asynchronously to avoid dead locks with
	 * the Linux IRQ shutdown code in tamed mode */
	if (cmd == CMD_IRQ_DISABLE) {
		irq_disable_cmd_state[irq] = 1;
		tag = l4_ipc_send(l4lx_thread_get_cap(p->irq_thread), l4_utcb(),
		                  tag, L4_IPC_SEND_TIMEOUT_0);
		error = l4_ipc_error(tag, l4_utcb());
		if (error && error != L4_IPC_SETIMEOUT)
			LOG_printf("%s: dis-IPC failed with %x\n", __func__, error);
	} else {
		tag = l4_ipc_send(l4lx_thread_get_cap(p->irq_thread), l4_utcb(),
		                  tag, L4_IPC_NEVER);
		error = l4_ipc_error(tag, l4_utcb());

		if (error)
			d_printk("%s: IPC failed with %x\n", __func__, error);
	}
}

void l4lx_irq_init(void)
{
}

#ifdef CONFIG_SMP
static void migrate_irq(unsigned irq, unsigned to_cpu)
{
	struct l4x_irq_desc_private *p = irq_get_chip_data(irq);
	send_msg(irq, CMD_IRQ_DISABLE);

	l4x_migrate_thread(l4lx_thread_get_cap(p->irq_thread),
	                   get_irq_cpu(irq), to_cpu);
	set_irq_cpu(irq, to_cpu);
	send_msg(irq, CMD_IRQ_UPDATE);

	send_msg(irq, CMD_IRQ_ENABLE);
}
#endif

/*******************************************
 * Hardware (device) interrupts.
 */

static void create_irq_thread(unsigned irq, struct l4x_irq_desc_private *p)
{
	char thread_name[7];

	// we're done if the IRQ thread already exists
	if (l4lx_thread_is_valid(p->irq_thread)) {
		dd_printk("%s: irqthread %d: already there\n", __func__, irq);
		return;
	}

	p->c.irq_cap = l4x_have_irqcap(irq, 0);
	BUG_ON(l4_is_invalid_cap(p->c.irq_cap));

	pr_info("%s: creating IRQ thread for %d (IRQ-cap %lx)\n",
	        __func__, irq, p->c.irq_cap);

	set_irq_cpu(irq, smp_processor_id());

	sprintf(thread_name, "IRQ%d", irq);
	p->irq_thread = l4lx_thread_create(irq_thread,
	                                   smp_processor_id(),
	                                   NULL, &irq, sizeof(irq),
	                                   l4x_cap_alloc(),
	                                   l4lx_irq_prio_get(irq),
	                                   0, 0, thread_name, NULL);

	if (!l4lx_thread_is_valid(p->irq_thread))
		enter_kdebug("Error creating IRQ-thread!");
}

/*
 * There are two possible ways this funtion is called:
 * - from request_irq(...) and friends with properly set up
 *   environment (defined action)
 * - from probe_irq_on where no action is defined
 *   it's assumed that the code that calls probe_irq_on also calls
 *   probe_irq_off some time later (e.g. in the same routine)
 *
 * Autodetection from probe_irq_on needs special treatment since it
 * sets no irq handler for this irq and when the interrupt source delivers
 * many interrupts so that the irq thread (which has a higher priority than
 * other threads) gets all these interrupts no other thread in scheduled.
 * The system then hangs.... Anyone with a better solution?
 *
 * The implemented strategy hopefully prevents spinning IRQ threads when
 * devices cause IRQ storms in probes.
 *
 * Seems to be called with irq_desc[irq].lock held.
 */
unsigned int l4lx_irq_io_startup(struct irq_data *data)
{
	unsigned irq = data->irq;
	struct l4x_irq_desc_private *p = irq_get_chip_data(irq);

	if (!p)
		return 0;

	BUG_ON(p->is_percpu);
	BUG_ON(!l4_is_invalid_cap(p->c.irq_cap));

	if (l4_is_invalid_cap(p->c.irq_cap = l4x_cap_alloc())) {
		pr_err("l4x-irq: failed to get IRQ cap\n");
		return 0;
	}

	if (l4_error(l4_icu_set_mode(l4io_request_icu(),
	                             irq, p->trigger)) < 0) {
		pr_err("l4x-irq: Failed to set type for IRQ %d\n", irq);
		WARN_ON(1);
		goto err;
	}

	if (l4io_request_irq(irq, p->c.irq_cap)) {
			pr_err("l4x-irq: Did not get IRQ %d from IO service\n", irq);
			goto err;
	}

	create_irq_thread(irq, p);

	if (!p->enabled)
		l4lx_irq_dev_enable(data);

	return 0;
err:
	l4x_cap_free(p->c.irq_cap);
	p->c.irq_cap = L4_INVALID_CAP;
	return 0;
}

unsigned int l4lx_irq_plain_startup(struct irq_data *data)
{
	struct l4x_irq_desc_private *p = irq_get_chip_data(data->irq);
	create_irq_thread(data->irq, p);
	l4lx_irq_dev_enable(data);
	return 0;
}

#ifdef CONFIG_SMP
int l4lx_irq_dev_set_affinity(struct irq_data *data,
                              const struct cpumask *dest, bool force)
{
	cpumask_t tmp;
	unsigned target_cpu;

	cpus_and(tmp, *cpu_online_mask, *dest);
	if (cpus_empty(tmp))
		cpu_set(0, tmp); // then take cpu0

	target_cpu = first_cpu(tmp);

	migrate_irq(data->irq, target_cpu);
	return 0;
}
#endif

void l4lx_irq_io_shutdown(struct irq_data *data)
{
	struct l4x_irq_desc_private *p = irq_get_chip_data(data->irq);

	dd_printk("%s: %u\n", __func__, data->irq);
	l4lx_irq_dev_disable(data);

	l4io_release_irq(data->irq, p->c.irq_cap);
	l4x_cap_free(p->c.irq_cap);
	p->c.irq_cap = L4_INVALID_CAP;
}

void l4lx_irq_plain_shutdown(struct irq_data *data)
{
}

void l4lx_irq_dev_enable(struct irq_data *data)
{
	dd_printk("%s: %u\n", __func__, data->irq);

	create_irq_thread(data->irq, irq_get_chip_data(data->irq));
	/* Only switch if IRQ thread already exists, not if we're running
	 * in the Linux server. */
	//if (test_bit(data->irq, irq_threads_started))
		send_msg(data->irq, CMD_IRQ_ENABLE);
}

void l4lx_irq_dev_disable(struct irq_data *data)
{
	dd_printk("%s: %u\n", __func__, data->irq);
	//if (test_bit(data->irq, irq_threads_started))
		send_msg(data->irq, CMD_IRQ_DISABLE);
}

void l4lx_irq_dev_ack(struct irq_data *data)
{
}

void l4lx_irq_dev_mask(struct irq_data *data)
{
}

void l4lx_irq_dev_mask_ack(struct irq_data *data)
{
}

void l4lx_irq_dev_unmask(struct irq_data *data)
{
}

void l4lx_irq_dev_eoi(struct irq_data *data)
{
	dd_printk("%s: %u\n", __func__, data->irq);
	//l4_irq_unmask(p->c.irq_cap);
}

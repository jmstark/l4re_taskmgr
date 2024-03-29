
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/param.h>
#include <linux/smp.h>
#include <linux/timex.h>
#include <linux/clockchips.h>
#include <linux/irq.h>
#include <linux/cpu.h>
#include <linux/sched_clock.h>

#include <asm/l4lxapi/thread.h>

#include <asm/generic/cap_alloc.h>
#include <asm/generic/setup.h>
#include <asm/generic/smp.h>
#include <asm/generic/vcpu.h>
#include <asm/generic/timer.h>

#include <l4/sys/factory.h>
#include <l4/sys/irq.h>
#include <l4/sys/task.h>
#include <l4/log/log.h>
#include <l4/re/env.h>

enum { PRIO_TIMER = CONFIG_L4_PRIO_IRQ_BASE + 1 };

enum Protos
{
	L4_PROTO_TIMER = 19L,
};

enum L4_timer_ops
{
	L4_TIMER_OP_START = 0UL,
	L4_TIMER_OP_STOP  = 1UL,
};

typedef unsigned long long l4timer_time_t;
static int timer_irq;

static struct clock_event_device __percpu *timer_evt;
static l4_cap_idx_t __percpu *timer_irq_caps;
static DEFINE_PER_CPU(l4_cap_idx_t, timer_srv);
static DEFINE_PER_CPU(l4lx_thread_t, timer_threads);

static void L4_CV timer_thread(void *data)
{
	l4_timeout_t to;
	l4_utcb_t *u = l4_utcb();
	l4_cap_idx_t irq_cap = *(l4_cap_idx_t *)data;
	l4_msgtag_t t;
	l4_umword_t l;
	l4_msg_regs_t *v = l4_utcb_mr_u(u);
	l4timer_time_t increment = 0;
	l4_cpu_time_t next_to = 0;
	enum {
		idx_base_mr   = 2,
	};
	const unsigned idx_at
		= l4_utcb_mr64_idx(idx_base_mr);
	const unsigned idx_increment
		= l4_utcb_mr64_idx(idx_base_mr + sizeof(v->mr64[0]) / sizeof(v->mr[0]));

	to = L4_IPC_NEVER;
	t = l4_ipc_wait(u, &l, to);
	while (1) {
		int reply = 1;
		int r = 0;

		if (l4_ipc_error(t, u) == L4_IPC_RETIMEOUT) {
			if (l4_ipc_error(l4_irq_trigger_u(irq_cap, u), u))
				LOG_printf("IRQ timer trigger failed\n");

			if (increment) {
				next_to += increment;
				to = l4_timeout(L4_IPC_TIMEOUT_0,
				                l4_timeout_abs_u(next_to, 1, u));
			} else
				to = L4_IPC_NEVER;
			reply = 0;
		} else if (l4_error(t) == L4_PROTO_TIMER) {
			switch (v->mr[0]) {
				case L4_TIMER_OP_START:
					next_to = v->mr64[idx_at];
					to = l4_timeout(L4_IPC_TIMEOUT_0,
					                l4_timeout_abs_u(next_to, 1, u));
					increment = v->mr64[idx_increment];
					r = 0;
					break;
				case L4_TIMER_OP_STOP:
					to = L4_IPC_NEVER;
					increment = 0;
					r = 0;
					break;
				default:
					LOG_printf("l4timer: invalid opcode\n");
					r = -ENOSYS;
					break;
			};
		} else
			LOG_printf("l4timer: msg r=%ld\n", l4_error(t));

		t = l4_msgtag(r, 0, 0, 0);
		if (reply)
			t = l4_ipc_reply_and_wait(u, t, &l, to);
		else
			t = l4_ipc_wait(u, &l, to);
	}


}

static l4_msgtag_t l4timer_start_u(l4_cap_idx_t timer, unsigned flags,
                                   l4timer_time_t at, l4timer_time_t increment,
                                   l4_utcb_t *utcb)
{
	int idx = 2;
	l4_msg_regs_t *v = l4_utcb_mr_u(utcb);
	v->mr[0] = L4_TIMER_OP_START;
	v->mr[1] = flags;
	v->mr64[l4_utcb_mr64_idx(idx)] = at;
	idx += sizeof(v->mr64[0]) / sizeof(v->mr[0]);
	v->mr64[l4_utcb_mr64_idx(idx)] = increment;
	idx += sizeof(v->mr64[0]) / sizeof(v->mr[0]);
	return l4_ipc_call(timer, utcb,
	                   l4_msgtag(L4_PROTO_TIMER, idx, 0, 0),
	                   L4_IPC_NEVER);
}

static l4_msgtag_t l4timer_stop_u(l4_cap_idx_t timer, l4_utcb_t *utcb)
{
	l4_msg_regs_t *v = l4_utcb_mr_u(utcb);
	v->mr[0] = L4_TIMER_OP_STOP;
	return l4_ipc_call(timer, utcb, l4_msgtag(L4_PROTO_TIMER, 1, 0, 0),
                           L4_IPC_NEVER);
}

static l4_msgtag_t l4timer_start(l4_cap_idx_t timer, unsigned flags,
                                 l4timer_time_t at, l4timer_time_t increment)
{
	return l4timer_start_u(timer, flags, at, increment, l4_utcb());
}

static l4_msgtag_t l4timer_stop(l4_cap_idx_t timer)
{
	return l4timer_stop_u(timer, l4_utcb());
}




static void timer_set_mode(enum clock_event_mode mode,
                           struct clock_event_device *clk)
{
	const l4timer_time_t increment = 1000000 / HZ;
	int r;

	switch (mode) {
	case CLOCK_EVT_MODE_SHUTDOWN:
	case CLOCK_EVT_MODE_ONESHOT:
		r = L4XV_FN_i(l4_error(l4timer_stop(this_cpu_read(timer_srv))));
		if (r)
			pr_warn("l4timer: stop failed (%d)\n", r);
		while (L4XV_FN_i(l4_ipc_error(l4_ipc_receive(*this_cpu_ptr(timer_irq_caps), l4_utcb(), L4_IPC_BOTH_TIMEOUT_0), l4_utcb())) != L4_IPC_RETIMEOUT)
			;
		break;
	case CLOCK_EVT_MODE_PERIODIC:
	case CLOCK_EVT_MODE_RESUME:
		r = L4XV_FN_i(l4_error(l4timer_start(this_cpu_read(timer_srv), 0,
		                                     l4_kip_clock(l4lx_kinfo),
		                                     increment)));
		if (r)
			pr_warn("l4timer: start failed (%d)\n", r);
		break;
	case CLOCK_EVT_MODE_UNUSED:
		break;
	default:
		pr_warn("l4timer_set_mode: Unknown mode %d\n", mode);
		break;
	}
}

static int timer_set_next_event(unsigned long evt,
                                struct clock_event_device *unused)
{
	int r;
	r = L4XV_FN_i(l4_error(l4timer_start(this_cpu_read(timer_srv), 0,
	                                     l4_kip_clock(l4lx_kinfo) + evt,
	                                     0)));
	if (r)
		pr_warn("l4timer: start failed (%d)\n", r);
	return 0;
}

static int timer_clock_event_init(struct clock_event_device *clk)
{
	int r;
	l4_cap_idx_t irq_cap;
	unsigned cpu = smp_processor_id();
	l4lx_thread_t thread;
	char s[12];
	L4XV_V(f);

	irq_cap = l4x_cap_alloc();
	if (l4_is_invalid_cap(irq_cap))
		return -ENOMEM;

	r = L4XV_FN_i(l4_error(l4_factory_create_irq(l4re_env()->factory,
	                                             irq_cap)));
	if (r)
		goto out1;

	*this_cpu_ptr(timer_irq_caps) = irq_cap;

	snprintf(s, sizeof(s), "timer%d", cpu);
	s[sizeof(s) - 1] = 0;

	L4XV_L(f);
	thread = l4lx_thread_create
                  (timer_thread,                /* thread function */
                   cpu,                         /* cpu */
                   NULL,                        /* stack */
                   &irq_cap, sizeof(irq_cap),   /* data */
                   l4x_cap_alloc(),             /* cap */
                   PRIO_TIMER,                  /* prio */
		   0,                           /* utcbp */
                   0,                           /* vcpup */
                   s,                           /* name */
		   NULL);
	L4XV_U(f);

	if (!l4lx_thread_is_valid(thread)) {
		r = -ENOMEM;
		goto out2;
	}

	this_cpu_write(timer_threads, thread);
	this_cpu_write(timer_srv, l4lx_thread_get_cap(thread));

	clk->features       = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC
		              | CLOCK_EVT_FEAT_C3STOP;
	clk->name           = "l4-timer";
	clk->rating         = 300;
	clk->cpumask        = cpumask_of(cpu);
	clk->irq            = timer_irq;
	clk->set_mode       = timer_set_mode;
	clk->set_next_event = timer_set_next_event;

	enable_percpu_irq(timer_irq, IRQ_TYPE_NONE);

	clk->set_mode(CLOCK_EVT_MODE_SHUTDOWN, clk);
	clockevents_config_and_register(clk, 1000000, 1, ~0UL);

	return 0;

out2:
	L4XV_FN_v(l4_task_release_cap(L4RE_THIS_TASK_CAP, irq_cap));
out1:
	l4x_cap_free(irq_cap);

	return r;
}

static void timer_clock_stop(struct clock_event_device *clk)
{
	l4lx_thread_t t = this_cpu_read(timer_threads);

	disable_percpu_irq(clk->irq);
	clk->set_mode(CLOCK_EVT_MODE_UNUSED, clk);

	L4XV_FN_v(l4lx_thread_shutdown(t, 1, NULL, 1));
	this_cpu_write(timer_threads, NULL);

	L4XV_FN_v(l4_task_release_cap(L4RE_THIS_TASK_CAP,
	                              *this_cpu_ptr(timer_irq_caps)));
	l4x_cap_free(*this_cpu_ptr(timer_irq_caps));
	*this_cpu_ptr(timer_irq_caps) = L4_INVALID_CAP;
}

static irqreturn_t timer_interrupt_handler(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static int timer_cpu_notify(struct notifier_block *self,
                            unsigned long action, void *hcpu)
{
	switch (action & ~CPU_TASKS_FROZEN) {
		case CPU_STARTING:
			timer_clock_event_init(this_cpu_ptr(timer_evt));
			break;
		case CPU_DYING:
			timer_clock_stop(this_cpu_ptr(timer_evt));
			break;
	}

	return NOTIFY_OK;
}

static struct notifier_block arch_timer_cpu_nb = {
	.notifier_call = timer_cpu_notify,
};


static int __init l4x_timer_init_ret(void)
{
	int r;

	if ((timer_irq = l4x_alloc_percpu_irq(&timer_irq_caps)) < 0)
		return -ENOMEM;

	pr_info("l4timer: Using IRQ%d\n", timer_irq);

	timer_evt = alloc_percpu(struct clock_event_device);
	if (!timer_evt) {
		r = -ENOMEM;
		goto out1;
	}

	r = request_percpu_irq(timer_irq, timer_interrupt_handler,
	                       "L4-timer", timer_evt);
	if (r < 0)
		goto out2;

	r = timer_clock_event_init(this_cpu_ptr(timer_evt));

	r = register_cpu_notifier(&arch_timer_cpu_nb);
	if (r)
		goto out3;

	return 0;


out3:
	free_percpu_irq(timer_irq, timer_evt);
out2:
	free_percpu(timer_evt);
out1:
	l4x_free_percpu_irq(timer_irq);
	return r;
}

#ifdef CONFIG_GENERIC_SCHED_CLOCK
static u32 notrace kip_clock_read_32(void)
{
	return l4_kip_clock_lw(l4re_kip());
}
#endif

static cycle_t l4x_clk_read(struct clocksource *cs)
{
	return l4_kip_clock(l4re_kip());
}

static struct clocksource kipclk_cs = {
	.name           = "l4kipclk",
	.rating         = 300,
	.read           = l4x_clk_read,
	.mask           = CLOCKSOURCE_MASK(64),
	.mult           = 1000,
	.flags          = CLOCK_SOURCE_IS_CONTINUOUS,
};

void __init l4x_timer_init(void)
{
	if (clocksource_register_hz(&kipclk_cs, 1000000))
		pr_err("l4timer: Failed to register clocksource\n");

	if (l4x_timer_init_ret())
		pr_err("l4timer: Failed to initialize!\n");

#ifdef CONFIG_GENERIC_SCHED_CLOCK
	setup_sched_clock(kip_clock_read_32, 32, 1000000);
#endif
}

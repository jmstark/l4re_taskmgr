
#ifndef __INCLUDED_FROM_L4LINUX_DISPATCH
#error Do NOT compile this file directly.
#endif

#ifdef CONFIG_HUGETLB_PAGE
#include <linux/hugetlb.h>
#endif

#include <l4/sys/cache.h>
#include <l4/sys/thread.h>
#include <l4/sys/task.h>
#include <l4/sys/debugger.h>
#include <l4/sys/factory.h>
#include <l4/re/consts.h>
#include <l4/re/env.h>
#ifdef CONFIG_L4_VCPU
#include <l4/vcpu/vcpu.h>
#endif

#include <asm/generic/cap_alloc.h>
#include <asm/generic/fpu.h>
#include <asm/generic/user.h>
#include <asm/generic/vcpu.h>
#include <asm/generic/stats.h>
#include <asm/generic/log.h>
#include <asm/server/server.h>
#include <asm/l4x/utcb.h>

#ifndef CONFIG_L4_VCPU
DEFINE_PER_CPU(int, l4x_idle_running);
#endif

static inline l4_umword_t l4x_parse_ptabs(struct task_struct *p,
                                          l4_umword_t address,
                                          l4_umword_t *pferror,
					  l4_fpage_t *fp, unsigned long *attr)
{
	unsigned fpage_size = L4_LOG2_PAGESIZE;
	spinlock_t *ptl;
	l4_umword_t phy = (l4_umword_t)(-EFAULT);
	pte_t *ptep;
	unsigned long flags;
	unsigned retry_count = 0;

	local_save_flags(flags);

retry:
	ptep = lookup_pte_lock(p->mm, address, &ptl);

#ifdef CONFIG_HUGETLB_PAGE
	struct vm_area_struct *vma = find_vma(p->mm, address);
	if (vma && is_vm_hugetlb_page(vma))
		fpage_size = L4_LOG2_SUPERPAGESIZE;
#endif

	if (!ptep || !pte_present(*ptep))
		goto not_present;

	if (!spin_trylock(ptl)) {
		local_irq_enable();
		if (retry_count < 10) {
			retry_count++;
			cpu_relax();
		} else
			schedule_timeout_interruptible(1);
		local_irq_restore(flags);
		goto retry;
	}

	if (unlikely(!pte_present(*ptep)))
		goto not_present_locked;

	if (!(address & 2)) {
		/* read access */
		l4x_pte_add_access_flag(ptep);
		phy = pte_val(*ptep) & PAGE_MASK;

		/* handle zero page specially */
		if (phy == 0)
			phy = PAGE0_PAGE_ADDRESS;

		*fp   = l4_fpage(phy, fpage_size,
		                 pte_exec(*ptep)
		                  ? L4_FPAGE_RX : L4_FPAGE_RO);
		*attr = l4x_map_page_attr_to_l4(*ptep);
	} else {
		/* write access */
		if (pte_write(*ptep)) {
			/* page present and writable */
			l4x_pte_add_access_and_dirty_flags(ptep);
			phy = pte_val(*ptep) & PAGE_MASK;

			/* handle the zero page specially */
			if (phy == 0)
				phy = PAGE0_PAGE_ADDRESS;

			*fp = l4_fpage(phy, fpage_size,
			               pte_exec(*ptep)
			                ? L4_FPAGE_RWX: L4_FPAGE_RW);
			*attr = l4x_map_page_attr_to_l4(*ptep);
		} else {
			/* page present, but not writable
			 * --> return error */
			*pferror = PF_EUSER + PF_EWRITE +
			           PF_EPROTECTION;
			fp->raw = 0;
		}
	}

	spin_unlock(ptl);
	return phy;

not_present_locked:
	spin_unlock(ptl);
not_present:
	/* page and/or pgdir not present --> return error */
	if ((address & 2))
		*pferror = PF_EUSER + PF_EWRITE +
			   PF_ENOTPRESENT;
	else
		*pferror = PF_EUSER + PF_EREAD +
		           PF_ENOTPRESENT;
	fp->raw = 0;

	return phy;
}

static inline void verbose_segfault(struct task_struct *p,
                                    struct pt_regs *regs,
                                    l4_umword_t pfa, l4_umword_t ip,
                                    l4_umword_t pferror)
{
#ifdef CONFIG_L4_DEBUG_SEGFAULTS
	if (l4x_dbg_stop_on_segv_pf) {
		l4x_printf("cpu%d: segfault for %s(%d) [T:%lx] "
		           "at %08lx, ip=%08lx, pferror = %lx\n",
		           raw_smp_processor_id(), p->comm, p->pid,
		           l4_debugger_global_id(p->mm->context.task),
		           pfa, ip, pferror);
		l4x_print_vm_area_maps(p, ip);
		l4x_print_regs(thread_val_err(&p->thread),
		               thread_val_trapno(&p->thread), regs);
		enter_kdebug("segfault");
		l4x_dbg_stop_on_segv_pf--;
	}
#endif
}

static inline int l4x_handle_page_fault(struct task_struct *p,
                                        struct pt_regs *regs,
                                        l4_umword_t pfa, l4_umword_t err,
                                        l4_umword_t ip)
{
	l4x_debug_stats_pagefault_hit();

	if (likely(pfa < TASK_SIZE)) {
		/* Normal page fault with a process' virtual address space */
		l4_umword_t lxaddr;
		unsigned long attr = 0;
		l4_fpage_t fp;
		l4_umword_t pferror = 0;

		lxaddr = l4x_parse_ptabs(p, pfa, &pferror, &fp, &attr);
		// optimize here: if we found the page, reply immediately
		// without looking for work, it's just a tlb miss
		if (lxaddr == (l4_umword_t)(-EFAULT)) {
			int ret;
			unsigned long flags;

			local_save_flags(flags);
			ret = l4x_do_page_fault(pfa, regs, pferror);
			local_irq_restore(flags);
			if (ret) {
				verbose_segfault(p, regs, pfa, ip, pferror);
				return 1;
			}

		} else {
			l4x_debug_stats_pagefault_but_in_PTs_hit();
			if (pfa & 2)
				l4x_debug_stats_pagefault_write_hit();
		}

#ifdef CONFIG_L4_FERRET_USER
	} else if (l4x_ferret_handle_pf(pfa, d0, d1)) {
		/* Handled */
#endif
	/* page fault in upage ? */
	} else if ((pfa & PAGE_MASK) == UPAGE_USER_ADDRESS && !(pfa & 2)) {
		/* */
	} else {
		printk("WARN: %s: Page-fault above task size: pfa=%lx pc=%lx\n",
		        p->comm, pfa, ip);
#ifdef CONFIG_L4_DEBUG_SEGFAULTS
		l4x_print_vm_area_maps(p, ip);
#endif
		l4x_print_regs(thread_val_err(&p->thread),
		               thread_val_trapno(&p->thread), regs);
		return 1; /* Failed */
	}

	return 0; /* Success */
}

static void
l4x_pf_pfa_to_fp(struct task_struct *p, unsigned long pfa,
		 l4_umword_t *d0, l4_umword_t *d1)
{
	l4_fpage_t fp;
	l4_umword_t pferror = 0;

	*d0 = pfa;

#ifdef CONFIG_L4_VCPU
	BUG_ON(!irqs_disabled());
#endif

	if (likely(pfa < TASK_SIZE)) {
		l4_umword_t phy;
		unsigned long attr = 0;

		phy = l4x_parse_ptabs(p, pfa, &pferror, &fp, &attr);
		if (phy == (l4_umword_t)(-EFAULT)) {
			*d0 = 0;
			return;
		}

		l4_cache_coherent
		    (l4_fpage_page(fp) << L4_LOG2_PAGESIZE,
		     ((l4_fpage_page(fp) + 1) << l4_fpage_size(fp)));

		*d0 = (*d0 & L4_PAGEMASK) | L4_ITEM_MAP | attr;
		*d1  = fp.fpage;
#ifdef CONFIG_L4_FERRET_USER
	} else if (l4x_ferret_handle_pf(pfa, d0, d1)) {
		/* Handled */
#endif
	/* page fault in upage ? */
	} else if ((pfa & PAGE_MASK) == UPAGE_USER_ADDRESS && !(pfa & 2)) {
		*d1 = l4_fpage(upage_addr, L4_LOG2_PAGESIZE,
		               L4_FPAGE_RX).fpage;
		*d0 = (*d0 & PAGE_MASK) | L4_ITEM_MAP;
	} else {
		*d0 = 0;
	}

#ifdef CONFIG_L4_VCPU
	BUG_ON(!irqs_disabled());
#endif
}

#ifndef CONFIG_L4_VCPU
static int l4x_hybrid_return(struct thread_info *ti,
                             l4_utcb_t *utcb,
                             l4_msgtag_t tag)
{
	struct task_struct *h = ti->task;
	struct thread_struct *t = &h->thread;

	if (!t->hybrid_sc_in_prog)
                return 0;

	if (l4_msgtag_is_page_fault(tag)) {
		l4x_printf("HYBRID PF!!\n");
		/* No exception IPC, it's a page fault, but shouldn't happen */
		goto out_fail;
	}

	if (!l4x_hybrid_check_after_syscall(utcb))
		goto out_fail;

	t->hybrid_sc_in_prog = 0;

	/* Keep registers */
	utcb_to_thread_struct(utcb, h, t);

	TBUF_LOG_HYB_RETURN(fiasco_tbuf_log_3val("hyb-ret", TBUF_TID(t->user_thread_id), l4_utcb_exc_pc(l4_utcb_exc_u(utcb)), 0));

	/* Wake up hybrid task h and reschedule */
	wake_up_process(h);

	return 1;

out_fail:
	LOG_printf("%s: Invalid hybrid return for %p ("
	           "%p, %lx, err=%lx, sc=%d, pc=%lx, sp=%lx, tag=%lx)!\n",
	           __func__, ti,
	           h, l4_utcb_exc_typeval(l4_utcb_exc_u(utcb)),
	           l4_utcb_exc_u(utcb)->err,
	           l4x_l4syscall_get_nr(l4_utcb_exc_u(utcb)->err,
			                l4_utcb_exc_pc(l4_utcb_exc_u(utcb))),
	           l4_utcb_exc_pc(l4_utcb_exc_u(utcb)),
	           l4_utcb_exc_u(utcb)->sp, tag.raw);
	LOG_printf("%s: Currently running user thread: " PRINTF_L4TASK_FORM
	           "  service: " PRINTF_L4TASK_FORM " pid=%d\n",
	           __func__, PRINTF_L4TASK_ARG(current->thread.user_thread_id),
	           PRINTF_L4TASK_ARG(l4x_cap_current()), current->pid);
	enter_kdebug("hybrid_return failed");
	return 0;
}

struct l4x_hybrid_object
{
	struct l4x_srv_object o;
	struct thread_info *ti;
};

static L4_CV long
l4x_hybrid_dispatch(struct l4x_srv_object *_this,
                    l4_umword_t obj, l4_utcb_t *msg,
                    l4_msgtag_t *tag)
{
	struct l4x_hybrid_object *ho = (struct l4x_hybrid_object *)_this;
	return l4x_hybrid_return(ho->ti, msg, *tag);
}

/*
 * First phase of a L4 system call by the user program
 */
static int l4x_hybrid_begin(struct task_struct *p,
                            struct thread_struct *t)
{
	int ret;
	l4_msgtag_t tag;
	int intnr = l4x_l4syscall_get_nr(t->error_code, regs_pc(p));

	if (intnr == -1
	    //|| !l4x_syscall_guard(p, intnr)
	    || t->hybrid_sc_in_prog)
		return 0;

	TBUF_LOG_HYB_BEGIN(fiasco_tbuf_log_3val("hyb-beg", TBUF_TID(t->user_thread_id), regs_pc(p), intnr));

	t->hybrid_sc_in_prog = 1;

	if (!t->is_hybrid) {
		l4_cap_idx_t hybgate;
		struct l4x_hybrid_object *ho;

#ifdef CONFIG_L4_DEBUG_REGISTER_NAMES
		char s[20] = "*";

		strncpy(s + 1, p->comm, sizeof(s) - 1);
		s[sizeof(s) - 1] = 0;

		l4_debugger_set_object_name(t->user_thread_id, s);
#endif

		t->is_hybrid = 1;

		hybgate = L4LX_KERN_CAP_HYBRID_BASE + (p->pid << L4_CAP_SHIFT);

		ho = kmalloc(sizeof(*ho), GFP_KERNEL);
		if (!ho)
			return 0;

		ho->o.dispatch = l4x_hybrid_dispatch;
		ho->ti = current_thread_info();

		tag = l4_factory_create_gate(l4re_env()->factory, hybgate,
		                             l4x_cap_current(),
		                             (l4_umword_t)ho);
		if (unlikely(l4_error(tag)))
			LOG_printf("Error creating hybrid gate\n");

		// pager gate, XXX: one cpu only currently
		tag = l4_task_map(p->mm->context.task,
				  L4RE_THIS_TASK_CAP,
				  l4_obj_fpage(hybgate, 0, L4_FPAGE_RO),
				  l4_map_obj_control(L4LX_USER_CAP_PAGER, L4_MAP_ITEM_MAP));
		if (l4_error(tag))
			LOG_printf("Hybrid: Error mapping pager gate%d to task\n", 0);

		// task
		tag = l4_task_map(p->mm->context.task,
				  L4RE_THIS_TASK_CAP,
				  l4_obj_fpage(p->mm->context.task, 0, L4_FPAGE_RO),
				  l4_map_obj_control(L4LX_USER_CAP_TASK, L4_MAP_ITEM_MAP));
		if (l4_error(tag))
			LOG_printf("Hybrid: Error mapping task to task\n");

		// factory
		tag = l4_task_map(p->mm->context.task,
				  L4RE_THIS_TASK_CAP,
				  l4_obj_fpage(L4_BASE_FACTORY_CAP, 0, L4_FPAGE_RW),
				  l4_map_obj_control(L4LX_USER_CAP_FACTORY, L4_MAP_ITEM_MAP));
		if (l4_error(tag))
			LOG_printf("Hybrid: Error mapping factory to task\n");

		// thread
		tag = l4_task_map(p->mm->context.task,
				  L4RE_THIS_TASK_CAP,
				  l4_obj_fpage(t->user_thread_id, 0, L4_FPAGE_RW),
				  l4_map_obj_control(L4LX_USER_CAP_THREAD, L4_MAP_ITEM_MAP));
		if (l4_error(tag))
			LOG_printf("Hybrid: Error mapping factory to task\n");

		// log
		tag = l4_task_map(p->mm->context.task,
				  L4RE_THIS_TASK_CAP,
				  l4_obj_fpage(L4_BASE_LOG_CAP, 0, L4_FPAGE_RO),
				  l4_map_obj_control(L4LX_USER_CAP_LOG, L4_MAP_ITEM_MAP));
		if (l4_error(tag))
			LOG_printf("Hybrid: Error mapping log to task\n");

		// mem_alloc
		tag = l4_task_map(p->mm->context.task,
				  L4RE_THIS_TASK_CAP,
				  l4_obj_fpage(l4re_env()->mem_alloc, 0, L4_FPAGE_RWX),
				  l4_map_obj_control(L4LX_USER_CAP_MA, L4_MAP_ITEM_MAP));
		if (l4_error(tag))
			LOG_printf("Hybrid: Error mapping mem_alloc to task\n");

		// kip
		tag = l4_task_map(p->mm->context.task,
				  L4RE_THIS_TASK_CAP,
				  l4_fpage((l4_umword_t)l4re_kip() &
					  PAGE_MASK, L4_PAGESHIFT, L4_FPAGE_RX),
				  l4_map_control(L4X_USER_KIP_ADDR, 0, L4_MAP_ITEM_MAP));
		if (l4_error(tag))
			LOG_printf("Hybrid: Error mapping kip to task\n");

		// add to hybrid list, we need to check for signals
		// (of sleeping hybrids)
		l4x_hybrid_add(p);
	}

	/* Let the user go on on the syscall instruction */
	tag = l4_ipc_send(p->thread.user_thread_id, l4_utcb(),
	                  l4_msgtag(L4_PROTO_ALLOW_SYSCALL, 0, 0, 0),
	                  L4_IPC_SEND_TIMEOUT_0);
	ret = l4_ipc_error(tag, l4_utcb());

	if (unlikely(ret))
		LOG_printf("%s: send error %x for %s(%d)\n",
		           __func__, ret, p->comm, p->pid);

	/* Mark current as uninterruptible and schedule away */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();

	if (signal_pending(p))
		l4x_do_signal(task_pt_regs(p), 0);

	return 1;
}

static void l4x_dispatch_suspend(struct task_struct *p,
                                 struct thread_struct *t)
{
	/* We're a suspended user process and want to
	 * sleep (aka schedule) now */

	if (unlikely(!t->initial_state_set
	             || !test_bit(smp_processor_id(), &t->threads_up)))
		return;

	/* Go to sleep */
	schedule();

	/* Handle signals */
	if (signal_pending(p))
		l4x_do_signal(task_pt_regs(p), 0);
}
#endif



#ifdef CONFIG_L4_VCPU

#ifdef CONFIG_HOTPLUG_CPU
void l4x_shutdown_cpu(unsigned cpu)
{
	// kill gate specific to this cpu
	l4x_destroy_ugate(cpu);

	// IPI IRQ
	l4x_cpu_ipi_stop(cpu);

	// suicide
	l4x_global_cli();
	l4lx_thread_shutdown(l4x_cpu_thread_get(cpu), 0, 0, 0);
}
#endif

#else

static l4lx_thread_t idler_thread[NR_CPUS];
static int           idler_up[NR_CPUS];

#ifdef CONFIG_HOTPLUG_CPU
void l4x_shutdown_cpu(unsigned cpu)
{
	struct task_struct *p;

	// invalidate and kill all user threads on cpu
	for_each_process(p) {
		struct thread_struct *t = &p->thread;
		if (test_bit(cpu, &t->threads_up)) {
			l4_cap_idx_t thread_id = t->user_thread_ids[cpu];
			l4lx_task_delete_thread(thread_id);
			l4lx_task_number_free(thread_id);
			t->user_thread_ids[cpu] = L4_INVALID_CAP;
			clear_bit(cpu, &t->threads_up);
		}
	}

	// kill gate specific to this cpu
	l4x_destroy_ugate(cpu);

	// kill idler
	l4lx_thread_shutdown(idler_thread[cpu], 1, NULL, 1);

	// kill ipi thread
	l4x_cpu_ipi_stop(cpu);

#ifdef CONFIG_X86_32
	// the next one will be switching stacks, so make utcb-getter work
	// there too
	asm volatile ("mov %0, %%gs" : : "r" (0x43));
#endif
	// suicide
	l4lx_thread_shutdown(l4x_cpu_thread_get(cpu), 0, NULL, 0);
}
#endif

static int l4x_handle_async_event(l4_umword_t label,
                                  l4_utcb_t *u,
                                  l4_msgtag_t tag)
{
	struct l4x_srv_object *o = (struct l4x_srv_object *)(label & ~3UL);
	return o->dispatch(o, label, u, &tag);
}

void l4x_wakeup_idler(int cpu)
{
	if (!idler_up[cpu])
		return;

	l4_thread_ex_regs(l4lx_thread_get_cap(idler_thread[cpu]), ~0UL, ~0UL,
			  L4_THREAD_EX_REGS_TRIGGER_EXCEPTION
	                   | L4_THREAD_EX_REGS_CANCEL);
	TBUF_LOG_WAKEUP_IDLE(fiasco_tbuf_log_3val("wakeup idle", cpu, 0, 0));
}

static L4_CV void idler_func(void *data)
{
	while (1)
		l4_sleep_forever();
}

void l4x_idle(void)
{
	l4_umword_t label;
	int error;
	l4_umword_t data0, data1;
	l4_msgtag_t tag;
	int cpu = smp_processor_id();
	l4_utcb_t *utcb = l4_utcb();
	l4_cap_idx_t me = l4x_cap_current();
	l4_umword_t u_err;

	if (unlikely(!idler_up[cpu])) {
		char s[9];

		snprintf(s, sizeof(s), "idler%d", cpu);
		s[sizeof(s) - 1] = 0;

		LOG_printf("cpu%d: utcb=%p " PRINTF_L4TASK_FORM "\n",
			   cpu, utcb, PRINTF_L4TASK_ARG(me));

		idler_thread[cpu] = l4lx_thread_create(idler_func, cpu,
		                                       NULL, NULL, 0,
		                                       l4x_cap_alloc(),
		                                       CONFIG_L4_PRIO_IDLER,
		                                       0, 0, s, NULL);
		if (!l4lx_thread_is_valid(idler_thread[cpu])) {
			LOG_printf("Could not create idler thread... exiting\n");
			l4x_exit_l4linux();
		}
		l4lx_thread_pager_change(l4lx_thread_get_cap(idler_thread[cpu]), me);
		idler_up[cpu] = 1;
	}


	per_cpu(l4x_current_proc_run, cpu) = current_thread_info();
	per_cpu(l4x_idle_running, cpu) = 1;
	barrier();

#ifdef CONFIG_HOTPLUG_CPU
	if (cpu_is_offline(cpu))
		l4x_cpu_dead();
#endif

	TBUF_LOG_IDLE(fiasco_tbuf_log_3val("l4x_idle <", cpu, 0, 0));

	local_irq_enable();

wait_again:
#ifdef CONFIG_L4_SERVER
	l4x_srv_setup_recv(utcb);
#endif
	tag = l4_ipc_wait(utcb, &label, L4_IPC_SEND_TIMEOUT_0);
	error = l4_ipc_error(tag, utcb);
	u_err = l4_utcb_exc_typeval(l4_utcb_exc_u(utcb));
	if (l4_msgtag_is_page_fault(tag)) {
		// must be a hybrid long-IPC PF
		// for now it's just a PF from the idle-exc-thread
		data0 = l4_utcb_mr_u(utcb)->mr[0];
		data1 = l4_utcb_mr_u(utcb)->mr[1];
		printk("idle-PF(%d): %lx %lx\n", cpu, data0, data1);
		if ((data0 & 2))
			l4_touch_rw((void *)data0, 1);
		else
			l4_touch_ro((void *)data0, 1);

		tag = l4_ipc_send(L4_SYSF_REPLY, utcb,
		                  l4_msgtag(0, 0, 0, 0),
		                  L4_IPC_SEND_TIMEOUT_0);
		if (l4_ipc_error(tag, utcb))
			printk("PF reply error\n");
		goto wait_again;
	} else
		data0 = data1 = 0;

	per_cpu(l4x_current_proc_run, cpu) = NULL;
	per_cpu(l4x_idle_running, cpu) = 0;
	barrier();

	TBUF_LOG_IDLE(fiasco_tbuf_log_3val("l4x_idle >",
	              TBUF_TID(label) | (cpu << 20), error, data0));

	if (unlikely(error)) {
		if (error != L4_IPC_RECANCELED) {
			LOG_printf("idle%d: IPC error = %x (idle)\n",
			           smp_processor_id(), error);
			enter_kdebug("l4_idle: ipc_wait failed");
		}
		return;
	}

	if (label == 0) {
		/* We have received a wakeup message from another
		 * kernel thread. Reschedule. */
		l4x_hybrid_do_regular_work();

		/* Paranoia */
		if (!l4_msgtag_is_exception(tag)
		    || !l4x_is_triggered_exception(u_err)) {
			LOG_printf("idler%d: error=%d label=%lx"
			           " exc-val = 0x%lx [!=%lx]"
			           "tag = %ld pc = %lx\n",
			           cpu, error, label, u_err,
				   l4_utcb_exc_typeval(l4_utcb_exc_u(utcb)),
				   l4_msgtag_label(tag),
			           l4_utcb_exc_pc(l4_utcb_exc_u(utcb)));
			enter_kdebug("Uhh, no exc?!");
		}
	} else if (unlikely(label == 0x12)) {
		LOG_printf("non hybrid in idle?!\n");
		enter_kdebug("non hybrid in idle?!");
	} else {
		if (unlikely(l4x_handle_async_event(label, utcb, tag)))
			l4x_printf("Async return with error\n");
	}
}
#endif /* vcpu */

static inline int l4x_dispatch_page_fault(struct task_struct *p,
                                          struct thread_struct *t,
                                          struct pt_regs *regsp)
{
	TBUF_LOG_USER_PF(fiasco_tbuf_log_3val("U-PF   ",
	                 TBUF_TID(p->thread.user_thread_id),
	                 l4x_l4pfa(t), regs_pc(p)));

	if (unlikely(l4x_handle_page_fault_with_exception(t, regsp)))
		return 0;

	if (l4x_handle_page_fault(p, regsp, l4x_l4pfa(t), t->error_code,
	                          regs_pc(p))) {
		local_irq_enable();
		if (!signal_pending(p))
			force_sig(SIGSEGV, p);
		l4x_do_signal(regsp, 0);
		return 0;
	}

#ifndef CONFIG_L4_VCPU
	if (need_resched())
		schedule();
#endif

	return 1;
}

static inline
l4_cap_idx_t
l4x_user_task_create(void)
{
	l4_cap_idx_t c;

	if (l4lx_task_get_new_task(L4_INVALID_CAP, &c)
	    || l4_is_invalid_cap(c)) {
		l4x_printf("l4x_thread_create: No task no left for user\n");
		return L4_INVALID_CAP;
	}

	if (l4lx_task_create(c)) {
		l4x_cap_free(c);
		return L4_INVALID_CAP;
	}

	return c;
}

#ifndef CONFIG_L4_VCPU
/*
 * - Suspend thread
 */
void l4x_suspend_user(struct task_struct *p, int cpu)
{
	/* Do not suspend if it is still in the setup phase, also
	 * no need to interrupt as it will not stay out long... */
	if (!test_bit(cpu, &p->thread.threads_up))
		return;

	l4_thread_ex_regs(p->thread.user_thread_id,
	                  ~0UL, ~0UL,
	                  L4_THREAD_EX_REGS_TRIGGER_EXCEPTION);
	TBUF_LOG_SUSP_PUSH(fiasco_tbuf_log_3val("suspend", TBUF_TID(p->thread.user_thread_id), 0, 0));

	l4x_debug_stats_suspend_hit();
}

static inline void
l4x_set_task_name(struct task_struct *p, struct thread_struct *t,
                  int cpu, int start_task)
{
#ifdef CONFIG_L4_DEBUG_REGISTER_NAMES
	char s[15];

#ifdef CONFIG_SMP
	snprintf(s, sizeof(s), "%s:%d", p->comm, cpu);
#else
	snprintf(s, sizeof(s), "%s", p->comm);
#endif
	s[sizeof(s) - 1] = 0;
	l4_debugger_set_object_name(t->user_thread_id, s);

	if (start_task) {
#ifdef CONFIG_SMP
		snprintf(s, sizeof(s), "%s", p->comm);
		s[sizeof(s) - 1] = 0;
#endif
		l4_debugger_set_object_name(p->mm->context.task, s);
	}

#endif
}

static inline void l4x_spawn_cpu_thread(struct task_struct *p,
                                        struct thread_struct *t)
{
	int cpu = smp_processor_id();
	int error;
	l4_msgtag_t tag;
	int start_task = 0;

	if (l4_is_invalid_cap(p->mm->context.task)) {
		l4_cap_idx_t c;

		while (l4_is_invalid_cap(c = l4x_user_task_create())) {
			msleep(1);
			l4x_evict_tasks(p);
		}

		start_task = 1;

		task_lock(p);
		if (likely(l4_is_invalid_cap(p->mm->context.task))) {
			p->mm->context.task = c;
			c = L4_INVALID_CAP;
		}
		task_unlock(p);

		if (unlikely(!l4_is_invalid_cap(c))) {
			l4lx_task_delete_task(c);
			l4lx_task_number_free(c);
		}
	}

	t->user_thread_id = l4x_cap_alloc();
	if (l4_is_invalid_cap(t->user_thread_id)) {
		LOG_printf("%s: Failed to create user thread (no caps)\n", __func__);
		return;
	}

	// map pager cap
	tag = l4_task_map(p->mm->context.task,
	                  L4RE_THIS_TASK_CAP,
	                  l4_obj_fpage(l4x_user_gate[cpu], 0, L4_FPAGE_RW),
	                  l4_map_obj_control(l4x_user_pager_cap(cpu),
	                                     L4_MAP_ITEM_MAP));
	if ((error = l4_error(tag))) {
		LOG_printf("Error %d(%lx) setting up gate%d (%lx) in task\n",
		           error, tag.raw, cpu, l4x_user_gate[cpu]);
		return;
	}

	if (!l4lx_task_create_thread_in_task(t->user_thread_id,
	                                     p->mm->context.task,
	                                     l4x_user_pager_cap(cpu), cpu)) {
		LOG_printf("%s: Failed to create user thread\n", __func__);
		return;
	}

	t->user_thread_ids[cpu] = t->user_thread_id;

	l4x_set_task_name(p, t, cpu, start_task);

	// now wait that thread comes in
	error = l4_ipc_error(l4_ipc_receive(t->user_thread_id, l4_utcb(),
	                                    L4_IPC_SEND_TIMEOUT_0),
	                     l4_utcb());
	if (error)
		LOG_printf("%s: IPC error %x\n", __func__, error);

	set_bit(cpu, &t->threads_up);

	if (start_task) {
		t->started = 1;

		l4x_arch_task_start_setup(p, p->mm->context.task);

		l4x_arch_do_syscall_trace(p);

		TBUF_LOG_START(fiasco_tbuf_log_3val("task start", TBUF_TID(t->user_thread_id), regs_pc(p), regs_sp(p)));

		if (signal_pending(p))
			l4x_do_signal(task_pt_regs(p), 0);

		t->initial_state_set = 1;
		t->is_hybrid = 0; /* cloned thread need to reset this */

	}

	l4x_arch_task_setup(t);
}

static void l4x_user_dispatcher(void)
{
	struct task_struct *p = current;
	struct thread_struct *t = &p->thread;
	l4_utcb_t *utcb;
	unsigned cpu;
	l4_umword_t data0 = 0, data1 = 0;
	int error = 0;
	l4_umword_t label;
	l4_msgtag_t tag;
	int reply_with_fpage;

	/* Start L4 activity */
	utcb = l4_utcb();
	preempt_disable();
	cpu = smp_processor_id();
	preempt_enable();
	l4x_spawn_cpu_thread(p, t);
	reply_with_fpage = 0;
	goto reply_IPC;

	while (1) {
		if (l4x_ispf(t)) {
			reply_with_fpage = l4x_dispatch_page_fault(p, t, task_pt_regs(p));
		} else {
			if (!l4x_dispatch_exception(p, t, thread_val_err(t),
			                            thread_val_trapno(t), task_pt_regs(p)))
				l4x_pre_iret_work(task_pt_regs(p), p, 0, 0, 0);

			reply_with_fpage = 0;
		}

		utcb = l4_utcb();
		preempt_disable();
		cpu  = smp_processor_id();
		preempt_enable();

		if (!test_bit(cpu, &p->thread.threads_up))
			l4x_spawn_cpu_thread(p, t);

		p->thread.user_thread_id
			= p->thread.user_thread_ids[cpu];

reply_IPC:
		thread_struct_to_utcb(p, t, utcb,
		                      L4_UTCB_EXCEPTION_REGS_SIZE);

		per_cpu(l4x_current_proc_run, cpu) = current_thread_info();

		/*
		 * Actually we could use l4_ipc_call here but for our
		 * (asynchronous) hybrid apps we need to do an open wait.
		 */

		local_irq_enable();

		TBUF_LOG_DSP_IPC_IN(fiasco_tbuf_log_3val
		   (reply_with_fpage ? "DSP-r-F" : "DSP-r-M",
		    TBUF_TID(current->thread.user_thread_id), data0, data1));

		/* send the reply message and wait for a new request. */
		if (reply_with_fpage) {
			int i;
			l4_umword_t data0 = 0, data1 = 0;

			l4x_pf_pfa_to_fp(p, l4x_l4pfa(t), &data0, &data1);

			i = per_cpu(utcb_snd_size, cpu);
			l4_utcb_mr_u(utcb)->mr[i]     = data0;
			l4_utcb_mr_u(utcb)->mr[i + 1] = data1;
		}

		tag = l4_msgtag(0, per_cpu(utcb_snd_size, cpu),
		                reply_with_fpage, l4x_msgtag_fpu(cpu) | l4x_msgtag_copy_ureg(utcb));

#ifdef CONFIG_L4_SERVER
		l4x_srv_setup_recv(utcb);
#endif
		if (l4x_msgtag_fpu(cpu))
			l4_utcb_inherit_fpu_u(utcb, 1);

		tag = l4_ipc_send_and_wait(p->thread.user_thread_id, utcb,
		                           tag, &label, L4_IPC_SEND_TIMEOUT_0);
after_IPC:
		per_cpu(l4x_current_proc_run, cpu) = NULL;
		l4_utcb_inherit_fpu_u(utcb, 0);

		error = l4_ipc_error(tag, utcb);
#if defined(CONFIG_SMP) && defined(CONFIG_X86)
		asm("mov %0, %%fs"
		    : : "r" (l4x_fiasco_gdt_entry_offset * 8 + 3) : "memory");
#endif

		// dbg
		if (unlikely(utcb != l4_utcb()))
			enter_kdebug("utcb mismatch");

		TBUF_LOG_DSP_IPC_OUT(fiasco_tbuf_log_3val("DSP-out", label,
		                     (error << 16), TBUF_TID(current->thread.user_thread_id)));
		TBUF_LOG_DSP_IPC_OUT(fiasco_tbuf_log_3val("DSP-val", label, data0, data1));

		if (unlikely(error == L4_IPC_SETIMEOUT)) {
			LOG_printf("dispatch%d: "
			           "IPC error SETIMEOUT (context) (to = "
			           PRINTF_L4TASK_FORM ", src = %lx)\n",
			           cpu,
			           PRINTF_L4TASK_ARG(p->thread.user_thread_id),
			           label);
			enter_kdebug("L4_IPC_SETIMEOUT?!");

only_receive_IPC:
			per_cpu(l4x_current_proc_run, cpu) = current_thread_info();
			TBUF_LOG_DSP_IPC_IN(fiasco_tbuf_log_3val("DSP-in (O) ",
			                    TBUF_TID(current->thread.user_thread_id),
			                    label, 0));
#ifdef CONFIG_L4_SERVER
			l4x_srv_setup_recv(utcb);
#endif
			if (l4x_msgtag_fpu(cpu))
				l4_utcb_inherit_fpu_u(utcb, 1);

			tag = l4_ipc_wait(utcb, &label, L4_IPC_SEND_TIMEOUT_0);
			goto after_IPC;
		} else if (unlikely(error)) {

			if (error == L4_IPC_SEMAPFAILED) {
				l4x_evict_tasks(p);
				goto only_receive_IPC;
			}

			LOG_printf("dispatch%d: IPC error = 0x%x (context) (to = "
			           PRINTF_L4TASK_FORM ", %s(%d), src = %lx)\n",
			           cpu, error,
			           PRINTF_L4TASK_ARG(p->thread.user_thread_id),
			           p->comm, p->pid, label);
			enter_kdebug("ipc error");
		}

		data0 = l4_utcb_mr_u(utcb)->mr[0];
		data1 = l4_utcb_mr_u(utcb)->mr[1];

                if (label == 0) {
                        // kernel internal wakeup, just wait again
			goto only_receive_IPC;
                } else if (label != 0x12) {
			// other event
			l4x_handle_async_event(label, utcb, tag);
			goto only_receive_IPC;
		} else {
                        // normal user, do normal path
                }

		// copy utcb now that we have made sure to have received
		// from t
		utcb_to_thread_struct(utcb, p, t);
	} /* endless loop */

	enter_kdebug("end of dispatch loop!?");
	l4x_deliver_signal(13);
} /* l4x_user_dispatcher */

void syscall_exit(void)
{
	struct task_struct *p = current;
	l4x_arch_do_syscall_trace(p);
	l4x_pre_iret_work(task_pt_regs(p), p, 0, 0, 0);
	l4x_user_dispatcher();
}

#endif /* !vcpu */


#ifdef CONFIG_L4_VCPU
static void l4x_handle_external_event(l4_vcpu_state_t *v,
                                      struct pt_regs *regs)
{
	struct l4x_srv_object *o = (struct l4x_srv_object *)(v->i.label & ~3UL);
	l4_msgtag_t tag = v->i.tag;
	o->dispatch(o, v->i.label, l4_utcb(), &tag);
}


void l4x_vcpu_handle_irq(l4_vcpu_state_t *v, struct pt_regs *regs)
{
	int irq;
	unsigned long flags;

	local_irq_save(flags);
	irq = v->i.label >> 2;

#ifdef CONFIG_SMP
	if (irq == L4X_VCPU_IRQ_IPI)
		l4x_vcpu_handle_ipi(regs);
	else
#endif
	if (unlikely(irq >= NR_IRQS))
		l4x_handle_external_event(v, regs);
	else {
#ifdef CONFIG_X86
		do_IRQ(irq, regs);
#else
		handle_IRQ(irq, regs);
#endif
	}
	local_irq_restore(flags);
}

static inline __noreturn
void l4x_vcpu_iret(struct task_struct *p,
                   struct thread_struct *t, struct pt_regs *regs,
		   l4_umword_t fp1, l4_umword_t fp2,
                   int copy_ptregs)
{
	l4_vcpu_state_t *vcpu;
	l4_utcb_t *utcb;
	l4_msgtag_t tag;

restart:
	local_irq_disable();
	utcb = l4_utcb();
	vcpu = l4x_vcpu_state_current();

	if (copy_ptregs)
		state_to_vcpu(vcpu, regs, p);
	else
		vcpu->saved_state &= ~L4_VCPU_F_USER_MODE;

	if (likely(vcpu->saved_state & L4_VCPU_F_USER_MODE)) {
		l4x_pte_check_empty(p->active_mm);
create_task:
		if (unlikely(p->mm && l4_is_invalid_cap(p->mm->context.task))) {
			l4_cap_idx_t c = l4x_user_task_create();
			if (unlikely(l4_is_invalid_cap(c))) {
				local_irq_enable();
				fp1 = 0;
				msleep(1);
				l4x_evict_tasks(p);
				goto restart;
			}

			l4x_arch_task_start_setup(vcpu, p, c);

			task_lock(p);
			if (likely(l4_is_invalid_cap(p->mm->context.task))) {
				p->mm->context.task = c;
#ifdef CONFIG_L4_DEBUG_REGISTER_NAMES
				l4_debugger_set_object_name(c, p->comm);
#endif
				l4x_arch_task_setup(&p->thread);

				vcpu->user_task = c;
				c = L4_INVALID_CAP;
			} else
				vcpu->user_task = p->mm->context.task;
			task_unlock(p);

			if (unlikely(!l4_is_invalid_cap(c))) {
				l4lx_task_delete_task(c);
				l4lx_task_number_free(c);
			}
		} else if (likely(p->mm)
		           && (p->mm->context.task != (vcpu->user_task & ~0xff)))
			vcpu->user_task = p->mm->context.task;


		thread_struct_to_vcpu(vcpu, t);
		vcpu->saved_state |= L4_VCPU_F_USER_MODE
		                     | L4_VCPU_F_IRQ
		                     | L4_VCPU_F_EXCEPTIONS
		                     | L4_VCPU_F_PAGE_FAULTS;

		if (l4x_msgtag_fpu(smp_processor_id()))
			vcpu->saved_state |= L4_VCPU_F_FPU_ENABLED;
		else
			vcpu->saved_state &= ~L4_VCPU_F_FPU_ENABLED;

		l4x_msgtag_copy_ureg(utcb);

	} else {
		if (vcpu->r.sp == 0)
			vcpu->r.sp = (l4_umword_t)task_pt_regs(current);

		vcpu->saved_state |= L4_VCPU_F_EXCEPTIONS;
		vcpu->saved_state &= ~(L4_VCPU_F_DEBUG_EXC | L4_VCPU_F_PAGE_FAULTS);
#ifdef CONFIG_X86_32
		vcpu->r.gs = l4x_x86_utcb_get_orig_segment();
#endif
	}

	while (1) {
#ifdef CONFIG_L4_SERVER
		l4x_srv_setup_recv(utcb);
#endif
		tag = l4_thread_vcpu_resume_start_u(utcb);
		if (fp1)
			l4_sndfpage_add_u((l4_fpage_t)fp2, fp1, &tag, utcb);
		tag = l4_thread_vcpu_resume_commit_u(L4_INVALID_CAP, tag, utcb);

		vcpu->state = 0;

		if (l4_ipc_error(tag, utcb) == L4_IPC_SEMAPFAILED) {
			l4x_evict_tasks(p);
		} else if (l4_error(tag) == -L4_ENOENT) {
			/* In case of new task with same cap-idx we need to
			 * force a new lookup */
			vcpu->user_task = L4_INVALID_CAP;
			goto create_task;
		} else{
			LOG_printf("l4x: cpu%d, resume returned: %ld/%ld [%x, %lx/%lx]\n",
			           smp_processor_id(),
			           l4_error(tag), l4_ipc_error(tag, utcb),
			           vcpu->saved_state,
			           vcpu->user_task, p->mm ? p->mm->context.task : ~0UL);
			enter_kdebug("IRET returned");
			while (1)
				;
		}
	}
}

static inline void l4x_vcpu_entry_sanity(l4_vcpu_state_t *vcpu)
{
#ifdef CONFIG_L4_DEBUG
	// sanity check
	if (unlikely(!(vcpu->saved_state & L4_VCPU_F_IRQ)
	             && l4vcpu_is_irq_entry(vcpu))) {
		LOG_printf("  retip=%08lx\n", _RET_IP_);
		enter_kdebug("sanity: irq");
	}
#endif
}

static void __noreturn
l4x_vcpu_entry_kern(l4_vcpu_state_t *vcpu)
{
	struct pt_regs regs;
	struct pt_regs *regsp = &regs;
	struct task_struct *p = current;
	struct thread_struct *t = &p->thread;
	int copy_ptregs = 0;

	if (l4vcpu_is_irq_entry(vcpu)) {
		vcpu_to_ptregs(vcpu, regsp);
		l4x_vcpu_handle_irq(vcpu, regsp);
		l4x_pre_iret_work(regsp, p, 0, 0, 1);
		copy_ptregs = 1;

	} else if (l4vcpu_is_page_fault_entry(vcpu)) {
		vcpu_to_ptregs(vcpu, regsp);
		if (vcpu->saved_state & L4_VCPU_F_IRQ)
			local_irq_enable();
		l4x_do_page_fault(vcpu->r.pfa, regsp, vcpu->r.err);
	} else {
		int ret = l4x_vcpu_handle_kernel_exc(&vcpu->r);

		if (!ret) {
			unsigned long err    = vcpu_val_err(vcpu);
			unsigned long trapno = vcpu_val_trapno(vcpu);
			vcpu_to_ptregs(vcpu, regsp);
			if (vcpu->saved_state & L4_VCPU_F_IRQ)
				local_irq_enable();
			if (!l4x_dispatch_exception(p, t, err, trapno, regsp))
				l4x_pre_iret_work(regsp, p, 0, 0, 1);
			copy_ptregs = 1;
		}
	}

#ifdef CONFIG_PREEMPT
	if (!preempt_count()) {
		extern asmlinkage void __sched preempt_schedule_irq(void);
		while (need_resched()
#ifdef CONFIG_X86
				&& (regsp->flags & X86_EFLAGS_IF)
#endif
				)
			preempt_schedule_irq();
	}
#endif

	smp_mb();
	l4x_vcpu_iret(p, t, regsp, 0, 0, copy_ptregs);
}

#ifdef ARCH_arm
asm(
".global l4x_vcpu_entry \n\t"
"l4x_vcpu_entry: \n\t"
"	bic	sp, sp, #7\n\t"
"	b	l4x_vcpu_entry_c \n\t"
);

asmlinkage void l4x_vcpu_entry_c(l4_vcpu_state_t *vcpu)
#elif defined(CONFIG_X86_64)
asm(
".global l4x_vcpu_entry \n\t"
"l4x_vcpu_entry: \n\t"
"	andq	$~15, %rsp\n\t"
"	subq	$8, %rsp\n\t"
"	jmp	l4x_vcpu_entry_c \n\t"
);
void l4x_vcpu_entry_c(l4_vcpu_state_t *vcpu)
#else
void l4x_vcpu_entry(l4_vcpu_state_t *vcpu)
#endif
{
	struct pt_regs *regsp;
	struct task_struct *p;
	struct thread_struct *t;

	vcpu->state = 0;
	smp_mb();

	if (likely((vcpu->saved_state & L4_VCPU_F_USER_MODE)))
		l4x_vcpu_entry_user_arch();

	l4x_vcpu_entry_sanity(vcpu);

	if (unlikely(!(vcpu->saved_state & L4_VCPU_F_USER_MODE))) {
		l4x_vcpu_entry_kern(vcpu);
		/* Won't come back */
		enter_kdebug("shouldn't return");
	}

	p = current;
	t = &p->thread;

	regsp = task_pt_regs(p);

	vcpu_to_ptregs(vcpu, regsp);
	vcpu_to_thread_struct(vcpu, t);

	if (l4vcpu_is_irq_entry(vcpu)) {
		l4x_vcpu_handle_irq(vcpu, regsp);
		l4x_pre_iret_work(regsp, p, 0, 0, 0);
	} else if (l4vcpu_is_page_fault_entry(vcpu)) {
		int reply_with_fpage = l4x_dispatch_page_fault(p, t, regsp);

		if (reply_with_fpage) {
			l4_umword_t data0 = 0, data1 = 0;
			local_irq_disable();
			do {
				l4x_pf_pfa_to_fp(p, l4x_l4pfa(t), &data0, &data1);
			} while (l4x_pre_iret_work(regsp, p, 0, 0, 0));
			l4x_vcpu_iret(p, t, regsp, data0, data1, 1);
		}
		l4x_pre_iret_work(regsp, p, 0, 0, 0);
	} else {
		if (!l4x_dispatch_exception(p, t, vcpu_val_err(vcpu),
		                            vcpu_val_trapno(vcpu), regsp))
			l4x_pre_iret_work(regsp, p, 0, 0, 0);
	}

	smp_mb();
	l4x_vcpu_iret(p, t, regsp, 0, 0, 1);
}

void syscall_exit(void)
{
	struct task_struct *p = current;
	struct pt_regs *r = task_pt_regs(p);
	l4x_arch_do_syscall_trace(p);
	l4x_pre_iret_work(r, p, 0, 0, 0);
	l4x_vcpu_iret(p, &p->thread, r, 0, 0, 1);
}

#endif /* vcpu */

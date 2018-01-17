#ifndef _ASM_X86_PARAM_H
#define _ASM_X86_PARAM_H


#ifndef DDE_LINUX
#ifdef __KERNEL__
# define HZ		CONFIG_HZ	/* Internal kernel timer frequency */
# define USER_HZ	100		/* some user interfaces are */
# define CLOCKS_PER_SEC	(USER_HZ)       /* in "ticks" like times() */
#endif

#ifndef HZ
#define HZ 100
#endif
#else /* DDE_LINUX */
extern unsigned long HZ;
#define USER_HZ 250	
#define CLOCKS_PER_SEC (USER_HZ)
#endif /* DDE_LINUX */

#define EXEC_PAGESIZE	4096

#ifndef NOGROUP
#define NOGROUP		(-1)
#endif

#define MAXHOSTNAMELEN	64	/* max length of hostname */

#endif /* _ASM_X86_PARAM_H */

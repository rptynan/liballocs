#define _GNU_SOURCE
#define SYSTRAP_DEFINE_FILE
#include "do-syscall.h"
#include "systrap.h"
#include "vas.h"
#include "allocsmt.h"
#include "maps.h"
#include "pageindex.h"
#define RELF_DEFINE_STRUCTURES
#include "relf.h"

/* Our secret private channel with libdlbind. This must always be linked in,
 * even in libcrunch stubs. */
extern __thread const char *dlbind_open_active_on __attribute__((weak,visibility("hidden")));
/* While weak thread-locals don't actually work (on x86-64).... */
extern int dlbind_dummy __attribute__((weak));

/* NOTE: We also get linked in from libcrunch's stubs file, which lacks most of liballocs.
 * (Read that again. It lacks most of liballocs, not just most of libcrunch.)
 * We used to say "so only call out to the hooks if they're present". But now we define
 * our own nudger, and libcrunch wraps it (or not). */
void __liballocs_nudge_mmap(void **p_addr, size_t *p_length, int *p_prot, int *p_flags,
                  int *p_fd, off_t *p_offset, const void *caller);
void __liballocs_nudge_open(const char **p_pathname, int *p_flags, mode_t *p_mode, const void *caller);
void __mmap_allocator_notify_munmap(void *addr, size_t length, void *caller) __attribute__((weak));
void __mmap_allocator_notify_mremap_before(void *old_addr, size_t old_size, size_t new_size, int flags, void *new_address, void *caller)
			__attribute__((weak));
void __mmap_allocator_notify_mremap_after(void *ret, void *old_addr, size_t old_size, size_t new_size, int flags, void *new_address, void *caller)
			__attribute__((weak));
void __mmap_allocator_notify_mmap(void *ret, void *requested_addr, size_t length, int prot, int flags,
                  int fd, off_t offset, void *caller)
			__attribute__((weak));
void __mmap_allocator_notify_brk(void *new_curbrk) __attribute__((weak));
extern _Bool __liballocs_is_initialized __attribute__((weak));
int __liballocs_global_init(void);
_Bool is_meta_object_for_lib(struct link_map *maybe_types, struct link_map *l, const char *meta_suffix)
			__attribute__((visibility("hidden")));
/* avoid standard headers */
char *realpath(const char *path, char *resolved_path);
int snprintf(char *str, size_t size, const char *format, ...);
int open(const char *pathname, int flags);
int close(int fd);

#define GUESS_CALLER(uc) \
	( (&pageindex && pageindex[ ((uintptr_t) ((uc).rsp)) >> LOG_PAGE_SIZE ] != 0) \
		? *(void**) ((uintptr_t) ((uc).rsp)) \
		: (void*) ((uc).rip) )

void brk_replacement(struct generic_syscall *s, post_handler *post) __attribute__((visibility("hidden")));
void brk_replacement(struct generic_syscall *s, post_handler *post)
{
	/* Linux gives us the old value on failure, and the new value on success. 
	 * In other words it always gives us the current sbrk. */
	void *brk_asked_for = (void*) s->args[0];
	long int ret = do_syscall1(s);
	void *brk_returned = (void*) ret;
	if (&__mmap_allocator_notify_brk) __mmap_allocator_notify_brk(brk_returned);
	
	/* Do the post-handling. */
	post(s, ret);
	
	/* We need to do our own resumption also. */
	resume_from_sigframe(ret, s->saved_context, /* HACK */ 2);
}

void mmap_replacement(struct generic_syscall *s, post_handler *post) __attribute__((visibility("hidden")));
void mmap_replacement(struct generic_syscall *s, post_handler *post)
{
	/* Unpack the mmap arguments. */
	void *addr = (void*) s->args[0];
	size_t length = s->args[1];
	int prot = s->args[2];
	int flags = s->args[3];
	int fd = s->args[4];
	off_t offset = s->args[5];
	
	/* Nudge them. */
	__liballocs_nudge_mmap(&addr, &length, &prot, &flags, 
			&fd, &offset, s->saved_context->pretcode);
	
	/* Re-pack them. */
	s->args[0] = (long int) addr;
	s->args[1] = length;
	s->args[2] = prot;
	s->args[3] = flags;
	s->args[4] = fd;
	s->args[5] = offset;
	
	/* Do the call. */
	long int ret = do_syscall6(s);

	/* If it did something, notify the allocator. */
	if ((void*) ret != (void*) -1 && &__mmap_allocator_notify_mmap)
	{
		__mmap_allocator_notify_mmap((void*) ret, addr, length, prot, flags, fd, offset,
			GUESS_CALLER(s->saved_context->uc.uc_mcontext));
	}

	/* Do the post-handling. */
	post(s, ret);
	
	/* We need to do our own resumption also. */
	resume_from_sigframe(ret, s->saved_context, /* HACK */ 2);
}

void munmap_replacement(struct generic_syscall *s, post_handler *post) __attribute__((visibility("hidden")));
void munmap_replacement(struct generic_syscall *s, post_handler *post)
{
	/* Unpack the mmap arguments. */
	void *addr = (void*) s->args[0];
	size_t length = s->args[1];
	
	/* Do the call. */
	long int ret = do_syscall2(s);
	
	/* If it did something, notify the allocator. */
	if (ret == 0 && &__mmap_allocator_notify_munmap) __mmap_allocator_notify_munmap(addr, length, 
		GUESS_CALLER(s->saved_context->uc.uc_mcontext));
	
	/* Do the post-handling. */
	post(s, ret);
	
	/* We need to do our own resumption also. */
	resume_from_sigframe(ret, s->saved_context, /* HACK */ 2);
}

void mremap_replacement(struct generic_syscall *s, post_handler *post) __attribute__((visibility("hidden")));
void mremap_replacement(struct generic_syscall *s, post_handler *post)
{
	/* Unpack the mremap arguments. */
	void *old_addr = (void*) s->args[0];
	size_t old_length = s->args[1];
	size_t new_length = s->args[2];
	int flags = s->args[3];
	void *maybe_new_address = (void*) s->args[4];
	
	/* Pre-notify the allocator. This is so it can grab the "prot". */
	if (&__mmap_allocator_notify_mremap_before)
	{
		__mmap_allocator_notify_mremap_before(old_addr, old_length, new_length, flags, 
			maybe_new_address, GUESS_CALLER(s->saved_context->uc.uc_mcontext));
	}
	
	/* Do the call. */
	long int ret = do_syscall5(s);
	
	// FIXME: also nudge mremaps
	
	/* Whether or not it did something, notify the allocator. */
	if (&__mmap_allocator_notify_mremap_after)
	{
		__mmap_allocator_notify_mremap_after((void*) ret, old_addr, old_length, new_length, flags, 
			maybe_new_address, GUESS_CALLER(s->saved_context->uc.uc_mcontext));
	}
	
	/* Do the post-handling. */
	post(s, ret);
	
	/* We need to do our own resumption also. */
	resume_from_sigframe(ret, s->saved_context, /* HACK */ 2);
}

void open_replacement(struct generic_syscall *s, post_handler *post) __attribute__((visibility("hidden")));
void open_replacement(struct generic_syscall *s, post_handler *post)
{
	/* Unpack the arguments */
	const char *path = (const char *) s->args[0];
	int flags = s->args[1];
	mode_t mode = s->args[2];
	
	/* Nudge them. */
	__liballocs_nudge_open(&path, &flags, &mode, s->saved_context->pretcode);
	
	/* Repack. */
	s->args[0] = (long int) path;
	s->args[1] = flags;
	s->args[2] = mode;
	
	long int ret = do_syscall3(s);
	
	/* Do the post-handling. */
	post(s, ret);
	
	/* We need to do our own resumption also. */
	resume_from_sigframe(ret, s->saved_context, /* HACK */ 2);
}

static int trap_ldso_or_libdl_cb(struct proc_entry *ent, char *linebuf, void *interpreter_fname_as_void)
{
	const char *interpreter_fname = (const char *) interpreter_fname_as_void;
	if (ent->x == 'x' && (0 == strcmp(interpreter_fname, ent->rest)
		|| 0 == strcmp(basename(ent->rest), "libdl.so.2")))
	{
		/* It's an executable mapping in the ld.so or libdl, so trap it. */
		trap_one_executable_region((unsigned char *) ent->first, (unsigned char *) ent->second,
			interpreter_fname, ent->w == 'w', ent->r == 'r');
	}
	
	return 0;
}

static _Bool trap_syscalls_in_symbol_named(const char *name, struct link_map *l,
	ElfW(Sym) *dynsym, ElfW(Sym) *dynsym_end, const char *dynstr, const char *dynstr_end)
{
	ElfW(Dyn) *gnu_hash_ent = dynamic_lookup(l->l_ld, DT_GNU_HASH);
	ElfW(Sym) *found = gnu_hash_ent ? gnu_hash_lookup((uint32_t*) gnu_hash_ent->d_un.d_ptr, 
		dynsym, dynstr, name) : NULL;
	if (found && found->st_shndx != STN_UNDEF)
	{
		trap_one_instruction_range((unsigned char *)(l->l_addr + found->st_value),
			(unsigned char *)(l->l_addr + found->st_value + found->st_size),
			0, 1);
		return 1;
	} else return 0;
}

extern ElfW(Dyn) _DYNAMIC[];
_Bool __liballocs_systrap_is_initialized; /* globally visible, so that it gets overridden. */
_Bool __lookup_static_allocation_by_name(struct link_map *l, const char *name,
	void **out_addr, size_t *out_len) __attribute__((weak));

/* This is logically a constructor, since it's important that it happens before
 * much stuff has been memory-mapped. BUT we have to hand off smoothly from the
 * mmap allocator, which processes /proc. So it will call us when it's done that.
 * This also avoids the "two copies" problem we had before, because only one copy
 * of this symbol will be callable, and the library initializer doesn't call it. 
 * But it does mean that libcrunch's stubs library has to call us explicitly. */
void __liballocs_systrap_init(void)
{
	/* NOTE: in a preload libcrunch run, there are two copies of this code running!
	 * One is in the preload library, the other in the stubs library. We should only
	 * run one of them.
	 * 
	 * Also, it's important that we don't call malloc() in this function, because its
	 * self-call detection will be foiled: we'll be calling *from* the stubs library 
	 * *into* the preload library. It's okay to call malloc from (non-static) things we 
	 * call, because it'll be the preload's copy of that callee that actually gets called.
	 * ARGH, but -Bsymbolic may have screwed with this. Oh, but we don't use it for the 
	 * stubs library, I don't think. Indeed, we don't. */
	if (__liballocs_systrap_is_initialized) return;
	
	/* To figure out what to trap, we're going to use the static allocation metadata from
	 * liballocs. So make sure we've done that. */
	if (&__liballocs_is_initialized && !__liballocs_is_initialized) __liballocs_global_init();
	
	static char realpath_buf[4096]; /* bit of a HACK */
	/* Make sure we're trapping all syscalls within ld.so. */
	replaced_syscalls[SYS_mmap] = mmap_replacement;
	replaced_syscalls[SYS_munmap] = munmap_replacement;
	replaced_syscalls[SYS_mremap] = mremap_replacement;
	replaced_syscalls[SYS_brk] = brk_replacement;
	replaced_syscalls[SYS_open] = open_replacement;
	/* Get a hold of the ld.so's link map entry. How? We get it from the auxiliary
	 * vector. */
	const char *interpreter_fname = NULL;
	ElfW(auxv_t) *auxv = get_auxv((const char **) environ, &interpreter_fname);
	if (!auxv) abort();
	ElfW(auxv_t) *auxv_at_base = auxv_lookup(auxv, AT_BASE);
	if (!auxv_at_base) abort();
	const void *interpreter_base = (const void *) auxv_at_base->a_un.a_val;
	for (struct link_map *l = find_r_debug()->r_map; l; l = l->l_next)
	{
		if ((const void *) l->l_addr == interpreter_base)
		{
			interpreter_fname = realpath(l->l_name, &realpath_buf[0]);
		}
	}
	if (!interpreter_fname) abort();
	struct proc_entry entry;
	char proc_buf[4096];
	int ret;
	ret = snprintf(proc_buf, sizeof proc_buf, "/proc/%d/maps", getpid());
	if (!(ret > 0)) abort();
	int fd = open(proc_buf, O_RDONLY);
	if (fd == -1) abort();
	char linebuf[8192];
	for_each_maps_entry(fd, linebuf, sizeof linebuf, &entry, trap_ldso_or_libdl_cb, (void*) interpreter_fname);
	close(fd);

	/* Also trap the mmap and mmap64 calls in the libc so. Again, we use relf
	 * routines to look them up. How do we identify libc in the link map?
	 * We don't! Instead, look for anything called "mmap" in any *other* object
	 * than ourselves (... HACK/workaround: that doesn't have a weird address
	 * or absent name -- these are vdso or other abominations). */
	_Bool found_an_mmap = 0;
	_Bool found_a_brk_or_sbrk = 0;
	for (struct link_map *l = find_r_debug()->r_map; l; l = l->l_next)
	{
		if ((const void *) l->l_ld != &_DYNAMIC && (intptr_t) l->l_addr > 0
			&& strlen(l->l_name) > 0)
		{
			ElfW(Dyn) *dynsym_ent = dynamic_lookup(l->l_ld, DT_SYMTAB);
			if (!dynsym_ent) continue;
			ElfW(Sym) *dynsym = (ElfW(Sym) *) dynsym_ent->d_un.d_ptr;
			if ((intptr_t) dynsym < 0) continue; /* HACK x86-64 vdso bug */
			if ((char*) dynsym < (char*) get_highest_loaded_object_below((void*) l->l_addr)->l_addr) continue; /* HACK x86-64 vdso bug */
			/* nasty hack for getting the end of dynsym */
			ElfW(Dyn) *dynstr_ent = dynamic_lookup(l->l_ld, DT_STRTAB);
			if (!dynstr_ent) continue;
			char *dynstr = (char *) dynstr_ent->d_un.d_ptr;
			if ((intptr_t) dynstr < 0) continue; /* HACK x86-64 vdso bug */
			if ((char*) dynstr < (char*) get_highest_loaded_object_below((void*) l->l_addr)->l_addr) continue; /* HACK x86-64 vdso bug */
			assert((char *) dynstr > (char *) dynsym);
			ElfW(Dyn) *dynstrsz_ent = dynamic_lookup(l->l_ld, DT_STRSZ);
			if (!dynstrsz_ent) continue;
			unsigned long dynstrsz = dynstrsz_ent->d_un.d_val;
			ElfW(Sym) *dynsym_end = dynsym + dynamic_symbol_count(l->l_ld, l);

			/* Now we can look up symbols. */
#define TRAP_SYSCALLS_IN_SYMBOL_NAMED(n) do { \
			ElfW(Sym) *found_ ## n = symbol_lookup_linear(dynsym, dynsym_end, dynstr, \
				dynstr + dynstrsz, #n); \
			if (found_ ## n && found_ ## n ->st_shndx != STN_UNDEF) \
			{ \
				trap_one_instruction_range((unsigned char *)(l->l_addr + found_ ## n ->st_value),  \
					(unsigned char *)(l->l_addr + found_ ## n ->st_value + found_ ## n ->st_size), \
					0, 1); \
			} \
			} while (0)
			
			found_an_mmap |= trap_syscalls_in_symbol_named("mmap",   l, dynsym, dynsym_end, dynstr, dynstr + dynstrsz);
			found_an_mmap |= trap_syscalls_in_symbol_named("mmap64", l, dynsym, dynsym_end, dynstr, dynstr + dynstrsz);
			trap_syscalls_in_symbol_named("munmap", l, dynsym, dynsym_end, dynstr, dynstr + dynstrsz);
			trap_syscalls_in_symbol_named("mremap", l, dynsym, dynsym_end, dynstr, dynstr + dynstrsz);
			_Bool found_sbrk = trap_syscalls_in_symbol_named("sbrk", l, dynsym, dynsym_end, dynstr, dynstr + dynstrsz);

			// we want to trap syscalls in "__brk"; // glibc HACK!
			// but "__brk" in glibc isn't an exportd symbol.
			// instead, we need to walk its allocations
			if (found_sbrk) // glibc HACK!
			{
				found_a_brk_or_sbrk = 1;
				if (&__lookup_static_allocation_by_name)
				{
					/* We want to trap syscalls in "__brk" but "__brk" in glibc 
					 * isn't an exported symbol. So consult our allocs data for 
					 * a definition named "__brk" and trap that. */
					void *addr;
					size_t len;
					_Bool success = __lookup_static_allocation_by_name(l, "__brk", &addr, &len);
					if (success)
					{
						trap_one_instruction_range((unsigned char*) addr, 
							(unsigned char*) addr + len, 0, 1);
					}
				}
			}
		}
	}

	/* There's no hope of us working if we didn't find these. ACTUALLY don't abort
	 * though, because aborting during startup stops gdb from taking control. */
	// if (!found_an_mmap) abort();
	// if (!found_a_brk_or_sbrk) abort();

	install_sigill_handler();
	__liballocs_systrap_is_initialized = 1;
}

void __liballocs_nudge_mmap(void **p_addr, size_t *p_length, int *p_prot, int *p_flags,
                  int *p_fd, off_t *p_offset, const void *caller)
{
	if (&dlbind_dummy && dlbind_open_active_on)
	{
		*p_flags &= ~(0x3 /*MAP_SHARED|MAP_PRIVATE*/);
		*p_flags |= 0x1 /* MAP_SHARED */;
	}
}

void __liballocs_nudge_open(const char **p_pathname, int *p_flags, mode_t *p_mode, const void *caller)
{
	if (&dlbind_dummy && dlbind_open_active_on)
	{
		*p_flags &= ~(O_RDWR|O_RDONLY);
		*p_flags |= O_RDWR;
	}
}

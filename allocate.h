#ifndef ALLOCATE_H
#define ALLOCATE_H

struct allocation_blob {
	struct allocation_blob *next;
	unsigned int left, offset;
	unsigned char data[];
};

struct allocator_struct {
	const char *name;
	struct allocation_blob *blobs;
	unsigned int alignment;
	unsigned int chunking;
	void *freelist;
	/* statistics */
	unsigned int allocations, total_bytes, useful_bytes;
	unsigned int nofree : 1;
};

extern void protect_allocations(struct allocator_struct *desc);
extern void drop_all_allocations(struct allocator_struct *desc);
extern void *allocate(struct allocator_struct *desc, unsigned int size);
extern void free_one_entry(struct allocator_struct *desc, void *entry);
extern void show_allocations(struct allocator_struct *);

#define __DECLARE_ALLOCATOR(type, x)		\
	extern type *__alloc_##x(int);		\
	extern void __free_##x(type *);		\
	extern void show_##x##_alloc(void);	\
	extern void clear_##x##_alloc(void);	\
	extern void protect_##x##_alloc(void);
#define DECLARE_ALLOCATOR(x) __DECLARE_ALLOCATOR(struct x, x)

#define __DO_ALLOCATOR(type, objsize, objalign, objname, x, norel)	\
	static struct allocator_struct x##_allocator = {	\
		.name = objname,				\
		.alignment = objalign,				\
		.chunking = CHUNK,				\
		.nofree = norel					\
	};							\
	type *__alloc_##x(int extra)				\
	{							\
		return allocate(&x##_allocator, objsize+extra);	\
	}							\
	void __free_##x(type *entry)				\
	{							\
		if ((!x##_allocator.nofree))			\
			free_one_entry(&x##_allocator, entry);	\
	}							\
	void show_##x##_alloc(void)				\
	{							\
		show_allocations(&x##_allocator);		\
	}							\
	void clear_##x##_alloc(void)				\
	{							\
		if ((!x##_allocator.nofree))			\
			drop_all_allocations(&x##_allocator);	\
	}							\
	void protect_##x##_alloc(void)				\
	{							\
		protect_allocations(&x##_allocator);		\
	}

#define __ALLOCATOR(t, n, x, norel) 					\
	__DO_ALLOCATOR(t, sizeof(t), __alignof__(t), n, x, norel)

#define ALLOCATOR(x, n, norel) __ALLOCATOR(struct x, n, x, norel)

DECLARE_ALLOCATOR(ident);
DECLARE_ALLOCATOR(token);
DECLARE_ALLOCATOR(sym_context);
DECLARE_ALLOCATOR(symbol);
DECLARE_ALLOCATOR(expression);
DECLARE_ALLOCATOR(statement);
DECLARE_ALLOCATOR(string);
DECLARE_ALLOCATOR(scope);
__DECLARE_ALLOCATOR(void, bytes);
DECLARE_ALLOCATOR(basic_block);
DECLARE_ALLOCATOR(entrypoint);
DECLARE_ALLOCATOR(instruction);
DECLARE_ALLOCATOR(multijmp);
DECLARE_ALLOCATOR(phi);
DECLARE_ALLOCATOR(pseudo);

#endif

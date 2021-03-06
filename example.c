/*
 * Example of how to write a compiler with sparse
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#include "symbol.h"
#include "expression.h"
#include "linearize.h"
#include "flow.h"
#include "storage.h"
#include "target.h"

static const char *opcodes[] = {
	[OP_BADOP] = "bad_op",

	/* Fn entrypoint */
	[OP_ENTRY] = "<entry-point>",

	/* Terminator */
	[OP_RET] = "ret",
	[OP_BR] = "br",
	[OP_SWITCH] = "switch",
	[OP_INVOKE] = "invoke",
	[OP_COMPUTEDGOTO] = "jmp *",
	[OP_UNWIND] = "unwind",
	
	/* Binary */
	[OP_ADD_LIN] = "add",
	[OP_SUB] = "sub",
	[OP_MULU] = "mulu",
	[OP_MULS] = "muls",
	[OP_DIVU] = "divu",
	[OP_DIVS] = "divs",
	[OP_MODU] = "modu",
	[OP_MODS] = "mods",
	[OP_SHL] = "shl",
	[OP_LSR] = "lsr",
	[OP_ASR] = "asr",
	
	/* Logical */
	[OP_AND_LIN] = "and",
	[OP_OR_LIN] = "or",
	[OP_XOR_LIN] = "xor",
	[OP_AND_BOOL] = "and-bool",
	[OP_OR_BOOL] = "or-bool",

	/* Binary comparison */
	[OP_SET_EQ] = "seteq",
	[OP_SET_NE] = "setne",
	[OP_SET_LE] = "setle",
	[OP_SET_GE] = "setge",
	[OP_SET_LT] = "setlt",
	[OP_SET_GT] = "setgt",
	[OP_SET_B] = "setb",
	[OP_SET_A] = "seta",
	[OP_SET_BE] = "setbe",
	[OP_SET_AE] = "setae",

	/* Uni */
	[OP_NOT_LIN] = "not",
	[OP_NEG] = "neg",

	/* Special three-input */
	[OP_SEL] = "select",
	
	/* Memory */
	[OP_MALLOC] = "malloc",
	[OP_FREE] = "free",
	[OP_ALLOCA] = "alloca",
	[OP_LOAD] = "load",
	[OP_STORE] = "store",
	[OP_SETVAL] = "set",
	[OP_GET_ELEMENT_PTR] = "getelem",

	/* Other */
	[OP_PHI] = "phi",
	[OP_PHISOURCE] = "phisrc",
	[OP_COPY] = "copy",
	[OP_CAST] = "cast",
	[OP_SCAST] = "scast",
	[OP_FPCAST] = "fpcast",
	[OP_PTRCAST] = "ptrcast",
	[OP_CALL] = "call",
	[OP_VANEXT] = "va_next",
	[OP_VAARG] = "va_arg",
	[OP_SLICE] = "slice",
	[OP_SNOP] = "snop",
	[OP_LNOP] = "lnop",
	[OP_NOP] = "nop",
	[OP_DEATHNOTE] = "dead",
	[OP_ASM] = "asm",

	/* Sparse tagging (line numbers, context, whatever) */
	[OP_CONTEXT] = "context",
};

static int last_reg, stack_offset;

struct hardreg {
	const char *name;
	struct pseudo_list *contains;
	unsigned busy:16,
		 dead:8,
		 used:1;
};

#define TAG_DEAD 1
#define TAG_DIRTY 2

/* Our "switch" generation is very very stupid. */
#define SWITCH_REG (1)

static void output_bb(SCTX_ struct basic_block *bb, unsigned long generation);

/*
 * We only know about the caller-clobbered registers
 * right now.
 */
static struct hardreg hardregs[] = {
	{ .name = "%eax" },
	{ .name = "%edx" },
	{ .name = "%ecx" },
	{ .name = "%ebx" },
	{ .name = "%esi" },
	{ .name = "%edi" },

	{ .name = "%ebp" },
	{ .name = "%esp" },
};
#define REGNO 6
#define REG_EBP 6
#define REG_ESP 7

struct bb_state {
	struct position pos;
	struct storage_hash_list *inputs;
	struct storage_hash_list *outputs;
	struct storage_hash_list *internal;

	/* CC cache.. */
	int cc_opcode, cc_dead;
	pseudo_t cc_target;
};

enum optype {
	OP_UNDEF,
	OP_REG,
	OP_VAL,
	OP_MEM,
	OP_ADDR,
};

struct operand {
	enum optype type;
	int size;
	union {
		struct hardreg *reg;
		long long value;
		struct /* OP_MEM and OP_ADDR */ {
			unsigned int offset;
			unsigned int scale;
			struct symbol *sym;
			struct hardreg *base;
			struct hardreg *index;
		};
	};
};

static const char *show_op(SCTX_ struct bb_state *state, struct operand *op)
{
	static char buf[256][4];
	static int bufnr;
	char *p, *ret;
	int nr;

	nr = (bufnr + 1) & 3;
	bufnr = nr;
	ret = p = buf[nr];

	switch (op->type) {
	case OP_UNDEF:
		return "undef";
	case OP_REG:
		return op->reg->name;
	case OP_VAL:
		sprintf(p, "$%lld", op->value);
		break;
	case OP_MEM:
	case OP_ADDR:
		if (op->offset)
			p += sprintf(p, "%d", op->offset);
		if (op->sym)
			p += sprintf(p, "%s%s",
				op->offset ? "+" : "",
				show_ident(sctx_ op->sym->ident));
		if (op->base || op->index) {
			p += sprintf(p, "(%s%s%s",
				op->base ? op->base->name : "",
				(op->base && op->index) ? "," : "",
				op->index ? op->index->name : "");
			if (op->scale > 1)
				p += sprintf(p, ",%d", op->scale);
			*p++ = ')';
			*p = '\0';
		}
		break;
	}
	return ret;
}

static struct storage_hash *find_storage_hash(SCTX_ pseudo_t pseudo, struct storage_hash_list *list)
{
	struct storage_hash *entry;
	FOR_EACH_PTR(list, entry) {
		if (entry->pseudo == pseudo)
			return entry;
	} END_FOR_EACH_PTR(entry);
	return NULL;
}

static struct storage_hash *find_or_create_hash(SCTX_ pseudo_t pseudo, struct storage_hash_list **listp)
{
	struct storage_hash *entry;

	entry = find_storage_hash(sctx_ pseudo, *listp);
	if (!entry) {
		entry = alloc_storage_hash(sctx_ alloc_storage(sctx));
		entry->pseudo = pseudo;
		add_ptr_list(listp, entry);
	}
	return entry;
}

/* Eventually we should just build it up in memory */
static void FORMAT_ATTR(2+SCTXCNT) output_line(SCTX_ struct bb_state *state, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

static void FORMAT_ATTR(2+SCTXCNT) output_label(SCTX_ struct bb_state *state, const char *fmt, ...)
{
	static char buffer[512];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	output_line(sctx_ state, "%s:\n", buffer);
}

static void FORMAT_ATTR(2+SCTXCNT) output_insn(SCTX_ struct bb_state *state, const char *fmt, ...)
{
	static char buffer[512];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	output_line(sctx_ state, "\t%s\n", buffer);
}

#define output_insn(state, fmt, arg...) \
	output_insn(state, fmt "\t\t# %s" , ## arg , __FUNCTION__)

static void FORMAT_ATTR(2+SCTXCNT) output_comment(SCTX_ struct bb_state *state, const char *fmt, ...)
{
	static char buffer[512];
	va_list args;

	if (!sctxp verbose)
		return;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	output_line(sctx_ state, "\t# %s\n", buffer);
}

static const char *show_memop(SCTX_ struct storage *storage)
{
	static char buffer[1000];

	if (!storage)
		return "undef";
	switch (storage->type) {
	case REG_FRAME:
		sprintf(buffer, "%d(FP)", storage->offset);
		break;
	case REG_STACK:
		sprintf(buffer, "%d(SP)", storage->offset);
		break;
	case REG_REG:
		return hardregs[storage->regno].name;
	default:
		return show_storage(sctx_ storage);
	}
	return buffer;
}

static int alloc_stack_offset(SCTX_ int size)
{
	int ret = stack_offset;
	stack_offset = ret + size;
	return ret;
}

static void alloc_stack(SCTX_ struct bb_state *state, struct storage *storage)
{
	storage->type = REG_STACK;
	storage->offset = alloc_stack_offset(sctx_ 4);
}

/*
 * Can we re-generate the pseudo, so that we don't need to
 * flush it to memory? We can regenerate:
 *  - immediates and symbol addresses
 *  - pseudos we got as input in non-registers
 *  - pseudos we've already saved off earlier..
 */
static int can_regenerate(SCTX_ struct bb_state *state, pseudo_t pseudo)
{
	struct storage_hash *in;

	switch (pseudo->type) {
	case PSEUDO_VAL:
	case PSEUDO_SYM:
		return 1;

	default:
		in = find_storage_hash(sctx_ pseudo, state->inputs);
		if (in && in->storage->type != REG_REG)
			return 1;
		in = find_storage_hash(sctx_ pseudo, state->internal);
		if (in)
			return 1;
	}
	return 0;
}

static void flush_one_pseudo(SCTX_ struct bb_state *state, struct hardreg *hardreg, pseudo_t pseudo)
{
	struct storage_hash *out;
	struct storage *storage;

	if (can_regenerate(sctx_ state, pseudo))
		return;

	output_comment(sctx_ state, "flushing %s from %s", show_pseudo(sctx_ pseudo), hardreg->name);
	out = find_storage_hash(sctx_ pseudo, state->internal);
	if (!out) {
		out = find_storage_hash(sctx_ pseudo, state->outputs);
		if (!out)
			out = find_or_create_hash(sctx_ pseudo, &state->internal);
	}
	storage = out->storage;
	switch (storage->type) {
	default:
		/*
		 * Aieee - the next user wants it in a register, but we
		 * need to flush it to memory in between. Which means that
		 * we need to allocate an internal one, dammit..
		 */
		out = find_or_create_hash(sctx_ pseudo, &state->internal);
		storage = out->storage;
		/* Fall through */
	case REG_UDEF:
		alloc_stack(sctx_ state, storage);
		/* Fall through */
	case REG_STACK:
		output_insn(sctx_ state, "movl %s,%s", hardreg->name, show_memop(sctx_ storage));
		break;
	}
}

/* Flush a hardreg out to the storage it has.. */
static void flush_reg(SCTX_ struct bb_state *state, struct hardreg *reg)
{
	pseudo_t pseudo;

	if (reg->busy)
		output_comment(sctx_ state, "reg %s flushed while busy is %d!", reg->name, reg->busy);
	if (!reg->contains)
		return;
	reg->dead = 0;
	reg->used = 1;
	FOR_EACH_PTR(reg->contains, pseudo) {
		if (CURRENT_TAG(pseudo) & TAG_DEAD)
			continue;
		if (!(CURRENT_TAG(pseudo) & TAG_DIRTY))
			continue;
		flush_one_pseudo(sctx_ state, reg, pseudo);
	} END_FOR_EACH_PTR(pseudo);
	free_ptr_list(&reg->contains);
}

static struct storage_hash *find_pseudo_storage(SCTX_ struct bb_state *state, pseudo_t pseudo, struct hardreg *reg)
{
	struct storage_hash *src;

	src = find_storage_hash(sctx_ pseudo, state->internal);
	if (!src) {
		src = find_storage_hash(sctx_ pseudo, state->inputs);
		if (!src) {
			src = find_storage_hash(sctx_ pseudo, state->outputs);
			/* Undefined? Screw it! */
			if (!src)
				return NULL;

			/*
			 * If we found output storage, it had better be local stack
			 * that we flushed to earlier..
			 */
			if (src->storage->type != REG_STACK)
				return NULL;
		}
	}

	/*
	 * Incoming pseudo with out any pre-set storage allocation?
	 * We can make up our own, and obviously prefer to get it
	 * in the register we already selected (if it hasn't been
	 * used yet).
	 */
	if (src->storage->type == REG_UDEF) {
		if (reg && !reg->used) {
			src->storage->type = REG_REG;
			src->storage->regno = reg - hardregs;
			return NULL;
		}
		alloc_stack(sctx_ state, src->storage);
	}
	return src;
}

static void mark_reg_dead(SCTX_ struct bb_state *state, pseudo_t pseudo, struct hardreg *reg)
{
	pseudo_t p;

	FOR_EACH_PTR(reg->contains, p) {
		if (p != pseudo)
			continue;
		if (CURRENT_TAG(p) & TAG_DEAD)
			continue;
		output_comment(sctx_ state, "marking pseudo %s in reg %s dead", show_pseudo(sctx_ pseudo), reg->name);
		TAG_CURRENT(p, TAG_DEAD);
		reg->dead++;
	} END_FOR_EACH_PTR(p);
}

static void add_pseudo_reg(SCTX_ struct bb_state *state, pseudo_t pseudo, struct hardreg *reg)
{
	output_comment(sctx_ state, "added pseudo %s to reg %s", show_pseudo(sctx_ pseudo), reg->name);
	add_ptr_list_tag(&reg->contains, pseudo, TAG_DIRTY);
}

static struct hardreg *preferred_reg(SCTX_ struct bb_state *state, pseudo_t target)
{
	struct storage_hash *dst;

	dst = find_storage_hash(sctx_ target, state->outputs);
	if (dst) {
		struct storage *storage = dst->storage;
		if (storage->type == REG_REG)
			return hardregs + storage->regno;
	}
	return NULL;
}

static struct hardreg *empty_reg(SCTX_ struct bb_state *state)
{
	int i;
	struct hardreg *reg = hardregs;

	for (i = 0; i < REGNO; i++, reg++) {
		if (!reg->contains)
			return reg;
	}
	return NULL;
}

static struct hardreg *target_reg(SCTX_ struct bb_state *state, pseudo_t pseudo, pseudo_t target)
{
	int i;
	int unable_to_find_reg = 0;
	struct hardreg *reg;

	/* First, see if we have a preferred target register.. */
	reg = preferred_reg(sctx_ state, target);
	if (reg && !reg->contains)
		goto found;

	reg = empty_reg(sctx_ state);
	if (reg)
		goto found;

	i = last_reg;
	do {
		i++;
		if (i >= REGNO)
			i = 0;
		reg = hardregs + i;
		if (!reg->busy) {
			flush_reg(sctx_ state, reg);
			last_reg = i;
			goto found;
		}
	} while (i != last_reg);
	assert(unable_to_find_reg);

found:
	add_pseudo_reg(sctx_ state, pseudo, reg);
	return reg;
}

static struct hardreg *find_in_reg(SCTX_ struct bb_state *state, pseudo_t pseudo)
{
	int i;
	struct hardreg *reg;

	for (i = 0; i < REGNO; i++) {
		pseudo_t p;

		reg = hardregs + i;
		FOR_EACH_PTR(reg->contains, p) {
			if (p == pseudo) {
				last_reg = i;
				output_comment(sctx_ state, "found pseudo %s in reg %s (busy=%d)", show_pseudo(sctx_ pseudo), reg->name, reg->busy);
				return reg;
			}
		} END_FOR_EACH_PTR(p);
	}
	return NULL;
}

static void flush_pseudo(SCTX_ struct bb_state *state, pseudo_t pseudo, struct storage *storage)
{
	struct hardreg *reg = find_in_reg(sctx_ state, pseudo);

	if (reg)
		flush_reg(sctx_ state, reg);
}

static void flush_cc_cache_to_reg(SCTX_ struct bb_state *state, pseudo_t pseudo, struct hardreg *reg)
{
	int opcode = state->cc_opcode;

	state->cc_opcode = 0;
	state->cc_target = NULL;
	output_insn(sctx_ state, "%s %s", opcodes[opcode], reg->name);
}

static void flush_cc_cache(SCTX_ struct bb_state *state)
{
	pseudo_t pseudo = state->cc_target;

	if (pseudo) {
		struct hardreg *dst;

		state->cc_target = NULL;

		if (!state->cc_dead) {
			dst = target_reg(sctx_ state, pseudo, pseudo);
			flush_cc_cache_to_reg(sctx_ state, pseudo, dst);
		}
	}
}

static void add_cc_cache(SCTX_ struct bb_state *state, int opcode, pseudo_t pseudo)
{
	assert(!state->cc_target);
	state->cc_target = pseudo;
	state->cc_opcode = opcode;
	state->cc_dead = 0;
	output_comment(sctx_ state, "caching %s", opcodes[opcode]);
}

/* Fill a hardreg with the pseudo it has */
static struct hardreg *fill_reg(SCTX_ struct bb_state *state, struct hardreg *hardreg, pseudo_t pseudo)
{
	struct storage_hash *src;
	struct instruction *def;

	if (state->cc_target == pseudo) {
		flush_cc_cache_to_reg(sctx_ state, pseudo, hardreg);
		return hardreg;
	}

	switch (pseudo->type) {
	case PSEUDO_VAL:
		output_insn(sctx_ state, "movl $%lld,%s", pseudo->value, hardreg->name);
		break;
	case PSEUDO_SYM:
		src = find_pseudo_storage(sctx_ state, pseudo, NULL);
		/* Static thing? */
		if (!src) {
			output_insn(sctx_ state, "movl $<%s>,%s", show_pseudo(sctx_ pseudo), hardreg->name);
			break;
		}
		switch (src->storage->type) {
		case REG_REG:
			/* Aiaiaiaiaii! Need to flush it to temporary memory */
			src = find_or_create_hash(sctx_ pseudo, &state->internal);
			/* Fall through */
		default:
			alloc_stack(sctx_ state, src->storage);
			/* Fall through */
		case REG_STACK:
		case REG_FRAME:
			flush_pseudo(sctx_ state, pseudo, src->storage);
			output_insn(sctx_ state, "leal %s,%s", show_memop(sctx_ src->storage), hardreg->name);
			break;
		}
		break;
	case PSEUDO_ARG:
	case PSEUDO_REG:
		def = pseudo->def;
		if (def && def->opcode == OP_SETVAL) {
			output_insn(sctx_ state, "movl $<%s>,%s", show_pseudo(sctx_ def->target), hardreg->name);
			break;
		}
		src = find_pseudo_storage(sctx_ state, pseudo, hardreg);
		if (!src)
			break;
		if (src->flags & TAG_DEAD)
			mark_reg_dead(sctx_ state, pseudo, hardreg);
		output_insn(sctx_ state, "mov.%d %s,%s", 32, show_memop(sctx_ src->storage), hardreg->name);
		break;
	default:
		output_insn(sctx_ state, "reload %s from %s", hardreg->name, show_pseudo(sctx_ pseudo));
		break;
	}
	return hardreg;
}

static struct hardreg *getreg(SCTX_ struct bb_state *state, pseudo_t pseudo, pseudo_t target)
{
	struct hardreg *reg;

	reg = find_in_reg(sctx_ state, pseudo);
	if (reg)
		return reg;
	reg = target_reg(sctx_ state, pseudo, target);
	return fill_reg(sctx_ state, reg, pseudo);
}

static void move_reg(SCTX_ struct bb_state *state, struct hardreg *src, struct hardreg *dst)
{
	output_insn(sctx_ state, "movl %s,%s", src->name, dst->name);
}

static struct hardreg *copy_reg(SCTX_ struct bb_state *state, struct hardreg *src, pseudo_t target)
{
	int i;
	struct hardreg *reg;

	/* If the container has been killed off, just re-use it */
	if (!src->contains)
		return src;

	/* If "src" only has one user, and the contents are dead, we can re-use it */
	if (src->busy == 1 && src->dead == 1)
		return src;

	reg = preferred_reg(sctx_ state, target);
	if (reg && !reg->contains) {
		output_comment(sctx_ state, "copying %s to preferred target %s", show_pseudo(sctx_ target), reg->name);
		move_reg(sctx_ state, src, reg);
		return reg;
	}

	for (i = 0; i < REGNO; i++) {
		reg = hardregs + i;
		if (!reg->contains) {
			output_comment(sctx_ state, "copying %s to %s", show_pseudo(sctx_ target), reg->name);
			output_insn(sctx_ state, "movl %s,%s", src->name, reg->name);
			return reg;
		}
	}

	flush_reg(sctx_ state, src);
	return src;
}

static void put_operand(SCTX_ struct bb_state *state, struct operand *op)
{
	switch (op->type) {
	case OP_REG:
		op->reg->busy--;
		break;
	case OP_ADDR:
	case OP_MEM:
		if (op->base)
			op->base->busy--;
		if (op->index)
			op->index->busy--;
		break;
	default:
		break;
	}
}

static struct operand *alloc_op(SCTX)
{
	struct operand *op = malloc(sizeof(*op));
	memset(op, 0, sizeof(*op));
	return op;
}

static struct operand *get_register_operand(SCTX_ struct bb_state *state, pseudo_t pseudo, pseudo_t target)
{
	struct operand *op = alloc_op(sctx);
	op->type = OP_REG;
	op->reg = getreg(sctx_ state, pseudo, target);
	op->reg->busy++;
	return op;
}

static int get_sym_frame_offset(SCTX_ struct bb_state *state, pseudo_t pseudo)
{
	int offset = pseudo->nr;
	if (offset < 0) {
		offset = alloc_stack_offset(sctx_ 4);
		pseudo->nr = offset;
	}
	return offset;
}

static struct operand *get_generic_operand(SCTX_ struct bb_state *state, pseudo_t pseudo)
{
	struct hardreg *reg;
	struct storage *src;
	struct storage_hash *hash;
	struct operand *op = malloc(sizeof(*op));

	memset(op, 0, sizeof(*op));
	switch (pseudo->type) {
	case PSEUDO_VAL:
		op->type = OP_VAL;
		op->value = pseudo->value;
		break;

	case PSEUDO_SYM: {
		struct symbol *sym = pseudo->sym;
		op->type = OP_ADDR;
		if (sym->ctype.modifiers & MOD_NONLOCAL) {
			op->sym = sym;
			break;
		}
		op->base = hardregs + REG_EBP;
		op->offset = get_sym_frame_offset(sctx_ state, pseudo);
		break;
	}

	default:
		reg = find_in_reg(sctx_ state, pseudo);
		if (reg) {
			op->type = OP_REG;
			op->reg = reg;
			reg->busy++;
			break;
		}
		hash = find_pseudo_storage(sctx_ state, pseudo, NULL);
		if (!hash)
			break;
		src = hash->storage;
		switch (src->type) {
		case REG_REG:
			op->type = OP_REG;
			op->reg = hardregs + src->regno;
			op->reg->busy++;
			break;
		case REG_FRAME:
			op->type = OP_MEM;
			op->offset = src->offset;
			op->base = hardregs + REG_EBP;
			break;
		case REG_STACK:
			op->type = OP_MEM;
			op->offset = src->offset;
			op->base = hardregs + REG_ESP;
			break;
		default:
			break;
		}
	}
	return op;
}

/* Callers should be made to use the proper "operand" formats */
static const char *generic(SCTX_ struct bb_state *state, pseudo_t pseudo)
{
	struct hardreg *reg;
	struct operand *op = get_generic_operand(sctx_ state, pseudo);
	static char buf[100];
	const char *str;

	switch (op->type) {
	case OP_ADDR:
		if (!op->offset && op->base && !op->sym)
			return op->base->name;
		if (op->sym && !op->base) {
			int len = sprintf(buf, "$ %s", show_op(sctx_ state, op));
			if (op->offset)
				sprintf(buf + len, " + %d", op->offset);
			return buf;
		}
		str = show_op(sctx_ state, op);
		put_operand(sctx_ state, op);
		reg = target_reg(sctx_ state, pseudo, NULL);
		output_insn(sctx_ state, "lea %s,%s", show_op(sctx_ state, op), reg->name);
		return reg->name;		

	default:
		str = show_op(sctx_ state, op);
	}
	put_operand(sctx_ state, op);
	return str;
}

static struct operand *get_address_operand(SCTX_ struct bb_state *state, struct instruction *memop)
{
	struct hardreg *base;
	struct operand *op = get_generic_operand(sctx_ state, memop->src);

	switch (op->type) {
	case OP_ADDR:
		op->offset += memop->offset;
		break;
	default:
		put_operand(sctx_ state, op);
		base = getreg(sctx_ state, memop->src, NULL);
		op->type = OP_ADDR;
		op->base = base;
		base->busy++;
		op->offset = memop->offset;
		op->sym = NULL;
	}
	return op;
}

static const char *address(SCTX_ struct bb_state *state, struct instruction *memop)
{
	struct operand *op = get_address_operand(sctx_ state, memop);
	const char *str = show_op(sctx_ state, op);
	put_operand(sctx_ state, op);
	return str;
}

static const char *reg_or_imm(SCTX_ struct bb_state *state, pseudo_t pseudo)
{
	switch(pseudo->type) {
	case PSEUDO_VAL:
		return show_pseudo(sctx_ pseudo);
	default:
		return getreg(sctx_ state, pseudo, NULL)->name;
	}
}

static void kill_dead_reg(SCTX_ struct hardreg *reg)
{
	if (reg->dead) {
		pseudo_t p;
		
		FOR_EACH_PTR(reg->contains, p) {
			if (CURRENT_TAG(p) & TAG_DEAD) {
				DELETE_CURRENT_PTR(p);
				reg->dead--;
			}
		} END_FOR_EACH_PTR(p);
		PACK_PTR_LIST(&reg->contains);
		assert(!reg->dead);
	}
}

static struct hardreg *target_copy_reg(SCTX_ struct bb_state *state, struct hardreg *src, pseudo_t target)
{
	kill_dead_reg(sctx_ src);
	return copy_reg(sctx_ state, src, target);
}

static void do_binop(SCTX_ struct bb_state *state, struct instruction *insn, pseudo_t val1, pseudo_t val2)
{
	const char *op = opcodes[insn->opcode];
	struct operand *src = get_register_operand(sctx_ state, val1, insn->target);
	struct operand *src2 = get_generic_operand(sctx_ state, val2);
	struct hardreg *dst;

	dst = target_copy_reg(sctx_ state, src->reg, insn->target);
	output_insn(sctx_ state, "%s.%d %s,%s", op, insn->size, show_op(sctx_ state, src2), dst->name);
	put_operand(sctx_ state, src);
	put_operand(sctx_ state, src2);
	add_pseudo_reg(sctx_ state, insn->target, dst);
}

static void generate_binop(SCTX_ struct bb_state *state, struct instruction *insn)
{
	flush_cc_cache(sctx_ state);
	do_binop(sctx_ state, insn, insn->src1, insn->src2);
}

static int is_dead_reg(SCTX_ struct bb_state *state, pseudo_t pseudo, struct hardreg *reg)
{
	pseudo_t p;
	FOR_EACH_PTR(reg->contains, p) {
		if (p == pseudo)
			return CURRENT_TAG(p) & TAG_DEAD;
	} END_FOR_EACH_PTR(p);
	return 0;
}

/*
 * Commutative binops are much more flexible, since we can switch the
 * sources around to satisfy the target register, or to avoid having
 * to load one of them into a register..
 */
static void generate_commutative_binop(SCTX_ struct bb_state *state, struct instruction *insn)
{
	pseudo_t src1, src2;
	struct hardreg *reg1, *reg2;

	flush_cc_cache(sctx_ state);
	src1 = insn->src1;
	src2 = insn->src2;
	reg2 = find_in_reg(sctx_ state, src2);
	if (!reg2)
		goto dont_switch;
	reg1 = find_in_reg(sctx_ state, src1);
	if (!reg1)
		goto do_switch;
	if (!is_dead_reg(sctx_ state, src2, reg2))
		goto dont_switch;
	if (!is_dead_reg(sctx_ state, src1, reg1))
		goto do_switch;

	/* Both are dead. Is one preferable? */
	if (reg2 != preferred_reg(sctx_ state, insn->target))
		goto dont_switch;

do_switch:
	src1 = src2;
	src2 = insn->src1;
dont_switch:
	do_binop(sctx_ state, insn, src1, src2);
}

/*
 * This marks a pseudo dead. It still stays on the hardreg list (the hardreg
 * still has its value), but it's scheduled to be killed after the next
 * "sequence point" when we call "kill_read_pseudos()"
 */
static void mark_pseudo_dead(SCTX_ struct bb_state *state, pseudo_t pseudo)
{
	int i;
	struct storage_hash *src;

	if (state->cc_target == pseudo)
		state->cc_dead = 1;
	src = find_pseudo_storage(sctx_ state, pseudo, NULL);
	if (src)
		src->flags |= TAG_DEAD;
	for (i = 0; i < REGNO; i++)
		mark_reg_dead(sctx_ state, pseudo, hardregs + i);
}

static void kill_dead_pseudos(SCTX_ struct bb_state *state)
{
	int i;

	for (i = 0; i < REGNO; i++) {
		kill_dead_reg(sctx_ hardregs + i);
	}
}

static void generate_store(SCTX_ struct instruction *insn, struct bb_state *state)
{
	output_insn(sctx_ state, "mov.%d %s,%s", insn->size, reg_or_imm(sctx_ state, insn->target), address(sctx_ state, insn));
}

static void generate_load(SCTX_ struct instruction *insn, struct bb_state *state)
{
	const char *input = address(sctx_ state, insn);
	struct hardreg *dst;

	kill_dead_pseudos(sctx_ state);
	dst = target_reg(sctx_ state, insn->target, NULL);
	output_insn(sctx_ state, "mov.%d %s,%s", insn->size, input, dst->name);
}

static void kill_pseudo(SCTX_ struct bb_state *state, pseudo_t pseudo)
{
	int i;
	struct hardreg *reg;

	output_comment(sctx_ state, "killing pseudo %s", show_pseudo(sctx_ pseudo));
	for (i = 0; i < REGNO; i++) {
		pseudo_t p;

		reg = hardregs + i;
		FOR_EACH_PTR(reg->contains, p) {
			if (p != pseudo)
				continue;
			if (CURRENT_TAG(p) & TAG_DEAD)
				reg->dead--;
			output_comment(sctx_ state, "removing pseudo %s from reg %s", 
				show_pseudo(sctx_ pseudo), reg->name);
			DELETE_CURRENT_PTR(p);
		} END_FOR_EACH_PTR(p);
		PACK_PTR_LIST(&reg->contains);
	}
}

static void generate_copy(SCTX_ struct bb_state *state, struct instruction *insn)
{
	struct hardreg *src = getreg(sctx_ state, insn->src, insn->target);
	kill_pseudo(sctx_ state, insn->target);
	add_pseudo_reg(sctx_ state, insn->target, src);
}

static void generate_cast(SCTX_ struct bb_state *state, struct instruction *insn)
{
	struct hardreg *src = getreg(sctx_ state, insn->src, insn->target);
	struct hardreg *dst;
	unsigned int old = insn->orig_type ? insn->orig_type->bit_size : 0;
	unsigned int new = insn->size;

	/*
	 * Cast to smaller type? Ignore the high bits, we
	 * just keep both pseudos in the same register.
	 */
	if (old >= new) {
		add_pseudo_reg(sctx_ state, insn->target, src);
		return;
	}

	dst = target_copy_reg(sctx_ state, src, insn->target);

	if (insn->orig_type && (insn->orig_type->ctype.modifiers & MOD_SIGNED)) {
		output_insn(sctx_ state, "sext.%d.%d %s", old, new, dst->name);
	} else {
		unsigned long long mask;
		mask = ~(~0ULL << old);
		mask &= ~(~0ULL << new);
		output_insn(sctx_ state, "andl.%d $%#llx,%s", insn->size, mask, dst->name);
	}
	add_pseudo_reg(sctx_ state, insn->target, dst);
}

static void generate_output_storage(SCTX_ struct bb_state *state);

static const char *conditional[] = {
	[OP_SET_EQ] = "e",
	[OP_SET_NE] = "ne",
	[OP_SET_LE] = "le",
	[OP_SET_GE] = "ge",
	[OP_SET_LT] = "lt",
	[OP_SET_GT] = "gt",
	[OP_SET_B] = "b",
	[OP_SET_A] = "a",
	[OP_SET_BE] = "be",
	[OP_SET_AE] = "ae"
};
	

static void generate_branch(SCTX_ struct bb_state *state, struct instruction *br)
{
	const char *cond = "XXX";
	struct basic_block *target;

	if (br->cond) {
		if (state->cc_target == br->cond) {
			cond = conditional[state->cc_opcode];
		} else {
			struct hardreg *reg = getreg(sctx_ state, br->cond, NULL);
			output_insn(sctx_ state, "testl %s,%s", reg->name, reg->name);
			cond = "ne";
		}
	}
	generate_output_storage(sctx_ state);
	target = br->bb_true;
	if (br->cond) {
		output_insn(sctx_ state, "j%s .L%p", cond, target);
		target = br->bb_false;
	}
	output_insn(sctx_ state, "jmp .L%p", target);
}

/* We've made sure that there is a dummy reg live for the output */
static void generate_switch(SCTX_ struct bb_state *state, struct instruction *insn)
{
	struct hardreg *reg = hardregs + SWITCH_REG;

	generate_output_storage(sctx_ state);
	output_insn(sctx_ state, "switch on %s", reg->name);
	output_insn(sctx_ state, "unimplemented: %s", show_instruction(sctx_ insn));
}

static void generate_ret(SCTX_ struct bb_state *state, struct instruction *ret)
{
	if (ret->src && ret->src != VOID) {
		struct hardreg *wants = hardregs+0;
		struct hardreg *reg = getreg(sctx_ state, ret->src, NULL);
		if (reg != wants)
			output_insn(sctx_ state, "movl %s,%s", reg->name, wants->name);
	}
	output_insn(sctx_ state, "ret");
}

/*
 * Fake "call" linearization just as a taster..
 */
static void generate_call(SCTX_ struct bb_state *state, struct instruction *insn)
{
	int offset = 0;
	pseudo_t arg;

	FOR_EACH_PTR(insn->arguments, arg) {
		output_insn(sctx_ state, "pushl %s", generic(sctx_ state, arg));
		offset += 4;
	} END_FOR_EACH_PTR(arg);
	flush_reg(sctx_ state, hardregs+0);
	flush_reg(sctx_ state, hardregs+1);
	flush_reg(sctx_ state, hardregs+2);
	output_insn(sctx_ state, "call %s", show_pseudo(sctx_ insn->func));
	if (offset)
		output_insn(sctx_ state, "addl $%d,%%esp", offset);
	if (insn->target && insn->target != VOID)
		add_pseudo_reg(sctx_ state, insn->target, hardregs+0);
}

static void generate_select(SCTX_ struct bb_state *state, struct instruction *insn)
{
	const char *cond;
	struct hardreg *src1, *src2, *dst;

	src1 = getreg(sctx_ state, insn->src2, NULL);
	dst = copy_reg(sctx_ state, src1, insn->target);
	add_pseudo_reg(sctx_ state, insn->target, dst);
	src2 = getreg(sctx_ state, insn->src3, insn->target);

	if (state->cc_target == insn->src1) {
		cond = conditional[state->cc_opcode];
	} else {
		struct hardreg *reg = getreg(sctx_ state, insn->src1, NULL);
		output_insn(sctx_ state, "testl %s,%s", reg->name, reg->name);
		cond = "ne";
	}

	output_insn(sctx_ state, "sel%s %s,%s", cond, src2->name, dst->name);
}

struct asm_arg {
	const struct ident *name;
	const char *value;
	pseudo_t pseudo;
	struct hardreg *reg;
};

static void replace_asm_arg(SCTX_ char **dst_p, struct asm_arg *arg)
{
	char *dst = *dst_p;
	int len = strlen(arg->value);

	memcpy(dst, arg->value, len);
	*dst_p = dst + len;
}

static void replace_asm_percent(SCTX_ const char **src_p, char **dst_p, struct asm_arg *args, int nr)
{
	const char *src = *src_p;
	char c;
	int index;

	c = *src++;
	switch (c) {
	case '0' ... '9':
		index = c - '0';
		if (index < nr)
			replace_asm_arg(sctx_ dst_p, args+index);
		break;
	}	
	*src_p = src;
	return;
}

static void replace_asm_named(SCTX_ const char **src_p, char **dst_p, struct asm_arg *args, int nr)
{
	const char *src = *src_p;
	const char *end = src;

	for(;;) {
		char c = *end++;
		if (!c)
			return;
		if (c == ']') {
			int i;

			*src_p = end;
			for (i = 0; i < nr; i++) {
				const struct ident *ident = args[i].name;
				int len;
				if (!ident)
					continue;
				len = ident->len;
				if (memcmp(src, ident->name, len))
					continue;
				replace_asm_arg(sctx_ dst_p, args+i);
				return;
			}
		}
	}
}

static const char *replace_asm_args(SCTX_ const char *str, struct asm_arg *args, int nr)
{
	static char buffer[1000];
	char *p = buffer;

	for (;;) {
		char c = *str;
		*p = c;
		if (!c)
			return buffer;
		str++;
		switch (c) {
		case '%':
			if (*str == '%') {
				str++;
				p++;
				continue;
			}
			replace_asm_percent(sctx_ &str, &p, args, nr);
			continue;
		case '[':
			replace_asm_named(sctx_ &str, &p, args, nr);
			continue;
		default:
			break;
		}
		p++;
	}
}

#define MAX_ASM_ARG (50)
static struct asm_arg asm_arguments[MAX_ASM_ARG];

static struct asm_arg *generate_asm_inputs(SCTX_ struct bb_state *state, struct asm_constraint_list *list, struct asm_arg *arg)
{
	struct asm_constraint *entry;

	FOR_EACH_PTR(list, entry) {
		const char *constraint = entry->constraint;
		pseudo_t pseudo = entry->pseudo;
		struct hardreg *reg, *orig;
		const char *string;
		int index;

		string = "undef";
		switch (*constraint) {
		case 'r':
			string = getreg(sctx_ state, pseudo, NULL)->name;
			break;
		case '0' ... '9':
			index = *constraint - '0';
			reg = asm_arguments[index].reg;
			orig = find_in_reg(sctx_ state, pseudo);
			if (orig)
				move_reg(sctx_ state, orig, reg);
			else
				fill_reg(sctx_ state, reg, pseudo);
			string = reg->name;
			break;
		default:
			string = generic(sctx_ state, pseudo);
			break;
		}

		output_insn(sctx_ state, "# asm input \"%s\": %s : %s", constraint, show_pseudo(sctx_ pseudo), string);

		arg->name = entry->ident;
		arg->value = string;
		arg->pseudo = NULL;
		arg->reg = NULL;
		arg++;
	} END_FOR_EACH_PTR(entry);
	return arg;
}

static struct asm_arg *generate_asm_outputs(SCTX_ struct bb_state *state, struct asm_constraint_list *list, struct asm_arg *arg)
{
	struct asm_constraint *entry;

	FOR_EACH_PTR(list, entry) {
		const char *constraint = entry->constraint;
		pseudo_t pseudo = entry->pseudo;
		struct hardreg *reg;
		const char *string;

		while (*constraint == '=' || *constraint == '+')
			constraint++;

		string = "undef";
		switch (*constraint) {
		case 'r':
		default:
			reg = target_reg(sctx_ state, pseudo, NULL);
			arg->pseudo = pseudo;
			arg->reg = reg;
			string = reg->name;
			break;
		}

		output_insn(sctx_ state, "# asm output \"%s\": %s : %s", constraint, show_pseudo(sctx_ pseudo), string);

		arg->name = entry->ident;
		arg->value = string;
		arg++;
	} END_FOR_EACH_PTR(entry);
	return arg;
}

static void generate_asm(SCTX_ struct bb_state *state, struct instruction *insn)
{
	const char *str = insn->string;

	if (insn->asm_rules->outputs || insn->asm_rules->inputs) {
		struct asm_arg *arg;

		arg = generate_asm_outputs(sctx_ state, insn->asm_rules->outputs, asm_arguments);
		arg = generate_asm_inputs(sctx_ state, insn->asm_rules->inputs, arg);
		str = replace_asm_args(sctx_ str, asm_arguments, arg - asm_arguments);
	}
	output_insn(sctx_ state, "%s", str);
}

static void generate_compare(SCTX_ struct bb_state *state, struct instruction *insn)
{
	struct hardreg *src;
	const char *src2;
	int opcode;

	flush_cc_cache(sctx_ state);
	opcode = insn->opcode;

	/*
	 * We should try to switch these around if necessary,
	 * and update the opcode to match..
	 */
	src = getreg(sctx_ state, insn->src1, insn->target);
	src2 = generic(sctx_ state, insn->src2);

	output_insn(sctx_ state, "cmp.%d %s,%s", insn->size, src2, src->name);

	add_cc_cache(sctx_ state, opcode, insn->target);
}

static void generate_one_insn(SCTX_ struct instruction *insn, struct bb_state *state)
{
	if (sctxp verbose)
		output_comment(sctx_ state, "%s", show_instruction(sctx_ insn));

	switch (insn->opcode) {
	case OP_ENTRY: {
		struct symbol *sym = insn->bb->ep->name;
		const char *name = show_ident(sctx_ sym->ident);
		if (sym->ctype.modifiers & MOD_STATIC)
			printf("\n\n%s:\n", name);
		else
			printf("\n\n.globl %s\n%s:\n", name, name);
		break;
	}

	/*
	 * OP_SETVAL likewise doesn't actually generate any
	 * code. On use, the "def" of the pseudo will be
	 * looked up.
	 */
	case OP_SETVAL:
		break;

	case OP_STORE:
		generate_store(sctx_ insn, state);
		break;

	case OP_LOAD:
		generate_load(sctx_ insn, state);
		break;

	case OP_DEATHNOTE:
		mark_pseudo_dead(sctx_ state, insn->target);
		return;

	case OP_COPY:
		generate_copy(sctx_ state, insn);
		break;

	case OP_ADD_LIN: case OP_MULU: case OP_MULS:
	case OP_AND_LIN: case OP_OR_LIN: case OP_XOR_LIN:
	case OP_AND_BOOL: case OP_OR_BOOL:
		generate_commutative_binop(sctx_ state, insn);
		break;

	case OP_SUB: case OP_DIVU: case OP_DIVS:
	case OP_MODU: case OP_MODS:
	case OP_SHL: case OP_LSR: case OP_ASR:
 		generate_binop(sctx_ state, insn);
		break;

	case OP_BINCMP ... OP_BINCMP_END:
		generate_compare(sctx_ state, insn);
		break;

	case OP_CAST: case OP_SCAST: case OP_FPCAST: case OP_PTRCAST:
		generate_cast(sctx_ state, insn);
		break;

	case OP_SEL:
		generate_select(sctx_ state, insn);
		break;

	case OP_BR:
		generate_branch(sctx_ state, insn);
		break;

	case OP_SWITCH:
		generate_switch(sctx_ state, insn);
		break;

	case OP_CALL:
		generate_call(sctx_ state, insn);
		break;

	case OP_RET:
		generate_ret(sctx_ state, insn);
		break;

	case OP_ASM:
		generate_asm(sctx_ state, insn);
		break;

	case OP_PHI:
	case OP_PHISOURCE:
	default:
		output_insn(sctx_ state, "unimplemented: %s", show_instruction(sctx_ insn));
		break;
	}
	kill_dead_pseudos(sctx_ state);
}

#define VERY_BUSY 1000
#define REG_FIXED 2000

static void write_reg_to_storage(SCTX_ struct bb_state *state, struct hardreg *reg, pseudo_t pseudo, struct storage *storage)
{
	int i;
	struct hardreg *out;

	switch (storage->type) {
	case REG_REG:
		out = hardregs + storage->regno;
		if (reg == out)
			return;
		output_insn(sctx_ state, "movl %s,%s", reg->name, out->name);
		return;
	case REG_UDEF:
		if (reg->busy < VERY_BUSY) {
			storage->type = REG_REG;
			storage->regno = reg - hardregs;
			reg->busy = REG_FIXED;
			return;
		}

		/* Try to find a non-busy register.. */
		for (i = 0; i < REGNO; i++) {
			out = hardregs + i;
			if (out->contains)
				continue;
			output_insn(sctx_ state, "movl %s,%s", reg->name, out->name);
			storage->type = REG_REG;
			storage->regno = i;
			out->busy = REG_FIXED;
			return;
		}

		/* Fall back on stack allocation ... */
		alloc_stack(sctx_ state, storage);
		/* Fall through */
	default:
		output_insn(sctx_ state, "movl %s,%s", reg->name, show_memop(sctx_ storage));
		return;
	}
}

static void write_val_to_storage(SCTX_ struct bb_state *state, pseudo_t src, struct storage *storage)
{
	struct hardreg *out;

	switch (storage->type) {
	case REG_UDEF:
		alloc_stack(sctx_ state, storage);
	default:
		output_insn(sctx_ state, "movl %s,%s", show_pseudo(sctx_ src), show_memop(sctx_ storage));
		break;
	case REG_REG:
		out = hardregs + storage->regno;
		output_insn(sctx_ state, "movl %s,%s", show_pseudo(sctx_ src), out->name);
	}
}

static void fill_output(SCTX_ struct bb_state *state, pseudo_t pseudo, struct storage *out)
{
	int i;
	struct storage_hash *in;
	struct instruction *def;

	/* Is that pseudo a constant value? */
	switch (pseudo->type) {
	case PSEUDO_VAL:
		write_val_to_storage(sctx_ state, pseudo, out);
		return;
	case PSEUDO_REG:
		def = pseudo->def;
		if (def && def->opcode == OP_SETVAL) {
			write_val_to_storage(sctx_ state, pseudo, out);
			return;
		}
	default:
		break;
	}

	/* See if we have that pseudo in a register.. */
	for (i = 0; i < REGNO; i++) {
		struct hardreg *reg = hardregs + i;
		pseudo_t p;

		FOR_EACH_PTR(reg->contains, p) {
			if (p == pseudo) {
				write_reg_to_storage(sctx_ state, reg, pseudo, out);
				return;
			}
		} END_FOR_EACH_PTR(p);
	}

	/* Do we have it in another storage? */
	in = find_storage_hash(sctx_ pseudo, state->internal);
	if (!in) {
		in = find_storage_hash(sctx_ pseudo, state->inputs);
		/* Undefined? */
		if (!in)
			return;
	}
	switch (out->type) {
	case REG_UDEF:
		*out = *in->storage;
		break;
	case REG_REG:
		output_insn(sctx_ state, "movl %s,%s", show_memop(sctx_ in->storage), hardregs[out->regno].name);
		break;
	default:
		if (out == in->storage)
			break;
		if ((out->type == in->storage->type) && (out->regno == in->storage->regno))
			break;
		output_insn(sctx_ state, "movl %s,%s", show_memop(sctx_ in->storage), show_memop(sctx_ out));
		break;
	}
	return;
}

static int final_pseudo_flush(SCTX_ struct bb_state *state, pseudo_t pseudo, struct hardreg *reg)
{
	struct storage_hash *hash;
	struct storage *out;
	struct hardreg *dst;

	/*
	 * Since this pseudo is live at exit, we'd better have output 
	 * storage for it..
	 */
	hash = find_storage_hash(sctx_ pseudo, state->outputs);
	if (!hash)
		return 1;
	out = hash->storage;

	/* If the output is in a register, try to get it there.. */
	if (out->type == REG_REG) {
		dst = hardregs + out->regno;
		/*
		 * Two good cases: nobody is using the right register,
		 * or we've already set it aside for output..
		 */
		if (!dst->contains || dst->busy > VERY_BUSY)
			goto copy_to_dst;

		/* Aiee. Try to keep it in a register.. */
		dst = empty_reg(sctx_ state);
		if (dst)
			goto copy_to_dst;

		return 0;
	}

	/* If the output is undefined, let's see if we can put it in a register.. */
	if (out->type == REG_UDEF) {
		dst = empty_reg(sctx_ state);
		if (dst) {
			out->type = REG_REG;
			out->regno = dst - hardregs;
			goto copy_to_dst;
		}
		/* Uhhuh. Not so good. No empty registers right now */
		return 0;
	}

	/* If we know we need to flush it, just do so already .. */
	output_insn(sctx_ state, "movl %s,%s", reg->name, show_memop(sctx_ out));
	return 1;

copy_to_dst:
	if (reg == dst)
		return 1;
	output_insn(sctx_ state, "movl %s,%s", reg->name, dst->name);
	add_pseudo_reg(sctx_ state, pseudo, dst);
	return 1;
}

/*
 * This tries to make sure that we put all the pseudos that are
 * live on exit into the proper storage
 */
static void generate_output_storage(SCTX_ struct bb_state *state)
{
	struct storage_hash *entry;

	/* Go through the fixed outputs, making sure we have those regs free */
	FOR_EACH_PTR(state->outputs, entry) {
		struct storage *out = entry->storage;
		if (out->type == REG_REG) {
			struct hardreg *reg = hardregs + out->regno;
			pseudo_t p;
			int flushme = 0;

			reg->busy = REG_FIXED;
			FOR_EACH_PTR(reg->contains, p) {
				if (p == entry->pseudo) {
					flushme = -100;
					continue;
				}
				if (CURRENT_TAG(p) & TAG_DEAD)
					continue;

				/* Try to write back the pseudo to where it should go ... */
				if (final_pseudo_flush(sctx_ state, p, reg)) {
					DELETE_CURRENT_PTR(p);
					continue;
				}
				flushme++;
			} END_FOR_EACH_PTR(p);
			PACK_PTR_LIST(&reg->contains);
			if (flushme > 0)
				flush_reg(sctx_ state, reg);
		}
	} END_FOR_EACH_PTR(entry);

	FOR_EACH_PTR(state->outputs, entry) {
		fill_output(sctx_ state, entry->pseudo, entry->storage);
	} END_FOR_EACH_PTR(entry);
}

static void generate(SCTX_ struct basic_block *bb, struct bb_state *state)
{
	int i;
	struct storage_hash *entry;
	struct instruction *insn;

	for (i = 0; i < REGNO; i++) {
		free_ptr_list(&hardregs[i].contains);
		hardregs[i].busy = 0;
		hardregs[i].dead = 0;
		hardregs[i].used = 0;
	}

	FOR_EACH_PTR(state->inputs, entry) {
		struct storage *storage = entry->storage;
		const char *name = show_storage(sctx_ storage);
		output_comment(sctx_ state, "incoming %s in %s", show_pseudo(sctx_ entry->pseudo), name);
		if (storage->type == REG_REG) {
			int regno = storage->regno;
			add_pseudo_reg(sctx_ state, entry->pseudo, hardregs + regno);
			name = hardregs[regno].name;
		}
	} END_FOR_EACH_PTR(entry);

	output_label(sctx_ state, ".L%p", bb);
	FOR_EACH_PTR(bb->insns, insn) {
		if (!insn->bb)
			continue;
		generate_one_insn(sctx_ insn, state);
	} END_FOR_EACH_PTR(insn);

	if (sctxp verbose) {
		output_comment(sctx_ state, "--- in ---");
		FOR_EACH_PTR(state->inputs, entry) {
			output_comment(sctx_ state, "%s <- %s", show_pseudo(sctx_ entry->pseudo), show_storage(sctx_ entry->storage));
		} END_FOR_EACH_PTR(entry);
		output_comment(sctx_ state, "--- spill ---");
		FOR_EACH_PTR(state->internal, entry) {
			output_comment(sctx_ state, "%s <-> %s", show_pseudo(sctx_ entry->pseudo), show_storage(sctx_ entry->storage));
		} END_FOR_EACH_PTR(entry);
		output_comment(sctx_ state, "--- out ---");
		FOR_EACH_PTR(state->outputs, entry) {
			output_comment(sctx_ state, "%s -> %s", show_pseudo(sctx_ entry->pseudo), show_storage(sctx_ entry->storage));
		} END_FOR_EACH_PTR(entry);
	}
	printf("\n");
}

static void generate_list(SCTX_ struct basic_block_list *list, unsigned long generation)
{
	struct basic_block *bb;
	FOR_EACH_PTR(list, bb) {
		if (bb->generation == generation)
			continue;
		output_bb(sctx_ bb, generation);
	} END_FOR_EACH_PTR(bb);
}

/*
 * Mark all the output registers of all the parents
 * as being "used" - this does not mean that we cannot
 * re-use them, but it means that we cannot ask the
 * parents to pass in another pseudo in one of those
 * registers that it already uses for another child.
 */
static void mark_used_registers(SCTX_ struct basic_block *bb, struct bb_state *state)
{
	struct basic_block *parent;

	FOR_EACH_PTR(bb->parents, parent) {
		struct storage_hash_list *outputs = gather_storage(sctx_ parent, STOR_OUT);
		struct storage_hash *entry;

		FOR_EACH_PTR(outputs, entry) {
			struct storage *s = entry->storage;
			if (s->type == REG_REG) {
				struct hardreg *reg = hardregs + s->regno;
				reg->used = 1;
			}
		} END_FOR_EACH_PTR(entry);
	} END_FOR_EACH_PTR(parent);
}

static void output_bb(SCTX_ struct basic_block *bb, unsigned long generation)
{
	struct bb_state state;

	bb->generation = generation;

	/* Make sure all parents have been generated first */
	generate_list(sctx_ bb->parents, generation);

	state.pos = bb->pos->pos;
	state.inputs = gather_storage(sctx_ bb, STOR_IN);
	state.outputs = gather_storage(sctx_ bb, STOR_OUT);
	state.internal = NULL;
	state.cc_opcode = 0;
	state.cc_target = NULL;

	/* Mark incoming registers used */
	mark_used_registers(sctx_ bb, &state);

	generate(sctx_ bb, &state);

	free_ptr_list(&state.inputs);
	free_ptr_list(&state.outputs);

	/* Generate all children... */
	generate_list(sctx_ bb->children, generation);
}

/*
 * We should set up argument sources here..
 *
 * Things like "first three arguments in registers" etc
 * are all for this place.
 *
 * On x86, we default to stack, unless it's a static
 * function that doesn't have its address taken.
 *
 * I should implement the -mregparm=X cmd line option.
 */
static void set_up_arch_entry(SCTX_ struct entrypoint *ep, struct instruction *entry)
{
	pseudo_t arg;
	struct symbol *sym, *argtype;
	int i, offset, regparm;

	sym = ep->name;
	regparm = 0;
	if (!(sym->ctype.modifiers & MOD_ADDRESSABLE))
		regparm = 3;
	sym = sym->ctype.base_type;
	i = 0;
	offset = 0;
	PREPARE_PTR_LIST(sym->arguments, argtype);
	FOR_EACH_PTR(entry->arg_list, arg) {
		struct storage *in = lookup_storage(sctx_ entry->bb, arg, STOR_IN);
		if (!in) {
			in = alloc_storage(sctx);
			add_storage(sctx_ in, entry->bb, arg, STOR_IN);
		}
		if (i < regparm) {
			in->type = REG_REG;
			in->regno = i;
		} else {
			int bits = argtype ? argtype->bit_size : 0;

			if (bits < sctxp bits_in_int)
				bits = sctxp bits_in_int;

			in->type = REG_FRAME;
			in->offset = offset;
			
			offset += bits_to_bytes(sctx_ bits);
		}
		i++;
		NEXT_PTR_LIST(argtype);
	} END_FOR_EACH_PTR(arg);
	FINISH_PTR_LIST(argtype);
}

/*
 * Set up storage information for "return"
 *
 * Not strictly necessary, since the code generator will
 * certainly move the return value to the right register,
 * but it can help register allocation if the allocator
 * sees that the target register is going to return in %eax.
 */
static void set_up_arch_exit(SCTX_ struct basic_block *bb, struct instruction *ret)
{
	pseudo_t pseudo = ret->src;

	if (pseudo && pseudo != VOID) {
		struct storage *out = lookup_storage(sctx_ bb, pseudo, STOR_OUT);
		if (!out) {
			out = alloc_storage(sctx);
			add_storage(sctx_ out, bb, pseudo, STOR_OUT);
		}
		out->type = REG_REG;
		out->regno = 0;
	}
}

/*
 * Set up dummy/silly output storage information for a switch
 * instruction. We need to make sure that a register is available
 * when we generate code for switch, so force that by creating
 * a dummy output rule.
 */
static void set_up_arch_switch(SCTX_ struct basic_block *bb, struct instruction *insn)
{
	pseudo_t pseudo = insn->cond;
	struct storage *out = lookup_storage(sctx_ bb, pseudo, STOR_OUT);
	if (!out) {
		out = alloc_storage(sctx);
		add_storage(sctx_ out, bb, pseudo, STOR_OUT);
	}
	out->type = REG_REG;
	out->regno = SWITCH_REG;
}

static void arch_set_up_storage(SCTX_ struct entrypoint *ep)
{
	struct basic_block *bb;

	/* Argument storage etc.. */
	set_up_arch_entry(sctx_ ep, ep->entry);

	FOR_EACH_PTR(ep->bbs, bb) {
		struct instruction *insn = last_instruction(sctx_ bb->insns);
		if (!insn)
			continue;
		switch (insn->opcode) {
		case OP_RET:
			set_up_arch_exit(sctx_ bb, insn);
			break;
		case OP_SWITCH:
			set_up_arch_switch(sctx_ bb, insn);
			break;
		default:
			/* nothing */;
		}
	} END_FOR_EACH_PTR(bb);
}

static void output(SCTX_ struct entrypoint *ep)
{
	unsigned long generation = ++sctxp bb_generation;

	last_reg = -1;
	stack_offset = 0;

	/* Get rid of SSA form (phinodes etc) */
	unssa(sctx_ ep);

	/* Set up initial inter-bb storage links */
	set_up_storage(sctx_ ep);

	/* Architecture-specific storage rules.. */
	arch_set_up_storage(sctx_ ep);

	/* Show the results ... */
	output_bb(sctx_ ep->entry->bb, generation);

	/* Clear the storage hashes for the next function.. */
	free_storage(sctx );
}

static int compile(SCTX_ struct symbol_list *list)
{
	struct symbol *sym;
	FOR_EACH_PTR(list, sym) {
		struct entrypoint *ep;
		expand_symbol(sctx_ sym);
		ep = linearize_symbol(sctx_ sym);
		if (ep)
			output(sctx_ ep);
	} END_FOR_EACH_PTR(sym);
	
	return 0;
}

int main(int argc, char **argv)
{
	struct string_list *filelist = NULL;
	char *file; SPARSE_CTX_INIT

	compile(sctx_ sparse_initialize(sctx_ argc, argv, &filelist));
	sctxp dbg_dead = 1;
	FOR_EACH_PTR_NOTAG(filelist, file) {
		compile(sctx_ sparse(sctx_ file));
	} END_FOR_EACH_PTR_NOTAG(file);
	return 0;
}

/*
 * Register - track pseudo usage, maybe eventually try to do register
 * allocation.
 *
 * Copyright (C) 2004 Linus Torvalds
 */

#include <assert.h>

#include "parse.h"
#include "expression.h"
#include "linearize.h"
#include "flow.h"

static void phi_defines(SCTX_ struct instruction * phi_node, pseudo_t target,
	void (*defines)(SCTX_ struct basic_block *, struct instruction *, pseudo_t))
{
	pseudo_t phi;
	FOR_EACH_PTR(phi_node->phi_list, phi) {
		struct instruction *def;
		if (phi == VOID)
			continue;
		def = phi->def;
		if (!def || !def->bb)
			continue;
		if (def->opcode == OP_PHI) {
			phi_defines(sctx_ def, target, defines);
			continue;
		}
		defines(sctx_ def->bb, phi->def, target);
	} END_FOR_EACH_PTR(phi);
}

static void asm_liveness(SCTX_ struct basic_block *bb, struct instruction *insn,
	void (*def)(SCTX_ struct basic_block *, struct instruction *, pseudo_t),
	void (*use)(SCTX_ struct basic_block *, struct instruction *, pseudo_t))
{
	struct asm_constraint *entry;

	FOR_EACH_PTR(insn->asm_rules->inputs, entry) {
		use(sctx_ bb, insn, entry->pseudo);
	} END_FOR_EACH_PTR(entry);
		
	FOR_EACH_PTR(insn->asm_rules->outputs, entry) {
		def(sctx_ bb, insn, entry->pseudo);
	} END_FOR_EACH_PTR(entry);
}

static void track_instruction_usage(SCTX_ struct basic_block *bb, struct instruction *insn,
	void (*def)(SCTX_ struct basic_block *, struct instruction *, pseudo_t),
	void (*use)(SCTX_ struct basic_block *, struct instruction *, pseudo_t))
{
	pseudo_t pseudo;

	#define USES(x)		use(sctx_ bb, insn, insn->x)
	#define DEFINES(x)	def(sctx_ bb, insn, insn->x)

	switch (insn->opcode) {
	case OP_RET:
		USES(src);
		break;

	case OP_BR: case OP_SWITCH:
		USES(cond);
		break;

	case OP_COMPUTEDGOTO:
		USES(target);
		break;
	
	/* Binary */
	case OP_BINARY ... OP_BINARY_END:
	case OP_BINCMP ... OP_BINCMP_END:
		USES(src1); USES(src2); DEFINES(target);
		break;

	/* Uni */
	case OP_NOT_LIN: case OP_NEG:
		USES(src1); DEFINES(target);
		break;

	case OP_SEL:
		USES(src1); USES(src2); USES(src3); DEFINES(target);
		break;
	
	/* Memory */
	case OP_LOAD:
		USES(src); DEFINES(target);
		break;

	case OP_STORE:
		USES(src); USES(target);
		break;

	case OP_SETVAL:
		DEFINES(target);
		break;

	case OP_SYMADDR:
		USES(symbol); DEFINES(target);
		break;

	/* Other */
	case OP_PHI:
		/* Phi-nodes are "backwards" nodes. Their def doesn't matter */
		phi_defines(sctx_ insn, insn->target, def);
		break;

	case OP_PHISOURCE:
		/*
		 * We don't care about the phi-source define, they get set
		 * up and expanded by the OP_PHI
		 */
		USES(phi_src);
		break;

	case OP_CAST:
	case OP_SCAST:
	case OP_FPCAST:
	case OP_PTRCAST:
		USES(src); DEFINES(target);
		break;

	case OP_CALL:
		USES(func);
		if (insn->target != VOID)
			DEFINES(target);
		FOR_EACH_PTR(insn->arguments, pseudo) {
			use(sctx_ bb, insn, pseudo);
		} END_FOR_EACH_PTR(pseudo);
		break;

	case OP_SLICE:
		USES(base); DEFINES(target);
		break;

	case OP_ASM:
		asm_liveness(sctx_ bb, insn, def, use);
		break;

	case OP_RANGE_LIN:
		USES(src1); USES(src2); USES(src3);
		break;

	case OP_BADOP:
	case OP_INVOKE:
	case OP_UNWIND:
	case OP_MALLOC:
	case OP_FREE:
	case OP_ALLOCA:
	case OP_GET_ELEMENT_PTR:
	case OP_VANEXT:
	case OP_VAARG:
	case OP_SNOP:
	case OP_LNOP:
	case OP_NOP:
	case OP_CONTEXT:
		break;
	}
}

int pseudo_in_list(SCTX_ struct pseudo_list *list, pseudo_t pseudo)
{
	pseudo_t old;
	FOR_EACH_PTR(list,old) {
		if (old == pseudo)
			return 1;
	} END_FOR_EACH_PTR(old);   
	return 0;
}

#ifndef DO_CTX
static int liveness_changed;
#endif

static void add_pseudo_exclusive(SCTX_ struct pseudo_list **list, pseudo_t pseudo)
{
	if (!pseudo_in_list(sctx_ *list, pseudo)) {
		sctxp liveness_changed = 1;
		add_pseudo(sctx_ list, pseudo);
	}
}

static inline int trackable_pseudo(pseudo_t pseudo)
{
	return pseudo && (pseudo->type == PSEUDO_REG || pseudo->type == PSEUDO_ARG);
}

static void insn_uses(SCTX_ struct basic_block *bb, struct instruction *insn, pseudo_t pseudo)
{
	if (trackable_pseudo(pseudo)) {
		struct instruction *def = pseudo->def;
		if (pseudo->type != PSEUDO_REG || def->bb != bb || def->opcode == OP_PHI)
			add_pseudo_exclusive(sctx_ &bb->needs, pseudo);
	}
}

static void insn_defines(SCTX_ struct basic_block *bb, struct instruction *insn, pseudo_t pseudo)
{
	assert(trackable_pseudo(pseudo));
	add_pseudo(sctx_ &bb->defines, pseudo);
}

static void track_bb_liveness(SCTX_ struct basic_block *bb)
{
	pseudo_t needs;

	FOR_EACH_PTR(bb->needs, needs) {
		struct basic_block *parent;
		FOR_EACH_PTR(bb->parents, parent) {
			if (!pseudo_in_list(sctx_ parent->defines, needs)) {
				add_pseudo_exclusive(sctx_ &parent->needs, needs);
			}
		} END_FOR_EACH_PTR(parent);
	} END_FOR_EACH_PTR(needs);
}

/*
 * We need to clear the liveness information if we 
 * are going to re-run it.
 */
void clear_liveness(SCTX_ struct entrypoint *ep)
{
	struct basic_block *bb;

	FOR_EACH_PTR(ep->bbs, bb) {
		free_ptr_list(&bb->needs);
		free_ptr_list(&bb->defines);
	} END_FOR_EACH_PTR(bb);
}

/*
 * Track inter-bb pseudo liveness. The intra-bb case
 * is purely local information.
 */
void track_pseudo_liveness(SCTX_ struct entrypoint *ep)
{
	struct basic_block *bb;

	/* Add all the bb pseudo usage */
	FOR_EACH_PTR(ep->bbs, bb) {
		struct instruction *insn;
		FOR_EACH_PTR(bb->insns, insn) {
			if (!insn->bb)
				continue;
			assert(insn->bb == bb);
			track_instruction_usage(sctx_ bb, insn, insn_defines, insn_uses);
		} END_FOR_EACH_PTR(insn);
	} END_FOR_EACH_PTR(bb);

	/* Calculate liveness.. */
	do {
		sctxp liveness_changed = 0;
		FOR_EACH_PTR_REVERSE(ep->bbs, bb) {
			track_bb_liveness(sctx_ bb);
		} END_FOR_EACH_PTR_REVERSE(bb);
	} while (sctxp liveness_changed);

	/* Remove the pseudos from the "defines" list that are used internally */
	FOR_EACH_PTR(ep->bbs, bb) {
		pseudo_t def;
		FOR_EACH_PTR(bb->defines, def) {
			struct basic_block *child;
			FOR_EACH_PTR(bb->children, child) {
				if (pseudo_in_list(sctx_ child->needs, def))
					goto is_used;
			} END_FOR_EACH_PTR(child);
			DELETE_CURRENT_PTR(def);
is_used:
		;
		} END_FOR_EACH_PTR(def);
		PACK_PTR_LIST(&bb->defines);
	} END_FOR_EACH_PTR(bb);
}

static void merge_pseudo_list(SCTX_ struct pseudo_list *src, struct pseudo_list **dest)
{
	pseudo_t pseudo;
	FOR_EACH_PTR(src, pseudo) {
		add_pseudo_exclusive(sctx_ dest, pseudo);
	} END_FOR_EACH_PTR(pseudo);
}

void track_phi_uses(SCTX_ struct instruction *insn)
{
	pseudo_t phi;
	FOR_EACH_PTR(insn->phi_list, phi) {
		struct instruction *def;
		if (phi == VOID || !phi->def)
			continue;
		def = phi->def;
		assert(def->opcode == OP_PHISOURCE);
		add_ptr_list(&def->phi_users, insn);
	} END_FOR_EACH_PTR(phi);
}

static void track_bb_phi_uses(SCTX_ struct basic_block *bb)
{
	struct instruction *insn;
	FOR_EACH_PTR(bb->insns, insn) {
		if (insn->bb && insn->opcode == OP_PHI)
			track_phi_uses(sctx_ insn);
	} END_FOR_EACH_PTR(insn);
}

#ifndef DO_CTX
static struct pseudo_list **live_list;
static struct pseudo_list *dead_list;
#endif

static void death_def(SCTX_ struct basic_block *bb, struct instruction *insn, pseudo_t pseudo)
{
}

static void death_use(SCTX_ struct basic_block *bb, struct instruction *insn, pseudo_t pseudo)
{
	if (trackable_pseudo(pseudo) && !pseudo_in_list(sctx_ *sctxp live_list, pseudo)) {
		add_pseudo(sctx_ &sctxp dead_list, pseudo);
		add_pseudo(sctx_ sctxp live_list, pseudo);
	}
}

static void track_pseudo_death_bb(SCTX_ struct basic_block *bb)
{
	struct pseudo_list *live = NULL;
	struct basic_block *child;
	struct instruction *insn;

	FOR_EACH_PTR(bb->children, child) {
		merge_pseudo_list(sctx_ child->needs, &live);
	} END_FOR_EACH_PTR(child);

	sctxp live_list = &live;
	FOR_EACH_PTR_REVERSE(bb->insns, insn) {
		if (!insn->bb)
			continue;

		sctxp dead_list = NULL;
		track_instruction_usage(sctx_ bb, insn, death_def, death_use);
		if (sctxp dead_list) {
			pseudo_t dead;
			FOR_EACH_PTR(sctxp dead_list, dead) {
				struct instruction *deathnote = __alloc_instruction(sctx_ 0);
				deathnote->bb = bb;
				deathnote->opcode = OP_DEATHNOTE;
				deathnote->target = dead;
				INSERT_CURRENT(deathnote, insn);
			} END_FOR_EACH_PTR(dead);
			free_ptr_list(&sctxp dead_list);
		}
	} END_FOR_EACH_PTR_REVERSE(insn);
	free_ptr_list(&live);
}

void track_pseudo_death(SCTX_ struct entrypoint *ep)
{
	struct basic_block *bb;

	FOR_EACH_PTR(ep->bbs, bb) {
		track_bb_phi_uses(sctx_ bb);
	} END_FOR_EACH_PTR(bb);

	FOR_EACH_PTR(ep->bbs, bb) {
		track_pseudo_death_bb(sctx_ bb);
	} END_FOR_EACH_PTR(bb);
}

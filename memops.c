/*
 * memops - try to combine memory ops.
 *
 * Copyright (C) 2004 Linus Torvalds
 */

#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>

#include "parse.h"
#include "expression.h"
#include "linearize.h"
#include "simplify.h"
#include "flow.h"


static pseudo_t convert_to_phinode(struct basic_block *bb, struct instruction *insn, struct instruction_list *dominators)
{
	struct instruction *node = alloc_phi_node(bb, insn->type, insn->target->ident);
	struct instruction *dom;

	add_phi_node(bb, node);

	// fill the 'arguments'
	FOR_EACH_PTR(dominators, dom) {
		struct instruction *phisrc = alloc_phisrc(dom->target, dom->type);
		insert_last_instruction(dom->bb, phisrc);
		link_phi(node, phisrc);
	} END_FOR_EACH_PTR(dom);
	return node->target;
}

static void rewrite_load_instruction(struct instruction *insn, struct instruction_list *dominators)
{
	struct basic_block *bb = insn->bb;	// FIXME
	struct instruction *dom;
	pseudo_t new = NULL;

	/*
	 * Check for somewhat common case of duplicate
	 * phi nodes.
	 */
	FOR_EACH_PTR(dominators, dom) {
		if (!new) {
			new = dom->target;
		} else if (new != dom->target) {
			new = convert_to_phinode(bb, insn, dominators);
			goto end;
		}
	} END_FOR_EACH_PTR(dom);

	/*
	 * All the same pseudo: replace it.
	 */
end:
	replace_with_pseudo(insn, new);
	repeat_phase |= REPEAT_CSE;
}

static int find_dominating_parents(struct instruction *insn, struct basic_block *bb, struct instruction_list **dominators, int local)
{
	struct basic_block *parent;

	FOR_EACH_PTR(bb->parents, parent) {
		struct instruction *one;

		FOR_EACH_PTR_REVERSE(parent->insns, one) {
			int dominance;
			if (!one->bb)
				continue;
			dominance = dominates(insn, one, local);
			if (dominance < 0) {
				if (one->opcode == OP_LOAD)
					continue;
				return 0;
			}
			if (!dominance)
				continue;
			goto found_dominator;
		} END_FOR_EACH_PTR_REVERSE(one);

		if (parent->generation == bb->generation)
			continue;
		parent->generation = bb->generation;

		if (!find_dominating_parents(insn, parent, dominators, local))
			return 0;
		continue;

found_dominator:
		add_instruction(dominators, one);
	} END_FOR_EACH_PTR(parent);
	return 1;
}		

static int address_taken(pseudo_t pseudo)
{
	struct pseudo_user *pu;
	FOR_EACH_PTR(pseudo->users, pu) {
		struct instruction *insn = pu->insn;
		if (insn->bb && (insn->opcode != OP_LOAD && insn->opcode != OP_STORE))
			return 1;
		if (pu->userp != &insn->src)
			return 1;
	} END_FOR_EACH_PTR(pu);
	return 0;
}

static int local_pseudo(pseudo_t pseudo)
{
	return pseudo->type == PSEUDO_SYM
		&& !(pseudo->sym->ctype.modifiers & (MOD_STATIC | MOD_NONLOCAL))
		&& !address_taken(pseudo);
}

static bool compatible_loads(struct instruction *a, struct instruction *b)
{
	if (is_integral_type(a->type) && is_float_type(b->type))
		return false;
	if (is_float_type(a->type) && is_integral_type(b->type))
		return false;
	return true;
}

static void rewrite_dominated_load(struct instruction *insn)
{
	struct instruction_list *dominators = NULL;
	struct basic_block *bb = insn->bb;
	pseudo_t pseudo = insn->src;
	int local = local_pseudo(pseudo);

	bb->generation = ++bb_generation;
	if (find_dominating_parents(insn, bb, &dominators, local)) {
		/* This happens with initial assignments to structures etc.. */
		if (!dominators) {
			if (local) {
				assert(pseudo->type != PSEUDO_ARG);
				replace_with_pseudo(insn, value_pseudo(0));
			}
			return;
		}
		rewrite_load_instruction(insn, dominators);
	}
	free_ptr_list(&dominators);
}

static void simplify_loads(struct basic_block *bb)
{
	struct instruction_list *worklist = NULL;
	struct instruction *insn;

	FOR_EACH_PTR_REVERSE(bb->insns, insn) {
		if (!insn->bb)
			continue;
		if (insn->opcode == OP_LOAD) {
			struct instruction *dom;
			pseudo_t pseudo = insn->src;
			int local = local_pseudo(pseudo);

			if (insn->is_volatile)
				continue;

			if (!has_users(insn->target)) {
				kill_instruction(insn);
				continue;
			}

			RECURSE_PTR_REVERSE(insn, dom) {
				int dominance;
				if (!dom->bb)
					continue;
				dominance = dominates(insn, dom, local);
				if (dominance) {
					/* possible partial dominance? */
					if (dominance < 0)  {
						if (dom->opcode == OP_LOAD)
							continue;
						goto next_load;
					}
					if (!compatible_loads(insn, dom))
						goto next_load;
					/* Yeehaa! Found one! */
					replace_with_pseudo(insn, dom->target);
					goto next_load;
				}
			} END_FOR_EACH_PTR_REVERSE(dom);

			/* OK, go find the parents */
			add_instruction(&worklist, insn);
		}
next_load:
		/* Do the next one */;
	} END_FOR_EACH_PTR_REVERSE(insn);

	FOR_EACH_PTR(worklist, insn) {
		rewrite_dominated_load(insn);
	} END_FOR_EACH_PTR(insn);
	free_ptr_list(&worklist);
}

static bool try_to_kill_store(struct instruction *insn,
			     struct instruction *dom, int local)
{
	int dominance = dominates(insn, dom, local);

	if (dominance) {
		/* possible partial dominance? */
		if (dominance < 0)
			return false;
		if (insn->target == dom->target && insn->bb == dom->bb) {
			// found a memop which makes the store redundant
			kill_instruction_force(insn);
			return false;
		}
		if (dom->opcode == OP_LOAD)
			return false;
		if (dom->is_volatile)
			return false;
		/* Yeehaa! Found one! */
		kill_instruction_force(dom);
	}
	return true;
}

static void kill_dominated_stores(struct basic_block *bb)
{
	struct instruction *insn;

	FOR_EACH_PTR_REVERSE(bb->insns, insn) {
		if (!insn->bb)
			continue;
		if (insn->opcode == OP_STORE) {
			struct basic_block *par;
			struct instruction *dom;
			pseudo_t pseudo = insn->src;
			int local;

			if (!insn->type)
				continue;
			if (insn->is_volatile)
				continue;

			local = local_pseudo(pseudo);
			RECURSE_PTR_REVERSE(insn, dom) {
				if (!dom->bb)
					continue;
				if (!try_to_kill_store(insn, dom, local))
					goto next_store;
			} END_FOR_EACH_PTR_REVERSE(dom);

			/* OK, we should check the parents now */
			FOR_EACH_PTR(bb->parents, par) {

				if (bb_list_size(par->children) != 1)
					goto next_parent;
				FOR_EACH_PTR(par->insns, dom) {
					if (!dom->bb)
						continue;
					if (dom == insn)
						goto next_parent;
					if (!try_to_kill_store(insn, dom, local))
						goto next_parent;
				} END_FOR_EACH_PTR(dom);
next_parent:
				;
			} END_FOR_EACH_PTR(par);
		}
next_store:
		/* Do the next one */;
	} END_FOR_EACH_PTR_REVERSE(insn);
}

void simplify_memops(struct entrypoint *ep)
{
	struct basic_block *bb;
	pseudo_t pseudo;

	FOR_EACH_PTR_REVERSE(ep->bbs, bb) {
		simplify_loads(bb);
	} END_FOR_EACH_PTR_REVERSE(bb);

	FOR_EACH_PTR_REVERSE(ep->bbs, bb) {
		kill_dominated_stores(bb);
	} END_FOR_EACH_PTR_REVERSE(bb);

	FOR_EACH_PTR(ep->accesses, pseudo) {
		struct symbol *var = pseudo->sym;
		unsigned long mod;
		if (!var)
			continue;
		mod = var->ctype.modifiers;
		if (mod & (MOD_VOLATILE | MOD_NONLOCAL | MOD_STATIC))
			continue;
		kill_dead_stores(ep, pseudo, local_pseudo(pseudo));
	} END_FOR_EACH_PTR(pseudo);
}

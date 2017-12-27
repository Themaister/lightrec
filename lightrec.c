/*
 * Copyright (C) 2014 Paul Cercueil <paul@crapouillou.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "blockcache.h"
#include "debug.h"
#include "disassembler.h"
#include "emitter.h"
#include "lightrec.h"
#include "regcache.h"
#include "optimizer.h"

#include <lightning.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#define GENMASK(h, l) \
	(((~0UL) << (l)) & (~0UL >> (__WORDSIZE - 1 - (h))))

static void __segfault_cb(struct lightrec_state *state, u32 addr)
{
	lightrec_set_exit_flags(state, LIGHTREC_EXIT_SEGFAULT);
	ERROR("Segmentation fault in recompiled code: invalid "
			"load/store at address 0x%08x\n", addr);
}

static u32 lightrec_rw_ops(struct lightrec_state *state,
		const struct opcode *op, u32 addr, u32 data)
{
	const struct lightrec_hw_ops *ops = state->hw_ops;

	switch (op->i.op) {
	case OP_SB:
	case OP_META_SB:
		ops->sb(state, op, addr, (u8) data);
		return 0;
	case OP_SH:
	case OP_META_SH:
		ops->sh(state, op, addr, (u16) data);
		return 0;
	case OP_SWL:
	case OP_SWR:
	case OP_SW:
	case OP_META_SW:
		ops->sw(state, op, addr, data);
		return 0;
	case OP_LB:
	case OP_META_LB:
		return (s32) (s8) ops->lb(state, op, addr);
	case OP_LBU:
	case OP_META_LBU:
		return ops->lb(state, op, addr);
	case OP_LH:
	case OP_META_LH:
		return (s32) (s16) ops->lh(state, op, addr);
	case OP_LHU:
	case OP_META_LHU:
		return ops->lh(state, op, addr);
	case OP_LW:
	case OP_META_LW:
	default:
		return ops->lw(state, op, addr);
	}
}

u32 lightrec_rw(struct lightrec_state *state,
		const struct opcode *op, u32 addr, u32 data)
{
	unsigned int i;
	u32 kaddr;
	void *new_addr;
	u32 shift, mem_data, mask;

	addr += (s16) op->i.imm;
	kaddr = kunseg(addr);

	new_addr = base_addr(state, kaddr);

	if (!new_addr)
		return lightrec_rw_ops(state, op, addr, data);

	switch (op->i.op) {
	case OP_SB:
		*(u8 *) new_addr = (u8) data;
		lightrec_invalidate(state, kaddr, 1);
		return 0;
	case OP_SH:
		*(u16 *) new_addr = (u16) data;
		lightrec_invalidate(state, kaddr, 2);
		return 0;
	case OP_SWL:
		shift = kaddr & 3;
		mem_data = *(u32 *)((uintptr_t) new_addr & ~3);
		mask = GENMASK(31, (shift + 1) * 8);

		*(u32 *)((uintptr_t) new_addr & ~3) = (data >> ((3 - shift) * 8))
			| (mem_data & mask);
		lightrec_invalidate(state, kaddr & ~0x3, 4);
		return 0;
	case OP_SWR:
		shift = kaddr & 3;
		mem_data = *(u32 *)((uintptr_t) new_addr & ~3);
		mask = (1 << (shift * 8)) - 1;

		*(u32 *)((uintptr_t) new_addr & ~3) = (data << (shift * 8))
			| (mem_data & mask);
		lightrec_invalidate(state, kaddr & ~0x3, 4);
		return 0;
	case OP_SW:
		*(u32 *) new_addr = data;
		lightrec_invalidate(state, kaddr, 4);
		return 0;
	case OP_SWC2:
		if (!state->cop_ops || !state->cop_ops->mfc) {
			WARNING("Missing MFC callback!\n");
			return 0;
		}

		*(u32 *) new_addr = state->cop_ops->mfc(
				state, 2, op->i.rt);
		lightrec_invalidate(state, kaddr, 4);
		return 0;
	case OP_LB:
		return (s32) *(s8 *) new_addr;
	case OP_LBU:
		return *(u8 *) new_addr;
	case OP_LH:
		return (s32) *(s16 *) new_addr;
	case OP_LHU:
		return *(u16 *) new_addr;
	case OP_LWL:
		shift = kaddr & 3;
		mem_data = *(u32 *)((uintptr_t) new_addr & ~3);
		mask = (1 << (24 - shift * 8)) - 1;

		return (data & mask) | (mem_data << (24 - shift * 8));
	case OP_LWR:
		shift = kaddr & 3;
		mem_data = *(u32 *)((uintptr_t) new_addr & ~3);
		mask = GENMASK(31, 32 - shift * 8);

		return (data & mask) | (mem_data >> (shift * 8));
	case OP_LWC2:
		if (!state->cop_ops || !state->cop_ops->mtc) {
			WARNING("Missing MFC callback!\n");
			return 0;
		}

		state->cop_ops->mtc(state, 2,
				op->i.rt, *(u32 *) new_addr);
		return 0;
	case OP_LW:
	default:
		return *(u32 *) new_addr;
	}

	__segfault_cb(state, addr);
	return 0;
}

static struct block * get_block(struct lightrec_state *state, u32 pc)
{
	struct block *block = lightrec_find_block(state->block_cache, pc);

	if (block && lightrec_block_is_outdated(block)) {
		DEBUG("Block at PC 0x%08x is outdated!\n", block->pc);

		lightrec_unregister_block(state->block_cache, block);
		lightrec_free_block(block);
		block = NULL;
	}

	if (!block) {
		block = lightrec_recompile_block(state, pc);
		if (!block) {
			ERROR("Unable to recompile block at PC 0x%x\n", pc);
			return NULL;
		}

		lightrec_register_block(state->block_cache, block);
	}

	return block;
}

static struct block * get_next_block(struct lightrec_state *state)
{
	return get_block(state, state->next_pc);
}

static struct block * generate_wrapper_block(struct lightrec_state *state)
{
	struct block *block;
	jit_state_t *_jit;
	jit_node_t *to_end, *to_end2, *loop, *addr2;
	unsigned int i;
	u32 offset;

	block = malloc(sizeof(*block));
	if (!block)
		goto err_no_mem;

	_jit = jit_new_state();
	if (!_jit)
		goto err_free_block;

	jit_name("wrapper");
	jit_note(__FILE__, __LINE__);

	jit_prolog();
	jit_frame(256);

	jit_getarg(JIT_R0, jit_arg());

	/* Force all callee-saved registers to be pushed on the stack */
	for (i = 0; i < NUM_REGS; i++)
		jit_movr(JIT_V(i), JIT_V(i));

	/* Pass lightrec_state structure to blocks, using the last callee-saved
	 * register that Lightning provides */
	jit_movi(LIGHTREC_REG_STATE, (intptr_t) state);

	loop = jit_label();

	/* Call the block's code */
	jit_jmpr(JIT_R0);

	/* The block will jump here, with the number of cycles executed in
	 * JIT_R0 */
	addr2 = jit_indirect();

	/* Increment the cycle counter */
	offset = offsetof(struct lightrec_state, current_cycle);
	jit_ldxi_i(JIT_R1, LIGHTREC_REG_STATE, offset);
	jit_addr(JIT_R1, JIT_R1, JIT_R0);
	jit_stxi_i(offset, LIGHTREC_REG_STATE, JIT_R1);

	/* Jump to end if (state->exit_flags != LIGHTREC_EXIT_NORMAL ||
	 *		state->target_cycle < state->current_cycle) */
	jit_ldxi_i(JIT_R0, LIGHTREC_REG_STATE,
			offsetof(struct lightrec_state, target_cycle));
	jit_ldxi_i(JIT_R2, LIGHTREC_REG_STATE,
			offsetof(struct lightrec_state, exit_flags));
	jit_ltr_u(JIT_R0, JIT_R0, JIT_R1);
	jit_orr(JIT_R0, JIT_R0, JIT_R2);
	to_end = jit_bnei(JIT_R0, 0);

	/* Get the next block */
	jit_prepare();
	jit_pushargr(LIGHTREC_REG_STATE);
	jit_finishi(&get_next_block);
	jit_retval(JIT_R0);

	/* If we get NULL, jump to end */
	to_end2 = jit_beqi(JIT_R0, 0);

	offset = offsetof(struct lightrec_state, current);
	jit_stxi(offset, LIGHTREC_REG_STATE, JIT_R0);

	/* Load the next block function in JIT_R0 and loop */
	jit_ldxi(JIT_R0, JIT_R0, offsetof(struct block, function));
	jit_patch_at(jit_jmpi(), loop);

	/* When exiting, the recompiled code will jump to that address */
	jit_note(__FILE__, __LINE__);
	jit_patch(to_end2);
	jit_movi(JIT_R0, LIGHTREC_EXIT_SEGFAULT);
	jit_stxi_i(offsetof(struct lightrec_state, exit_flags),
			LIGHTREC_REG_STATE, JIT_R0);

	jit_patch(to_end);
	jit_epilog();

	block->state = state;
	block->_jit = _jit;
	block->function = jit_emit();
	block->opcode_list = NULL;

	state->eob_wrapper_func = jit_address(addr2);

#if (LOG_LEVEL >= DEBUG_L)
	DEBUG("Wrapper block:\n");
	jit_disassemble();
#endif

	/* We're done! */
	jit_clear_state();
	return block;

err_free_block:
	free(block);
err_no_mem:
	ERROR("Unable to compile wrapper: Out of memory\n");
	return NULL;
}

struct block * lightrec_recompile_block(struct lightrec_state *state, u32 pc)
{
	struct opcode *elm, *list;
	struct block *block;
	jit_state_t *_jit;
	bool skip_next = false;
	const u32 *code;
	u32 addr = kunseg(pc);

	code = base_addr(state, addr);

	block = malloc(sizeof(*block));
	if (!block)
		goto err_no_mem;

	list = lightrec_disassemble(code, &block->length);
	if (!list)
		goto err_free_block;

	_jit = jit_new_state();
	if (!_jit)
		goto err_free_list;

	lightrec_regcache_reset(state->reg_cache);

	block->pc = pc;
	block->kunseg_pc = addr;
	block->state = state;
	block->_jit = _jit;
	block->opcode_list = list;
	block->cycles = 0;
	block->code = code;
	block->hash = calculate_block_hash(block);

	lightrec_optimize(list);

	jit_prolog();
	jit_tramp(256);

	for (elm = list; elm; elm = SLIST_NEXT(elm, next)) {
		int ret;

		block->cycles += lightrec_cycles_of_opcode(elm);

		if (skip_next) {
			skip_next = false;
		} else if (elm->opcode) {
			ret = lightrec_rec_opcode(block, elm, pc);
			skip_next = ret == SKIP_DELAY_SLOT;
		}

		if (likely(!(elm->flags & LIGHTREC_SKIP_PC_UPDATE)))
			pc += 4;
	}

	jit_ret();
	jit_epilog();

	block->function = jit_emit();

#if (LOG_LEVEL >= DEBUG_L)
	DEBUG("Recompiling block at PC: 0x%x\n", block->pc);
	lightrec_print_disassembly(block);
	DEBUG("Lightrec generated:\n");
	jit_disassemble();
#endif
	jit_clear_state();
	return block;

err_free_list:
	lightrec_free_opcode_list(list);
err_free_block:
	free(block);
err_no_mem:
	ERROR("Unable to recompile block: Out of memory\n");
	return NULL;
}

u32 lightrec_execute(struct lightrec_state *state, u32 pc, u32 target_cycle)
{
	void (*func)(void *) = (void (*)(void *)) state->wrapper->function;
	struct block *block = get_block(state, pc);

	if (unlikely(!block)) {
		state->exit_flags = LIGHTREC_EXIT_SEGFAULT;
		return pc;
	}

	state->exit_flags = LIGHTREC_EXIT_NORMAL;
	state->current = block;

	/* Handle the cycle counter overflowing */
	if (unlikely(target_cycle < state->current_cycle))
		target_cycle = UINT_MAX;

	state->target_cycle = target_cycle;

	func((void *) block->function);
	return state->next_pc;
}

u32 lightrec_execute_one(struct lightrec_state *state, u32 pc)
{
	return lightrec_execute(state, pc, state->current_cycle);
}

void lightrec_free_block(struct block *block)
{
	lightrec_free_opcode_list(block->opcode_list);
	_jit_destroy_state(block->_jit);
	free(block);
}

struct lightrec_state * lightrec_init(char *argv0,
		const struct lightrec_hw_ops *hw_ops,
		const struct lightrec_cop_ops *cop_ops,
		void *ram, void *bios, void *scratchpad)
{
	struct lightrec_state *state;
	unsigned int i;

	init_jit(argv0);

	state = calloc(1, sizeof(*state));
	if (!state)
		goto err_finish_jit;

	state->block_cache = lightrec_blockcache_init();
	if (!state->block_cache)
		goto err_free_state;

	state->reg_cache = lightrec_regcache_init();
	if (!state->reg_cache)
		goto err_free_block_cache;

	/* TODO: calculate the best page shift */
	state->page_shift = 9;

	state->invalidation_table = calloc(
				(0x200000 >> state->page_shift) + 1,
				sizeof(u32));
	if (!state->invalidation_table)
		goto err_free_reg_cache;

	state->wrapper = generate_wrapper_block(state);
	if (!state->wrapper)
		goto err_free_invalidation_table;

	state->hw_ops = hw_ops;
	state->cop_ops = cop_ops;
	state->ram_addr = ram;
	state->bios_addr = bios;
	state->scratch_addr = scratchpad;

	return state;

err_free_invalidation_table:
	free(state->invalidation_table);
err_free_reg_cache:
	lightrec_free_regcache(state->reg_cache);
err_free_block_cache:
	lightrec_free_block_cache(state->block_cache);
err_free_state:
	free(state);
err_finish_jit:
	finish_jit();
	return NULL;
}

void lightrec_destroy(struct lightrec_state *state)
{
	unsigned int i;

	lightrec_free_regcache(state->reg_cache);
	lightrec_free_block_cache(state->block_cache);
	lightrec_free_block(state->wrapper);
	finish_jit();

	free(state->invalidation_table);
	free(state);
}

void lightrec_invalidate(struct lightrec_state *state, u32 addr, u32 len)
{
	u32 offset, count, kaddr = kunseg(addr);

	if (kaddr < 0x200000) {
		offset = kaddr >> state->page_shift;
		count = (len + (1 << state->page_shift) - 1) >> state->page_shift;

		while (count--)
			state->invalidation_table[offset++] = state->current_cycle;
	}
}

void lightrec_set_exit_flags(struct lightrec_state *state, u32 flags)
{
	state->exit_flags |= flags;
}

u32 lightrec_exit_flags(struct lightrec_state *state)
{
	return state->exit_flags;
}

void lightrec_dump_registers(struct lightrec_state *state, u32 regs[34])
{
	memcpy(regs, state->native_reg_cache, sizeof(state->native_reg_cache));
}

void lightrec_restore_registers(struct lightrec_state *state, u32 regs[34])
{
	memcpy(state->native_reg_cache, regs, sizeof(state->native_reg_cache));
}

u32 lightrec_current_cycle_count(const struct lightrec_state *state,
		const struct opcode *op)
{
	u32 cycles = state->current_cycle;

	if (op)
		cycles += lightrec_cycles_of_block(state->current, op);

	return cycles;
}

void lightrec_reset_cycle_count(struct lightrec_state *state, u32 cycles)
{
	state->current_cycle = cycles;
}

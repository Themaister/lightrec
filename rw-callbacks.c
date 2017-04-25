/*
 * Copyright (C) 2017 Paul Cercueil <paul@crapouillou.net>
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

#include "disassembler.h"
#include "lightrec-private.h"

#define GENMASK(h, l) \
	(((~0UL) << (l)) & (~0UL >> (__WORDSIZE - 1 - (h))))

void lightrec_lb(struct lightrec_state *state, const struct opcode *op)
{
	const struct lightrec_mem_map *map;
	u32 kaddr, addr, offset;
	u8 val;

	addr = state->native_reg_cache[op->i.rs] + (s16) op->i.imm;
	kaddr = kunseg(addr);
	map = lightrec_find_map(state, kaddr);

	if (unlikely(!map))
		return;

	offset = kaddr - map->pc;

	while (map->mirror_of)
		map = map->mirror_of;

	if (unlikely(map->ops))
		val = map->ops->lb(state, op, addr);
	else
		val = *(u8 *)((uintptr_t) map->address + offset);

	if (likely(op->i.rt > 0)) {
		if (op->i.op == OP_LBU)
			state->native_reg_cache[op->i.rt] = (u32) val;
		else
			state->native_reg_cache[op->i.rt] = (u32)(s32)(s8) val;
	}
}

void lightrec_lh(struct lightrec_state *state, const struct opcode *op)
{
	const struct lightrec_mem_map *map;
	u32 kaddr, addr, offset;
	u16 val;

	addr = state->native_reg_cache[op->i.rs] + (s16) op->i.imm;
	kaddr = kunseg(addr);
	map = lightrec_find_map(state, kaddr);

	if (unlikely(!map))
		return;

	offset = kaddr - map->pc;

	while (map->mirror_of)
		map = map->mirror_of;

	if (unlikely(map->ops))
		val = map->ops->lh(state, op, addr);
	else
		val = *(u16 *)((uintptr_t) map->address + offset);

	if (likely(op->i.rt > 0)) {
		if (op->i.op == OP_LHU)
			state->native_reg_cache[op->i.rt] = (u32) val;
		else
			state->native_reg_cache[op->i.rt] = (u32)(s32)(s16) val;
	}
}

void lightrec_lw(struct lightrec_state *state, const struct opcode *op)
{
	const struct lightrec_mem_map *map;
	u32 val, kaddr, addr, offset;

	addr = state->native_reg_cache[op->i.rs] + (s16) op->i.imm;
	kaddr = kunseg(addr);
	map = lightrec_find_map(state, kaddr);

	if (unlikely(!map))
		return;

	offset = kaddr - map->pc;

	while (map->mirror_of)
		map = map->mirror_of;

	if (unlikely(map->ops))
		val = map->ops->lw(state, op, addr);
	else
		val = *(u32 *)((uintptr_t) map->address + offset);

	if (likely(op->i.rt > 0))
		state->native_reg_cache[op->i.rt] = val;
}

void lightrec_lwlr(struct lightrec_state *state, const struct opcode *op)
{
	const struct lightrec_mem_map *map;
	u32 val, kaddr, addr, offset;

	addr = state->native_reg_cache[op->i.rs] + (s16) op->i.imm;
	kaddr = kunseg(addr);
	map = lightrec_find_map(state, kaddr);

	if (unlikely(!map))
		return;

	offset = kaddr - map->pc;

	while (map->mirror_of)
		map = map->mirror_of;

	if (unlikely(map->ops)) {
		val = map->ops->lw(state, op, addr);
	} else {
		uintptr_t haddr = (uintptr_t) map->address + offset;
		u32 mem_data = *(u32 *)(haddr & ~3);
		u32 data = state->native_reg_cache[op->i.rt];
		u32 shift = kaddr & 3;
		u32 mask, mem_shift;

		if (op->i.op == OP_LWL) {
			mask = (1 << (24 - shift * 8)) - 1;
			mem_shift = 24 - shift * 8;
			val = (data & mask) | (mem_data << mem_shift);
		} else {
			mask = GENMASK(31, 32 - shift * 8);
			mem_shift = shift * 8;
			val = (data & mask) | (mem_data >> mem_shift);
		}
	}

	if (likely(op->i.rt > 0))
		state->native_reg_cache[op->i.rt] = val;
}

void lightrec_lwc2(struct lightrec_state *state, const struct opcode *op)
{
	const struct lightrec_mem_map *map;
	u32 val, kaddr, addr, offset;

	addr = state->native_reg_cache[op->i.rs] + (s16) op->i.imm;
	kaddr = kunseg(addr);
	map = lightrec_find_map(state, kaddr);

	if (unlikely(!map))
		return;

	offset = kaddr - map->pc;

	while (map->mirror_of)
		map = map->mirror_of;

	val = *(u32 *)((uintptr_t) map->address + offset);
	state->cop_ops->mtc(state, 2, op->i.rt, val);
}

void lightrec_sw(struct lightrec_state *state, const struct opcode *op)
{
	const struct lightrec_mem_map *map;
	u32 kaddr, addr, data, offset;

	addr = state->native_reg_cache[op->i.rs] + (s16) op->i.imm;
	data = state->native_reg_cache[op->i.rt];
	kaddr = kunseg(addr);
	map = lightrec_find_map(state, kaddr);

	if (unlikely(!map))
		return;

	offset = kaddr - map->pc;

	while (map->mirror_of)
		map = map->mirror_of;

	if (unlikely(map->ops)) {
		map->ops->sw(state, op, addr, data);
	} else {
		*(u32 *)((uintptr_t) map->address + offset) = data;
		lightrec_invalidate_map(state, map, kaddr, 4);
	}
}

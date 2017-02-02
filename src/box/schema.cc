/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdint.h>
#include "schema.h"
#include "user_def.h"
#include "engine.h"
#include "memtx_index.h"
#include "func.h"
#include "tuple.h"
#include "assoc.h"
#include "lua/utils.h"
#include "lua/space.h"
#include "key_def.h"
#include "alter.h"
#include "scoped_guard.h"
#include <stdio.h>
/**
 * @module Data Dictionary
 *
 * The data dictionary is responsible for storage and caching
 * of system metadata, such as information about existing
 * spaces, indexes, tuple formats. Space and index metadata
 * is called in dedicated spaces, _space and _index respectively.
 * The contents of these spaces is fully cached in a cache of
 * struct space objects.
 *
 * struct space is an in-memory instance representing a single
 * space with its metadata, space data, and methods to manage
 * it.
 */

/** All existing spaces. */
static struct mh_i32ptr_t *spaces;
static struct mh_i32ptr_t *funcs;
static struct mh_strnptr_t *funcs_by_name;
uint32_t sc_version;
/**
 * Lock of scheme modification
 */
struct latch schema_lock = LATCH_INITIALIZER(schema_lock);

bool
space_is_system(struct space *space)
{
	return space->def.id > BOX_SYSTEM_ID_MIN &&
		space->def.id < BOX_SYSTEM_ID_MAX;
}

/** Return space by its number */
struct space *
space_by_id(uint32_t id)
{
	mh_int_t space = mh_i32ptr_find(spaces, id, NULL);
	if (space == mh_end(spaces))
		return NULL;
	return (struct space *) mh_i32ptr_node(spaces, space)->val;
}

const char *
space_name_by_id(uint32_t id)
{
	struct space *space = space_by_id(id);
	return space ? space_name(space) : "";
}

/**
 * Visit all spaces and apply 'func'.
 */
void
space_foreach(void (*func)(struct space *sp, void *udata), void *udata)
{
	mh_int_t i;
	struct space *space;
	char key[6];
	assert(mp_sizeof_uint(BOX_SYSTEM_ID_MIN) <= sizeof(key));
	mp_encode_uint(key, BOX_SYSTEM_ID_MIN);

	/*
	 * Make sure we always visit system spaces first,
	 * in order from lowest space id to the highest..
	 * This is essential for correctly recovery from the
	 * snapshot, and harmless otherwise.
	 */
	space = space_by_id(BOX_SPACE_ID);
	Index *pk = space ? space_index(space, 0) : NULL;
	if (pk) {
		struct iterator *it = pk->allocIterator();
		auto scoped_guard = make_scoped_guard([=] { it->free(it); });
		pk->initIterator(it, ITER_GE, key, 1);

		struct tuple *tuple;
		while ((tuple = it->next(it))) {
			/* Get space id, primary key, field 0. */
			uint32_t id = tuple_field_u32_xc(tuple, 0);
			space = space_cache_find(id);
			if (! space_is_system(space))
				break;
			func(space, udata);
		}
	}

	mh_foreach(spaces, i) {
		space = (struct space *) mh_i32ptr_node(spaces, i)->val;
		if (space_is_system(space))
			continue;
		func(space, udata);
	}
}

/** Delete a space from the space cache and Lua. */
struct space *
space_cache_delete(uint32_t id)
{
	if (tarantool_L)
		box_lua_space_delete(tarantool_L, id);
	mh_int_t k = mh_i32ptr_find(spaces, id, NULL);
	assert(k != mh_end(spaces));
	struct space *space = (struct space *)mh_i32ptr_node(spaces, k)->val;
	mh_i32ptr_del(spaces, k, NULL);
	sc_version++;
	return space;
}

/**
 * Update the space in the space cache and in Lua. Returns
 * the old space instance, if any, or NULL if it's a new space.
 */
struct space *
space_cache_replace(struct space *space)
{
	const struct mh_i32ptr_node_t node = { space_id(space), space };
	struct mh_i32ptr_node_t old, *p_old = &old;
	mh_int_t k = mh_i32ptr_put(spaces, &node, &p_old, NULL);
	if (k == mh_end(spaces)) {
		panic_syserror("Out of memory for the data "
			       "dictionary cache.");
	}
	sc_version++;
	/*
	 * Must be after the space is put into the hash, since
	 * box.schema.space.bless() uses hash look up to find the
	 * space and create userdata objects for space objects.
	 */
	box_lua_space_new(tarantool_L, space);
	return p_old ? (struct space *) p_old->val : NULL;
}

/** A wrapper around space_new() for data dictionary spaces. */
struct space *
sc_space_new(struct space_def *space_def,
	     struct key_def *key_def,
	     struct trigger *trigger)
{
	struct rlist key_list;
	rlist_create(&key_list);
	rlist_add_entry(&key_list, key_def, link);
	struct space *space = space_new(space_def, &key_list);
	(void) space_cache_replace(space);
	if (trigger)
		trigger_add(&space->on_replace, trigger);
	/*
	 * Data dictionary spaces are fully built since:
	 * - they contain data right from the start
	 * - they are fully operable already during recovery
	 * - if there is a record in the snapshot which mandates
	 *   addition of a new index to a system space, this
	 *   index is built tuple-by-tuple, not in bulk, which
	 *   ensures validation of tuples when starting from
	 *   a snapshot of older version.
	 */
	space->handler->engine->initSystemSpace(space);
	return space;
}

int
schema_find_id(uint32_t system_space_id, uint32_t index_id,
	       const char *name, uint32_t len, uint32_t *out_id)
{
	*out_id = BOX_ID_NIL;
	char key[BOX_NAME_MAX * 2 + 1], *key_end;
	/**
	 * This is an internal-only method, we should know the
	 * max length in advance.
	 */
	if (len + 5 > sizeof(key))
		return 0;
	key_end = mp_encode_array(key, 1);
	key_end = mp_encode_str(key_end, name, len);

	box_iterator_t *it = box_index_iterator(system_space_id, index_id,
						ITER_EQ, key, key_end);
	if (it == NULL)
		return -1;

	struct tuple *tuple = NULL;

	if (box_iterator_next(it, &tuple) == -1)
		return -1;

	if (tuple) {
		/* id is always field #1 */
		const char *field = tuple_field(tuple, 0);
		if (field == NULL) {
			diag_set(ClientError, ER_NO_SUCH_FIELD, 0);
			return -1;
		}
		if (mp_typeof(*field) != MP_UINT) {
			diag_set(ClientError, ER_FIELD_TYPE,
				 0 + TUPLE_INDEX_BASE,
				 mp_type_strs[MP_UINT]);
			return -1;
		}
		uint64_t id = mp_decode_uint(&field);
		if (id > UINT32_MAX) {
			diag_set(ClientError, ER_FIELD_TYPE,
				 0 + TUPLE_INDEX_BASE,
				 field_type_strs[FIELD_TYPE_UNSIGNED]);
			return -1;
		}
		*out_id = (uint32_t )id;
	}
	return 0;
}

/**
 * Initialize a prototype for the two mandatory data
 * dictionary spaces and create a cache entry for them.
 * When restoring data from the snapshot these spaces
 * will get altered automatically to their actual format.
 */
void
schema_init()
{
	/* Initialize the space cache. */
	spaces = mh_i32ptr_new();
	funcs = mh_i32ptr_new();
	funcs_by_name = mh_strnptr_new();
	/*
	 * Create surrogate space objects for the mandatory system
	 * spaces (the primal eggs from which we get all the
	 * chicken). Their definitions will be overwritten by the
	 * data in the snapshot, and they will thus be
	 * *re-created* during recovery.  Note, the index type
	 * must be TREE and space identifiers must be the smallest
	 * one to ensure that these spaces are always recovered
	 * (and re-created) first.
	 */
	/* _schema - key/value space with schema description */
	struct space_def def = {
		BOX_SCHEMA_ID, ADMIN, 0, "_schema", "memtx", {false}
	};
	struct key_opts opts = key_opts_default;
	struct key_def *key_def = key_def_new(def.id,
					      0 /* index id */,
					      "primary", /* name */
					      TREE /* index type */,
					      &opts,
					      1); /* part count */
	if (key_def == NULL)
		diag_raise();
	key_def_set_part(key_def, 0 /* part no */, 0 /* field no */,
			 FIELD_TYPE_STRING);
	(void) sc_space_new(&def, key_def, &on_replace_schema);

	/* _space - home for all spaces. */
	key_def->space_id = def.id = BOX_SPACE_ID;
	snprintf(def.name, sizeof(def.name), "_space");
	key_def_set_part(key_def, 0 /* part no */, 0 /* field no */,
			 FIELD_TYPE_UNSIGNED);

	(void) sc_space_new(&def, key_def, &alter_space_on_replace_space);

	/* _user - all existing users */
	key_def->space_id = def.id = BOX_USER_ID;
	snprintf(def.name, sizeof(def.name), "_user");
	(void) sc_space_new(&def, key_def, &on_replace_user);

	/* _func - all executable objects on which one can have grants */
	key_def->space_id = def.id = BOX_FUNC_ID;
	snprintf(def.name, sizeof(def.name), "_func");
	(void) sc_space_new(&def, key_def, &on_replace_func);
	/*
	 * _priv - association user <-> object
	 * The real index is defined in the snapshot.
	 */
	key_def->space_id = def.id = BOX_PRIV_ID;
	snprintf(def.name, sizeof(def.name), "_priv");
	(void) sc_space_new(&def, key_def, &on_replace_priv);
	/*
	 * _cluster - association server uuid <-> server id
	 * The real index is defined in the snapshot.
	 */
	key_def->space_id = def.id = BOX_CLUSTER_ID;
	snprintf(def.name, sizeof(def.name), "_cluster");
	(void) sc_space_new(&def, key_def, &on_replace_cluster);
	key_def_delete(key_def);

	/* _index - definition of indexes in all spaces */
	def.id = BOX_INDEX_ID;
	snprintf(def.name, sizeof(def.name), "_index");
	key_def = key_def_new(def.id,
			      0 /* index id */,
			      "primary",
			      TREE /* index type */,
			      &opts,
			      2); /* part count */
	if (key_def == NULL)
		diag_raise();
	/* space no */
	key_def_set_part(key_def, 0 /* part no */, 0 /* field no */,
			 FIELD_TYPE_UNSIGNED);
	/* index no */
	key_def_set_part(key_def, 1 /* part no */, 1 /* field no */,
			 FIELD_TYPE_UNSIGNED);
	(void) sc_space_new(&def, key_def, &alter_space_on_replace_index);
	key_def_delete(key_def);
}

void
schema_free(void)
{
	if (spaces == NULL)
		return;
	while (mh_size(spaces) > 0) {
		mh_int_t i = mh_first(spaces);

		struct space *space = (struct space *)
				mh_i32ptr_node(spaces, i)->val;
		space_cache_delete(space_id(space));
		space_delete(space);
	}
	mh_i32ptr_delete(spaces);
	while (mh_size(funcs) > 0) {
		mh_int_t i = mh_first(funcs);

		struct func *func = ((struct func *)
				     mh_i32ptr_node(funcs, i)->val);
		func_cache_delete(func->def.fid);
	}
	mh_i32ptr_delete(funcs);
}

void
func_cache_replace(struct func_def *def)
{
	struct func *old = func_by_id(def->fid);
	if (old) {
		func_update(old, def);
		return;
	}
	if (mh_size(funcs) >= BOX_FUNCTION_MAX)
		tnt_raise(ClientError, ER_FUNCTION_MAX, BOX_FUNCTION_MAX);
	struct func *func = func_new(def);
	if (func == NULL) {
error:
		panic_syserror("Out of memory for the data "
			       "dictionary cache (stored function).");
	}
	const struct mh_i32ptr_node_t node = { def->fid, func };
	mh_int_t k1 = mh_i32ptr_put(funcs, &node, NULL, NULL);
	if (k1 == mh_end(funcs)) {
		func_delete(func);
		goto error;
	}
	size_t def_name_len = strlen(func->def.name);
	uint32_t name_hash = mh_strn_hash(func->def.name, def_name_len);
	const struct mh_strnptr_node_t strnode = {
		func->def.name, def_name_len, name_hash, func };

	mh_int_t k2 = mh_strnptr_put(funcs_by_name, &strnode, NULL, NULL);
	if (k2 == mh_end(funcs_by_name)) {
		mh_i32ptr_del(funcs, k1, NULL);
		func_delete(func);
		goto error;
	}
}

void
func_cache_delete(uint32_t fid)
{
	mh_int_t k = mh_i32ptr_find(funcs, fid, NULL);
	if (k == mh_end(funcs))
		return;
	struct func *func = (struct func *)
		mh_i32ptr_node(funcs, k)->val;
	mh_i32ptr_del(funcs, k, NULL);
	k = mh_strnptr_find_inp(funcs_by_name, func->def.name,
				strlen(func->def.name));
	if (k != mh_end(funcs))
		mh_strnptr_del(funcs_by_name, k, NULL);
	func_delete(func);
}

struct func *
func_by_id(uint32_t fid)
{
	mh_int_t func = mh_i32ptr_find(funcs, fid, NULL);
	if (func == mh_end(funcs))
		return NULL;
	return (struct func *) mh_i32ptr_node(funcs, func)->val;
}

struct func *
func_by_name(const char *name, uint32_t name_len)
{
	mh_int_t func = mh_strnptr_find_inp(funcs_by_name, name, name_len);
	if (func == mh_end(funcs_by_name))
		return NULL;
	return (struct func *) mh_strnptr_node(funcs_by_name, func)->val;
}

bool
schema_find_grants(const char *type, uint32_t id)
{
	struct space *priv = space_cache_find(BOX_PRIV_ID);
	/** "object" index */
	MemtxIndex *index = index_find_system(priv, 2);
	struct iterator *it = index->position();
	char key[10 + BOX_NAME_MAX];
	mp_encode_uint(mp_encode_str(key, type, strlen(type)), id);
	index->initIterator(it, ITER_EQ, key, 2);
	return it->next(it);
}

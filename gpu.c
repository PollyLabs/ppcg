/*
 * Copyright 2010-2011 INRIA Saclay
 * Copyright 2012-2013 Ecole Normale Superieure
 *
 * Use of this software is governed by the MIT license
 *
 * Written by Sven Verdoolaege, INRIA Saclay - Ile-de-France,
 * Parc Club Orsay Universite, ZAC des vignes, 4 rue Jacques Monod,
 * 91893 Orsay, France
 * and Ecole Normale Superieure, 45 rue d’Ulm, 75230 Paris, France
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <isl/polynomial.h>
#include <isl/union_set.h>
#include <isl/aff.h>
#include <isl/ilp.h>
#include <isl/flow.h>
#include <isl/schedule.h>
#include <isl/schedule_node.h>
#include <isl/options.h>
#include <isl/ast_build.h>

#include "cpu.h"
#include "gpu.h"
#include "gpu_array_tile.h"
#include "gpu_group.h"
#include "gpu_tree.h"
#include "schedule.h"
#include "ppcg_options.h"
#include "print.h"

struct gpu_array_info;

/* Collect all references to the given array and store pointers to them
 * in array->refs.
 *
 * If the array contains structures, then there is no need to collect
 * the references since we will not be computing any reference groups.
 */
static void collect_references(struct gpu_prog *prog,
	struct gpu_array_info *array)
{
	int i;
	int n;

	if (array->has_compound_element)
		return;

	n = 0;
	for (i = 0; i < prog->n_stmts; ++i) {
		struct gpu_stmt *stmt = &prog->stmts[i];
		struct gpu_stmt_access *access;

		for (access = stmt->accesses; access; access = access->next) {
			const char *name;
			name = isl_map_get_tuple_name(access->access,
						      isl_dim_out);
			if (name && !strcmp(array->name, name))
				n++;
		}
	}

	array->n_ref = n;
	array->refs = isl_alloc_array(prog->ctx, struct gpu_stmt_access *, n);
	assert(array->refs);

	n = 0;
	for (i = 0; i < prog->n_stmts; ++i) {
		struct gpu_stmt *stmt = &prog->stmts[i];
		struct gpu_stmt_access *access;

		for (access = stmt->accesses; access; access = access->next) {
			const char *name;
			name = isl_map_get_tuple_name(access->access,
						      isl_dim_out);
			if (!name || strcmp(array->name, name))
				continue;

			array->refs[n++] = access;
		}
	}
}

/* Compute and return the extent of "array", taking into account the set of
 * accessed elements.
 *
 * In particular, the extent in the outer dimension is taken
 * from "accessed", while the extents in the remaining dimensions
 * are taken from array->extent.
 *
 * The extent in the outer dimension cannot be taken from array->extent
 * because that may be unbounded.  Furthermore, even if it is bounded,
 * it may be larger than the piece of the array that is being accessed.
 */
static __isl_give isl_set *compute_extent(struct pet_array *array,
	__isl_keep isl_set *accessed)
{
	int n_index;
	isl_id *id;
	isl_set *outer;
	isl_set *extent;

	extent = isl_set_copy(array->extent);

	n_index = isl_set_dim(accessed, isl_dim_set);
	if (n_index == 0)
		return extent;

	extent = isl_set_project_out(extent, isl_dim_set, 0, 1);
	outer = isl_set_copy(accessed);
	outer = isl_set_project_out(outer, isl_dim_set, 1, n_index - 1);
	extent = isl_set_flat_product(outer, extent);
	id = isl_set_get_tuple_id(accessed);
	extent = isl_set_set_tuple_id(extent, id);

	return extent;
}

/* Is the array "array" being extracted a read-only scalar?
 *
 * That is, is "array" a scalar that is never possibly written to.
 * An array containing structures is never considered to be a scalar.
 */
static int is_read_only_scalar(struct gpu_array_info *array,
	struct gpu_prog *prog)
{
	isl_set *space;
	isl_union_map *write;
	int empty;

	if (array->has_compound_element)
		return 0;
	if (array->n_index != 0)
		return 0;

	write = isl_union_map_copy(prog->may_write);
	space = isl_set_universe(isl_space_copy(array->space));
	write = isl_union_map_intersect_range(write,
						isl_union_set_from_set(space));
	empty = isl_union_map_is_empty(write);
	isl_union_map_free(write);

	return empty;
}

/* Compute bounds on the host array "pa" based on the corresponding
 * accessed elements in "arrays"
 * and collect all references to the array.
 * Store the results in "info".
 *
 * If the array is zero-dimensional and does not contain structures,
 * i.e., if the array is a scalar, we check whether it is read-only.
 * We also check whether the array is accessed at all.
 */
static int extract_array_info(struct gpu_prog *prog,
	struct gpu_array_info *info, struct pet_array *pa,
	__isl_keep isl_union_set *arrays)
{
	int i, empty;
	const char *name;
	int n_index;
	isl_pw_aff **bounds;
	isl_set *accessed, *extent;

	n_index = isl_set_dim(pa->extent, isl_dim_set);
	name = isl_set_get_tuple_name(pa->extent);
	bounds = isl_alloc_array(prog->ctx, isl_pw_aff *, n_index);
	if (!bounds)
		return -1;

	info->space = isl_set_get_space(pa->extent);
	info->name = strdup(name);
	info->n_index = n_index;
	info->bound = bounds;
	info->linearize = prog->scop->options->linearize_device_arrays;

	info->type = strdup(pa->element_type);
	info->size = pa->element_size;
	info->local = pa->declared && !pa->exposed;
	info->has_compound_element = pa->element_is_record;
	info->read_only_scalar = is_read_only_scalar(info, prog);

	accessed = isl_union_set_extract_set(arrays,
					    isl_space_copy(info->space));
	empty = isl_set_is_empty(accessed);
	extent = compute_extent(pa, accessed);
	isl_set_free(accessed);
	info->extent = extent;
	if (empty < 0)
		return -1;
	info->accessed = !empty;
	for (i = 0; i < n_index; ++i) {
		isl_set *dom;
		isl_local_space *ls;
		isl_aff *one;
		isl_pw_aff *bound;

		dom = isl_set_copy(extent);
		dom = isl_set_project_out(dom, isl_dim_set, i + 1,
					    n_index - (i + 1));
		dom = isl_set_project_out(dom, isl_dim_set, 0, i);
		if (!isl_set_dim_has_upper_bound(dom, isl_dim_set, 0)) {
			fprintf(stderr, "unable to determine extent of '%s' "
				"in dimension %d\n", info->name, i);
			dom = isl_set_free(dom);
		}
		bound = isl_set_dim_max(dom, 0);
		dom = isl_pw_aff_domain(isl_pw_aff_copy(bound));
		ls = isl_local_space_from_space(isl_set_get_space(dom));
		one = isl_aff_zero_on_domain(ls);
		one = isl_aff_add_constant_si(one, 1);
		bound = isl_pw_aff_add(bound, isl_pw_aff_alloc(dom, one));
		bound = isl_pw_aff_gist(bound, isl_set_copy(prog->context));

		bounds[i] = bound;
		if (!isl_pw_aff_is_cst(bound))
			info->linearize = 1;
	}

	collect_references(prog, info);

	return 0;
}

/* Remove independence from the order constraints "order" on array "array".
 * Since the pairs of iterations in the filter relation of an independence
 * are guaranteed to be completely independent by the user, there is
 * no need to ensure that live ranges are ordered along thong pairs.
 * We make an exception for local variables, though, as the independence
 * guarantee does not apply to those.
 *
 * The order constraints are used in two places.
 * Those on scalars are used in check_scalar_live_ranges to check if
 * we need to force the scalar to be private.  Any non-local scalar
 * should not be forced scalar if it only appears in independent loops.
 * Those on non-scalars are added to the coincidence constraints
 * in compute_schedule because we do not support any array expansion.
 * Accesses to non-local arrays should not prevent a loop from being
 * considered coincident so we should indeed remove those constraints
 * from the order constraints.
 */
static __isl_give isl_union_map *remove_independences(struct gpu_prog *prog,
	struct gpu_array_info *array, __isl_take isl_union_map *order)
{
	int i;

	for (i = 0; i < prog->scop->pet->n_independence; ++i) {
		struct pet_independence *pi = prog->scop->pet->independences[i];
		if (isl_union_set_contains(pi->local, array->space))
			continue;

		order = isl_union_map_subtract(order,
						isl_union_map_copy(pi->filter));
	}

	return order;
}

/* For each array in "prog", store the (untagged) order dependences
 * derived from the array in array->dep_order.
 * In particular, consider all references that access the given array
 * and take the order dependences that have one of these references
 * as source.  (Since an order dependence relates two references to
 * the same array, the target of these order dependences will also
 * be one of these references.)
 * Additionally, store the union of these array->dep_order relations
 * for all non-scalar arrays in prog->array_order.
 */
void collect_order_dependences(struct gpu_prog *prog)
{
	int i;
	isl_space *space;
	isl_union_map *accesses;

	space = isl_union_map_get_space(prog->read);
	prog->array_order = isl_union_map_empty(space);

	accesses = isl_union_map_copy(prog->scop->tagged_reads);
	accesses = isl_union_map_union(accesses,
			    isl_union_map_copy(prog->scop->tagged_may_writes));
	accesses = isl_union_map_universe(accesses);
	accesses = isl_union_map_apply_range(accesses,
					    isl_union_map_copy(prog->to_outer));

	for (i = 0; i < prog->n_array; ++i) {
		struct gpu_array_info *array = &prog->array[i];
		isl_set *set;
		isl_union_set *uset;
		isl_union_map *order;

		set = isl_set_universe(isl_space_copy(array->space));
		uset = isl_union_set_from_set(set);
		uset = isl_union_map_domain(
		    isl_union_map_intersect_range(isl_union_map_copy(accesses),
						    uset));
		order = isl_union_map_copy(prog->scop->tagged_dep_order);
		order = isl_union_map_intersect_domain(order, uset);
		order = isl_union_map_zip(order);
		order = isl_union_set_unwrap(isl_union_map_domain(order));
		order = remove_independences(prog, array, order);
		array->dep_order = order;

		if (gpu_array_is_scalar(array) && !array->has_compound_element)
			continue;

		prog->array_order = isl_union_map_union(prog->array_order,
					isl_union_map_copy(array->dep_order));
	}

	isl_union_map_free(accesses);
}

/* Construct a gpu_array_info for each array referenced by prog->scop and
 * collect them in prog->array.
 *
 * The sizes are based on the extents and the set of possibly accessed
 * elements by "prog".
 * If there are any member accesses involved, then they are first mapped
 * to the outer arrays of structs.
 *
 * If we are allowing live range reordering, then also set
 * the dep_order field.  Otherwise leave it NULL.
 */
static int collect_array_info(struct gpu_prog *prog)
{
	int i;
	int r = 0;
	isl_union_set *arrays;

	arrays = isl_union_map_range(isl_union_map_copy(prog->read));
	arrays = isl_union_set_union(arrays,
		    isl_union_map_range(isl_union_map_copy(prog->may_write)));

	arrays = isl_union_set_apply(arrays,
					isl_union_map_copy(prog->to_outer));

	arrays = isl_union_set_coalesce(arrays);

	prog->n_array = prog->scop->pet->n_array;
	prog->array = isl_calloc_array(prog->ctx,
				     struct gpu_array_info, prog->n_array);
	assert(prog->array);
	for (i = 0; i < prog->scop->pet->n_array; ++i)
		if (extract_array_info(prog, &prog->array[i],
					prog->scop->pet->arrays[i], arrays) < 0)
			r = -1;

	isl_union_set_free(arrays);

	if (prog->scop->options->live_range_reordering)
		collect_order_dependences(prog);

	return r;
}

static void free_array_info(struct gpu_prog *prog)
{
	int i, j;

	for (i = 0; i < prog->n_array; ++i) {
		int n_index = prog->array[i].n_index;
		free(prog->array[i].type);
		free(prog->array[i].name);
		for (j = 0; j < n_index; ++j)
			isl_pw_aff_free(prog->array[i].bound[j]);
		isl_space_free(prog->array[i].space);
		isl_set_free(prog->array[i].extent);
		free(prog->array[i].bound);
		free(prog->array[i].refs);
		isl_union_map_free(prog->array[i].dep_order);
	}
	free(prog->array);
}

/* Check if a gpu array is a scalar.  A scalar is a value that is not stored
 * as an array or through a pointer reference, but as a single data element.
 * At the moment, scalars are represented as zero-dimensional arrays.
 * Note that the single data element may be an entire structure.
 */
int gpu_array_is_scalar(struct gpu_array_info *array)
{
	return array->n_index == 0;
}

/* Is "array" a read-only scalar?
 */
int gpu_array_is_read_only_scalar(struct gpu_array_info *array)
{
	return array->read_only_scalar;
}

/* Return the set of parameter values for which the array has a positive
 * size in all dimensions.
 * If the sizes are only valid for some parameter values, then those
 * constraints are also taken into account.
 */
__isl_give isl_set *gpu_array_positive_size_guard(struct gpu_array_info *array)
{
	int i;
	isl_space *space;
	isl_set *guard;

	space = isl_space_params(isl_space_copy(array->space));
	guard = isl_set_universe(space);

	for (i = 0; i < array->n_index; ++i) {
		isl_pw_aff *bound;
		isl_set *guard_i, *zero;

		bound = isl_pw_aff_copy(array->bound[i]);
		guard_i = isl_pw_aff_nonneg_set(isl_pw_aff_copy(bound));
		zero = isl_pw_aff_zero_set(bound);
		guard_i = isl_set_subtract(guard_i, zero);
		guard = isl_set_intersect(guard, guard_i);
	}

	return guard;
}

/* Internal data structure for extract_size_of_type.
 * "type" specifies the name of the space that we want to extract.
 * "res" is used to store the subset of that space.
 */
struct ppcg_extract_size_data {
	const char *type;
	isl_set *res;
};

/* This function is called for each set in a union_set.
 * If the name of the set matches data->type, we store the
 * set in data->res.
 */
static int extract_size_of_type(__isl_take isl_set *size, void *user)
{
	struct ppcg_extract_size_data *data = user;
	const char *name;

	name = isl_set_get_tuple_name(size);
	if (name && !strcmp(name, data->type)) {
		data->res = size;
		return -1;
	}

	isl_set_free(size);
	return 0;
}

/* Given a union map { kernel[i] -> *[...] },
 * return the range in the space called "type" for the kernel with
 * sequence number "id".
 */
static __isl_give isl_set *extract_sizes(__isl_keep isl_union_map *sizes,
	const char *type, int id)
{
	isl_space *space;
	isl_set *dom;
	isl_union_set *local_sizes;
	struct ppcg_extract_size_data data = { type, NULL };

	if (!sizes)
		return NULL;

	space = isl_union_map_get_space(sizes);
	space = isl_space_set_from_params(space);
	space = isl_space_add_dims(space, isl_dim_set, 1);
	space = isl_space_set_tuple_name(space, isl_dim_set, "kernel");
	dom = isl_set_universe(space);
	dom = isl_set_fix_si(dom, isl_dim_set, 0, id);

	local_sizes = isl_union_set_apply(isl_union_set_from_set(dom),
					isl_union_map_copy(sizes));
	isl_union_set_foreach_set(local_sizes, &extract_size_of_type, &data);
	isl_union_set_free(local_sizes);
	return data.res;
}

/* Given a singleton set, extract the first (at most *len) elements
 * of the single integer tuple into *sizes and update *len if needed.
 */
static void read_sizes_from_set(__isl_take isl_set *set, int *sizes, int *len)
{
	int i;
	int dim;

	if (!set)
		return;

	dim = isl_set_dim(set, isl_dim_set);
	if (dim < *len)
		*len = dim;

	for (i = 0; i < *len; ++i) {
		isl_val *v;

		v = isl_set_plain_get_val_if_fixed(set, isl_dim_set, i);
		assert(v);

		sizes[i] = isl_val_get_num_si(v);
		isl_val_free(v);
	}

	isl_set_free(set);
}

/* Add the map { kernel[id] -> type[sizes] } to gen->used_sizes,
 * if the option debug->dump_sizes is set.
 */
static void set_used_sizes(struct gpu_gen *gen, const char *type, int id,
	int *sizes, int len)
{
	int i;
	isl_space *space;
	isl_map *map;

	if (!gen->options->debug->dump_sizes)
		return;

	space = isl_union_map_get_space(gen->used_sizes);
	space = isl_space_set_from_params(space);
	space = isl_space_add_dims(space, isl_dim_set, 1);
	space = isl_space_set_tuple_name(space, isl_dim_set, "kernel");
	space = isl_space_from_domain(space);
	space = isl_space_add_dims(space, isl_dim_out, len);
	space = isl_space_set_tuple_name(space, isl_dim_out, type);

	map = isl_map_universe(space);
	map = isl_map_fix_si(map, isl_dim_in, 0, id);
	for (i = 0; i < len; ++i)
		map = isl_map_fix_si(map, isl_dim_out, i, sizes[i]);

	gen->used_sizes = isl_union_map_add_map(gen->used_sizes, map);
}

/* Extract user specified "tile" sizes from the "sizes" command line option,
 * defaulting to option->tile_size in each dimension.
 * *tile_len contains the maximum number of tile sizes needed.
 * Update *tile_len to the number of specified tile sizes, if any, and
 * return a pointer to the tile sizes (or NULL on error).
 * Add the effectively used sizes to gen->used_sizes.
 */
static int *read_tile_sizes(struct gpu_gen *gen, int *tile_len)
{
	int n;
	int *tile_size;
	isl_set *size;

	tile_size = isl_alloc_array(gen->ctx, int, *tile_len);
	if (!tile_size)
		return NULL;
	for (n = 0; n < *tile_len; ++n)
		tile_size[n] = gen->options->tile_size;

	size = extract_sizes(gen->sizes, "tile", gen->kernel_id);
	read_sizes_from_set(size, tile_size, tile_len);
	set_used_sizes(gen, "tile", gen->kernel_id, tile_size, *tile_len);

	return tile_size;
}

/* Extract user specified "block" sizes from the "sizes" command line option,
 * after filling in some potentially useful defaults.
 */
static void read_block_sizes(struct ppcg_kernel *kernel,
	__isl_keep isl_union_map *sizes)
{
	isl_set *size;

	if (kernel->n_block > 3)
		kernel->n_block = 3;
	switch (kernel->n_block) {
	case 1:
		kernel->block_dim[0] = 512;
		break;
	case 2:
		kernel->block_dim[0] = 32;
		kernel->block_dim[1] = 16;
		break;
	default:
		kernel->block_dim[0] = 32;
		kernel->block_dim[1] = 4;
		kernel->block_dim[2] = 4;
		break;
	}

	size = extract_sizes(sizes, "block", kernel->id);
	read_sizes_from_set(size, kernel->block_dim, &kernel->n_block);
}

/* Extract user specified "grid" sizes from the "sizes" command line option,
 * after filling in some potentially useful defaults.
 */
static void read_grid_sizes(struct ppcg_kernel *kernel,
	__isl_keep isl_union_map *sizes)
{
	isl_set *size;

	if (kernel->n_grid > 2)
		kernel->n_grid = 2;
	switch (kernel->n_grid) {
	case 1:
		kernel->grid_dim[0] = 32768;
		break;
	default:
		kernel->grid_dim[0] = 256;
		kernel->grid_dim[1] = 256;
		break;
	}

	size = extract_sizes(sizes, "grid", kernel->id);
	read_sizes_from_set(size, kernel->grid_dim, &kernel->n_grid);
}

/* Extract user specified grid and block sizes from the gen->sizes
 * command line option after filling in some potentially useful defaults.
 * Store the extracted sizes in "kernel".
 * Add the effectively used sizes to gen->used_sizes.
 */
static void read_grid_and_block_sizes(struct ppcg_kernel *kernel,
	struct gpu_gen *gen)
{
	read_block_sizes(kernel, gen->sizes);
	read_grid_sizes(kernel, gen->sizes);
	set_used_sizes(gen, "block", kernel->id,
					    kernel->block_dim, kernel->n_block);
	set_used_sizes(gen, "grid", kernel->id,
					    kernel->grid_dim, kernel->n_grid);
}

static void *free_stmts(struct gpu_stmt *stmts, int n)
{
	int i;

	if (!stmts)
		return NULL;

	for (i = 0; i < n; ++i) {
		struct gpu_stmt_access *access, *next;

		for (access = stmts[i].accesses; access; access = next) {
			next = access->next;
			isl_id_free(access->ref_id);
			isl_map_free(access->access);
			isl_map_free(access->tagged_access);
			free(access);
		}

		isl_id_free(stmts[i].id);
	}
	free(stmts);

	return NULL;
}

/* Construct a map from a domain of dimensionality "len"
 * to a domain of dimensionality "len" + "tile_len" that tiles
 * the "tile_len" coordinates starting at "first".
 * In particular, [s_i] -> [s_i / tile_size[i], s_i % tile_size[i]].
 * "dim" prescribes the parameters.
 */
static __isl_give isl_map *tile(__isl_take isl_space *dim, int len,
        int first, int tile_len, int *tile_size)
{
	int i;
	isl_basic_map *bmap;
	isl_constraint *c;
	isl_local_space *ls;

	dim = isl_space_add_dims(dim, isl_dim_in, len);
	dim = isl_space_add_dims(dim, isl_dim_out, len + tile_len);
	bmap = isl_basic_map_universe(isl_space_copy(dim));
	ls = isl_local_space_from_space(dim);

	for (i = 0; i < len - tile_len; ++i) {
		int j = i < first ? i : i + tile_len;
		int k = i < first ? i : i + 2 * tile_len;

		c = isl_equality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_in, j, -1);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out, k, 1);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}

	for (i = 0; i < tile_len; ++i) {
		c = isl_equality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_in,
						first + i, -1);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out,
						first + i, tile_size[i]);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out,
						first + i + tile_len, 1);
		bmap = isl_basic_map_add_constraint(bmap, c);

		c = isl_inequality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_out,
						   first + i + tile_len, 1);
		bmap = isl_basic_map_add_constraint(bmap, c);

		c = isl_inequality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_out,
						   first + i + tile_len, -1);
		c = isl_constraint_set_constant_si(c, tile_size[i] - 1);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}

	isl_local_space_free(ls);

	return isl_map_from_basic_map(bmap);
}

/* Construct a map from a domain of dimensionality "len"
 * to a domain of dimensionality "len" + "wrap_len" that "wraps"
 * the "wrap_len" coordinates starting at "first" according to "wrap_size".
 * In particular, [s_i] -> [s_i, s_i % wrap_size[i]].
 * To do so, we need extra variables corresponding to [s_i / wrap_size[i]],
 * that are projected out at the end.
 * "dim" prescribes the parameters.
 */
static __isl_give isl_map *wrap(__isl_take isl_space *dim, int len,
        int first, int wrap_len, int *wrap_size)
{
	int i;
	isl_basic_map *bmap;
	isl_constraint *c;
	isl_local_space *ls;

	dim = isl_space_add_dims(dim, isl_dim_in, len);
	dim = isl_space_add_dims(dim, isl_dim_out, len + 2 * wrap_len);
	bmap = isl_basic_map_universe(isl_space_copy(dim));
	ls = isl_local_space_from_space(dim);

	for (i = 0; i < len; ++i) {
		int k = i < first + wrap_len ? i : i + 2 * wrap_len;

		c = isl_equality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_in, i, -1);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out, k, 1);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}

	for (i = 0; i < wrap_len; ++i) {
		c = isl_equality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_out,
						    first + i, -1);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out,
						    first + wrap_len + i, 1);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out,
				    first + 2 * wrap_len + i, wrap_size[i]);
		bmap = isl_basic_map_add_constraint(bmap, c);

		c = isl_inequality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_out,
						    first + wrap_len + i, 1);
		bmap = isl_basic_map_add_constraint(bmap, c);

		c = isl_inequality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_out,
						    first + wrap_len + i, -1);
		c = isl_constraint_set_constant_si(c, wrap_size[i] - 1);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}

	isl_local_space_free(ls);

	bmap = isl_basic_map_project_out(bmap, isl_dim_out,
				first + 2 * wrap_len, wrap_len);

	return isl_map_from_basic_map(bmap);
}

/* Tile the B loops over the tile sizes and then tile/wrap
 * the T1 loops over the blocks.
 */
static __isl_give isl_union_map *tile_schedule(struct gpu_gen *gen,
	__isl_take isl_union_map *sched)
{
	struct ppcg_kernel *kernel = gen->kernel;
	isl_space *dim;
	isl_map *tiling, *block_tiling;

	dim = isl_union_map_get_space(sched);
	tiling = tile(isl_space_copy(dim), gen->untiled_len,
		      gen->tile_first, kernel->tile_len, kernel->tile_size);

	if (gen->options->wrap)
		block_tiling = wrap(dim, gen->untiled_len + kernel->tile_len,
			    gen->tile_first, kernel->n_grid, kernel->grid_dim);
	else
		block_tiling = tile(dim, gen->untiled_len + kernel->tile_len,
			    gen->tile_first, kernel->n_grid, kernel->grid_dim);

	gen->tiled_len = gen->untiled_len + kernel->tile_len + kernel->n_grid;

	tiling = isl_map_apply_range(tiling, block_tiling);

	sched = isl_union_map_apply_range(sched,
					     isl_union_map_from_map(tiling));

	gen->shared_len = gen->tile_first + kernel->tile_len + kernel->n_grid;

	return sched;
}

/* Equate the "T1P" iterators in the tiled schedule "sched"
 * to the block dimensions.
 */
static __isl_give isl_union_map *parametrize_tiled_schedule(
	struct gpu_gen *gen, __isl_take isl_union_map *sched)
{
	struct ppcg_kernel *kernel = gen->kernel;
	isl_space *dim;
	isl_set *par;

	dim = isl_union_map_get_space(sched);
	par = parametrization(dim, gen->tiled_len,
		gen->tile_first + kernel->n_grid, kernel->block_ids);
	sched = isl_union_map_intersect_range(sched,
						isl_union_set_from_set(par));

	return sched;
}

/* Tile/wrap the P1 loops over the threads.
 */
static __isl_give isl_union_map *thread_tile_schedule(struct gpu_gen *gen,
	__isl_take isl_union_map *sched)
{
	struct ppcg_kernel *kernel = gen->kernel;
	isl_space *dim;
	isl_map *tiling;
	isl_set *par;

	dim = isl_union_map_get_space(sched);

	if (gen->options->wrap)
		tiling = wrap(isl_space_copy(dim), gen->tiled_len,
			gen->shared_len, kernel->n_block, kernel->block_dim);
	else
		tiling = tile(isl_space_copy(dim), gen->tiled_len,
			gen->shared_len, kernel->n_block, kernel->block_dim);
	gen->thread_tiled_len = gen->tiled_len + kernel->n_block;

	sched = isl_union_map_apply_range(sched,
					     isl_union_map_from_map(tiling));

	par = parametrization(dim, gen->thread_tiled_len,
		gen->tile_first + kernel->tile_len +
		kernel->n_grid + kernel->n_block, kernel->thread_ids);
	sched = isl_union_map_intersect_range(sched,
						isl_union_set_from_set(par));

	gen->shared_len = gen->tile_first + kernel->tile_len + kernel->n_grid;

	return sched;
}

/* If the user asked for it, scale the shared memory tile loops
 * (T1T and T2) of "sched" by kernel->tile_size[i].
 * If we are not performing "wrapping", then additionally scale the T1P
 * loops by kernel->grid_dim[i].
 */
static __isl_give isl_union_map *scale_tile_loops(struct gpu_gen *gen,
	__isl_take isl_union_map *sched)
{
	struct ppcg_kernel *kernel = gen->kernel;
	int i;
	isl_space *dim;
	isl_basic_map *scale;
	isl_constraint *c;
	isl_local_space *ls;

	if (!gen->options->scale_tile_loops)
		return sched;

	dim = isl_union_map_get_space(sched);
	dim = isl_space_add_dims(dim, isl_dim_in, gen->tiled_len);
	dim = isl_space_add_dims(dim, isl_dim_out, gen->tiled_len);
	scale = isl_basic_map_universe(isl_space_copy(dim));
	ls = isl_local_space_from_space(dim);

	for (i = 0; i < gen->tiled_len; ++i) {
		int f = 1;

		if (i >= gen->tile_first &&
		    i < gen->tile_first + kernel->n_grid) {
			f = kernel->tile_size[i - gen->tile_first];
			if (!gen->options->wrap)
				f *= kernel->grid_dim[i - gen->tile_first];
		} else if (i >= gen->tile_first + kernel->n_grid &&
			   i < gen->tile_first + kernel->n_grid +
				kernel->tile_len) {
			f = kernel->tile_size[i -
					    (gen->tile_first + kernel->n_grid)];
		}

		c = isl_equality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_in, i, f);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out, i, -1);
		scale = isl_basic_map_add_constraint(scale, c);
	}

	isl_local_space_free(ls);

	sched = isl_union_map_apply_range(sched,
		isl_union_map_from_map(isl_map_from_basic_map(scale)));

	return sched;
}

/* If we are not performing "wrapping" and if the user asked for it,
 * scale the thread tile loops (P1T) of "sched" by kernel->block_dim[i].
 */
static __isl_give isl_union_map *scale_thread_tile_loops(struct gpu_gen *gen,
	__isl_take isl_union_map *sched)
{
	int i;
	isl_space *dim;
	isl_basic_map *scale;
	isl_constraint *c;
	isl_local_space *ls;

	if (gen->options->wrap)
		return sched;
	if (!gen->options->scale_tile_loops)
		return sched;

	dim = isl_union_map_get_space(sched);
	dim = isl_space_add_dims(dim, isl_dim_in, gen->thread_tiled_len);
	dim = isl_space_add_dims(dim, isl_dim_out, gen->thread_tiled_len);
	scale = isl_basic_map_universe(isl_space_copy(dim));
	ls = isl_local_space_from_space(dim);

	for (i = 0; i < gen->thread_tiled_len; ++i) {
		int f = 1;

		if (i >= gen->shared_len &&
		    i < gen->shared_len + gen->kernel->n_block)
			f = gen->kernel->block_dim[i - gen->shared_len];

		c = isl_equality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_in, i, f);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out, i, -1);
		scale = isl_basic_map_add_constraint(scale, c);
	}

	isl_local_space_free(ls);

	sched = isl_union_map_apply_range(sched,
		isl_union_map_from_map(isl_map_from_basic_map(scale)));

	return sched;
}

/* If we are not performing "wrapping" and if the user asked for it,
 * scale the "n_tile" loops starting at "first" of "sched" by gen->block_dim[i].
 */
static __isl_give isl_union_map *scale_access_tile_loops(struct gpu_gen *gen,
	__isl_take isl_union_map *sched, int len, int first, int n_tile)
{
	int i;
	isl_space *dim;
	isl_basic_map *scale;
	isl_constraint *c;
	isl_local_space *ls;

	if (gen->options->wrap)
		return sched;
	if (!gen->options->scale_tile_loops)
		return sched;

	dim = isl_union_map_get_space(sched);
	dim = isl_space_add_dims(dim, isl_dim_in, len);
	dim = isl_space_add_dims(dim, isl_dim_out, len);
	scale = isl_basic_map_universe(isl_space_copy(dim));
	ls = isl_local_space_from_space(dim);

	for (i = 0; i < len; ++i) {
		int f = 1;

		if (i >= first && i < first + n_tile)
			f = gen->kernel->block_dim[i - first];

		c = isl_equality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_in, i, f);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out, i, -1);
		scale = isl_basic_map_add_constraint(scale, c);
	}

	isl_local_space_free(ls);

	sched = isl_union_map_apply_range(sched,
		isl_union_map_from_map(isl_map_from_basic_map(scale)));

	return sched;
}

/* Add parameters p[i] with identifiers "ids" to "set",
 * with bounds to 0 <= p[i] < size[i].
 */
__isl_give isl_set *add_bounded_parameters(__isl_take isl_set *set,
	int *size, __isl_keep isl_id_list *ids)
{
	int i, len;
	unsigned nparam;

	len = isl_id_list_n_id(ids);
	nparam = isl_set_dim(set, isl_dim_param);
	set = isl_set_add_dims(set, isl_dim_param, len);

	for (i = 0; i < len; ++i) {
		isl_id *id;

		id = isl_id_list_get_id(ids, i);
		set = isl_set_set_dim_id(set, isl_dim_param, nparam + i, id);
		set = isl_set_lower_bound_si(set, isl_dim_param, nparam + i, 0);
		set = isl_set_upper_bound_si(set, isl_dim_param,
					    nparam + i, size[i] - 1);
	}

	return set;
}

/* Add "len" parameters p[i] with identifiers "ids" and intersect "set"
 * with
 *
 *	{ : 0 <= p[i] < size[i] }
 *
 * or an overapproximation.
 */
static __isl_give isl_set *add_bounded_parameters_dynamic(
	__isl_take isl_set *set, __isl_keep isl_multi_pw_aff *size,
	__isl_keep isl_id_list *ids)
{
	int i, len;
	unsigned nparam;
	isl_space *space;
	isl_local_space *ls;

	len = isl_multi_pw_aff_dim(size, isl_dim_out);
	nparam = isl_set_dim(set, isl_dim_param);
	set = isl_set_add_dims(set, isl_dim_param, len);

	for (i = 0; i < len; ++i) {
		isl_id *id;

		id = isl_id_list_get_id(ids, i);
		set = isl_set_set_dim_id(set, isl_dim_param, nparam + i, id);
	}

	space = isl_space_params(isl_set_get_space(set));
	ls = isl_local_space_from_space(space);
	for (i = 0; i < len; ++i) {
		isl_pw_aff *param, *size_i, *zero;
		isl_set *bound;

		param = isl_pw_aff_var_on_domain(isl_local_space_copy(ls),
						isl_dim_param, nparam + i);

		size_i = isl_multi_pw_aff_get_pw_aff(size, i);
		bound = isl_pw_aff_lt_set(isl_pw_aff_copy(param), size_i);
		bound = isl_set_from_basic_set(isl_set_simple_hull(bound));
		set = isl_set_intersect_params(set, bound);

		zero = isl_pw_aff_zero_on_domain(isl_local_space_copy(ls));
		bound = isl_pw_aff_ge_set(param, zero);
		set = isl_set_intersect_params(set, bound);
	}
	isl_local_space_free(ls);

	return set;
}

/* Construct a map from an access to group->array to the corresponding
 * shared/private memory tile.
 * The map is of the form
 *
 *	{ [D[i] -> A[a]] -> T[t] }
 *
 * where D represents the initial shared_len dimensions
 * of the computed schedule.
 */
static __isl_give isl_map *shift_access(struct gpu_array_ref_group *group)
{
	struct gpu_array_tile *tile;
	isl_multi_aff *tiling;

	tile = group->private_tile;
	if (!tile)
		tile = group->shared_tile;

	tiling = isl_multi_aff_copy(tile->tiling);

	return isl_map_from_multi_aff(tiling);
}

/* Given a schedule that iterates over all elements in a piece of an array,
 * perform tiling/wrapping over the threads.
 *
 * In particular, we tile the final iterators so that the final thread
 * dimension runs over the final array dimension.
 * However, if those final iterators have only a single iteration,
 * we try to tile earlier iterators instead.
 */
static __isl_give isl_map *tile_access_schedule(struct gpu_gen *gen,
	__isl_take isl_map *sched)
{
	isl_space *dim;
	isl_union_map *usched;
	isl_map *tiling;
	isl_set *par;
	unsigned nvar = isl_map_dim(sched, isl_dim_out);
	int n_tile;
	int first;

	n_tile = gen->kernel->n_block;
	if (n_tile > nvar) {
		int i;
		sched = isl_map_insert_dims(sched,
						isl_dim_out, 0, n_tile - nvar);
		for (i = 0; i < n_tile - nvar; ++i)
			sched = isl_map_fix_si(sched, isl_dim_out, i, 0);
		nvar = n_tile;
	}

	first = nvar - n_tile;

	for (; first > 0; first --)
		if (!map_plain_is_fixed(sched, isl_dim_out, first + n_tile - 1))
			break;

	dim = isl_map_get_space(sched);
	dim = isl_space_params(dim);
	if (gen->options->wrap)
		tiling = wrap(isl_space_copy(dim), nvar, first,
				n_tile, gen->kernel->block_dim);
	else
		tiling = tile(isl_space_copy(dim), nvar, first,
				n_tile, gen->kernel->block_dim);
	sched = isl_map_apply_range(sched, tiling);

	par = parametrization(dim, nvar + n_tile, first + n_tile,
				gen->kernel->thread_ids);
	sched = isl_map_intersect_range(sched, par);

	usched = isl_union_map_from_map(sched);
	usched = scale_access_tile_loops(gen, usched, nvar + n_tile,
					 first, n_tile);
	sched = isl_map_from_union_map(usched);

	return sched;
}

/* Return the union of all tagged access relations in the group.
 */
static __isl_give isl_union_map *group_tagged_access_relation(
	struct gpu_array_ref_group *group)
{
	int i;
	isl_union_map *access;

	access = isl_union_map_empty(isl_map_get_space(group->access));
	for (i = 0; i < group->n_ref; ++i) {
		isl_map *map_i;

		map_i = isl_map_copy(group->refs[i]->tagged_access);
		access = isl_union_map_union(access,
					    isl_union_map_from_map(map_i));
	}

	return access;
}

/* Return the extent of "array", recomputed from the bounds.
 * The recomputed extent may be simpler than the original extent.
 */
static __isl_give isl_set *array_extent(struct gpu_array_info *array)
{
	int i;
	isl_id *id;
	isl_space *space;
	isl_local_space *ls;
	isl_set *extent;

	id = isl_set_get_tuple_id(array->extent);
	space = isl_set_get_space(array->extent);
	extent = isl_set_universe(isl_space_copy(space));
	ls = isl_local_space_from_space(space);
	for (i = 0; i < array->n_index; ++i) {
		isl_pw_aff *bound;
		isl_aff *aff;
		isl_pw_aff *index;
		isl_set *lt;

		extent = isl_set_lower_bound_si(extent, isl_dim_set, i, 0);

		aff = isl_aff_var_on_domain(isl_local_space_copy(ls),
						isl_dim_set, i);
		index = isl_pw_aff_from_aff(aff);
		bound = isl_pw_aff_copy(array->bound[i]);
		bound = isl_pw_aff_from_range(bound);
		bound = isl_pw_aff_add_dims(bound, isl_dim_in, array->n_index);
		bound = isl_pw_aff_set_tuple_id(bound, isl_dim_in,
						isl_id_copy(id));
		lt = isl_pw_aff_lt_set(index, bound);
		extent = isl_set_intersect(extent, lt);
	}
	isl_local_space_free(ls);
	isl_id_free(id);

	return extent;
}

/* Return a map from the first shared_len dimensions of the computed
 * schedule to the array tile in
 * global memory that corresponds to the shared memory copy.
 *
 * In particular, return a map
 *
 *	{ D[i] -> A[a] }
 *
 * with constraints
 *
 *	tile_offset(i) <= a <= tile_offset(i) + tile_size - 1		(1)
 *
 * and
 *
 *	0 <= a <= array_size - 1					(2)
 *
 * Note that if some stride has been detected (i.e., when
 * group->shared_tile->bound[i].shift is set), then a in (1) refers
 * to the shifted and scaled down version.
 *
 * Constraints (1) are obtained by mapping the size constraints on the
 * shared/private memory tile back to the access relation.
 * Constraints (2) are obtained from the (recomputed) extent.
 */
static __isl_give isl_map *group_tile(struct gpu_array_ref_group *group)
{
	int i;
	int n_index = group->array->n_index;
	isl_map *tile;
	isl_space *space;
	isl_set *local;
	isl_set *extent;

	space = isl_multi_aff_get_space(group->shared_tile->tiling);
	space = isl_space_range(space);
	local = isl_set_universe(space);
	for (i = 0; i < n_index; ++i) {
		isl_val *bound;

		local = isl_set_lower_bound_si(local, isl_dim_set, i, 0);
		bound = isl_val_copy(group->shared_tile->bound[i].size);
		bound = isl_val_sub_ui(bound, 1);
		local = isl_set_upper_bound_val(local, isl_dim_set, i, bound);
	}
	local = isl_set_preimage_multi_aff(local,
				isl_multi_aff_copy(group->shared_tile->tiling));
	tile = isl_set_unwrap(local);
	extent = array_extent(group->array);
	tile = isl_map_intersect_range(tile, extent);

	return tile;
}

/* Given a mapping "iterator_map" from the AST schedule to a domain,
 * return the corresponding mapping from the AST schedule to
 * to the first shared_len dimensions of the schedule computed by PPCG.
 */
static __isl_give isl_pw_multi_aff *compute_sched_to_shared(struct gpu_gen *gen,
	__isl_take isl_pw_multi_aff *iterator_map)
{
	isl_union_map *umap;
	isl_space *space;
	isl_map *map, *sched;;

	space = isl_space_range(isl_pw_multi_aff_get_space(iterator_map));
	space = isl_space_from_domain(space);
	space = isl_space_add_dims(space, isl_dim_out, gen->shared_len);

	umap = isl_union_map_copy(gen->shared_sched);
	umap = isl_union_map_apply_range(umap,
			isl_union_map_copy(gen->shared_proj));
	map = isl_union_map_extract_map(umap, space);
	isl_union_map_free(umap);

	sched = isl_map_preimage_domain_pw_multi_aff(map, iterator_map);
	sched = isl_map_detect_equalities(sched);

	return isl_pw_multi_aff_from_map(sched);
}

/* Set unroll[j] if the input dimension j is involved in
 * the index expression represented by ma.
 */
static int check_unroll(__isl_take isl_set *set, __isl_take isl_multi_aff *ma,
	void *user)
{
	int i, j;
	int n_in = isl_multi_aff_dim(ma, isl_dim_in);
	int n_out = isl_multi_aff_dim(ma, isl_dim_out);
	int *unroll = user;

	for (i = 0; i < n_out; ++i) {
		isl_aff *aff;

		aff = isl_multi_aff_get_aff(ma, i);
		for (j = 0; j < n_in; ++j)
			if (isl_aff_involves_dims(aff, isl_dim_in, j, 1))
				unroll[j] = 1;
		isl_aff_free(aff);
	}

	isl_set_free(set);
	isl_multi_aff_free(ma);
	return 0;
}

/* Given an array pos mapping input dimensions to the corresponding
 * output dimension, construct the corresponding map.
 */
static __isl_give isl_map *permutation(__isl_take isl_space *dim,
	int *pos, int len)
{
	int i;
	isl_constraint *c;
	isl_basic_map *bmap;
	isl_local_space *ls;

	dim = isl_space_add_dims(dim, isl_dim_in, len);
	dim = isl_space_add_dims(dim, isl_dim_out, len);
	bmap = isl_basic_map_universe(isl_space_copy(dim));
	ls = isl_local_space_from_space(dim);

	for (i = 0; i < len; ++i) {
		c = isl_equality_alloc(isl_local_space_copy(ls));
		c = isl_constraint_set_coefficient_si(c, isl_dim_in, i,
						      -1);
		c = isl_constraint_set_coefficient_si(c, isl_dim_out, pos[i],
						      1);
		bmap = isl_basic_map_add_constraint(bmap, c);
	}
	isl_local_space_free(ls);

	return isl_map_from_basic_map(bmap);
}

/* Remove the private tiles from all array reference groups,
 * except for the groups of arrays that are marked force_private.
 */
static void remove_private_tiles(struct gpu_gen *gen)
{
	int i, j;

	for (i = 0; i < gen->kernel->n_array; ++i) {
		struct gpu_local_array_info *local = &gen->kernel->array[i];

		if (local->force_private)
			continue;

		for (j = 0; j < local->n_group; ++j) {
			struct gpu_array_ref_group *group = local->groups[j];

			group->private_tile =
				    gpu_array_tile_free(group->private_tile);
		}
	}
}

/* Find all loops involved in any of the index expressions for any of
 * the private accesses, move them innermost and then mark them as
 * requiring unrolling by setting gen->first_unroll.
 * The loops involved should all be parallel because of the checks
 * we performed in check_private_group_access.  Moving them innermost
 * is therefore a valid transformation.
 *
 * If any of the arrays are marked force_private, however, then
 * those loops may not be parallel with respect to the marked arrays.
 * If any of the loops would have to be moved innermost for the
 * (non forced) private accesses and if there are any force_private
 * arrays, then we revert the decision to map the selected arrays
 * to private memory.  An alternative solution would be to expand
 * the force_private arrays.
 *
 * Loops up to gen->shared_len are generated before the mapping to
 * threads is applied.  They should therefore be ignored.
 *
 * We compute the hidden equalities of the schedule first
 * since we will need them in our calls to isl_pw_multi_aff_from_map
 * and because we want to make sure that the same equalities
 * are also available to the code generator.
 */
static __isl_give isl_union_map *interchange_for_unroll(struct gpu_gen *gen,
	__isl_take isl_union_map *sched)
{
	struct ppcg_kernel *kernel = gen->kernel;
	int i, j;
	int unroll[gen->thread_tiled_len];
	int perm[gen->thread_tiled_len];
	isl_space *dim;
	isl_map *permute;
	int len = gen->shared_len + kernel->n_parallel + kernel->n_block;

	gen->first_unroll = -1;

	sched = isl_union_map_detect_equalities(sched);
	for (i = 0; i < gen->thread_tiled_len; ++i)
		unroll[i] = 0;
	for (i = 0; i < kernel->n_array; ++i) {
		struct gpu_local_array_info *array = &kernel->array[i];

		for (j = 0; j < array->n_group; ++j) {
			isl_union_map *access;
			isl_map *acc;
			isl_pw_multi_aff *pma;

			if (!array->groups[j]->private_tile)
				continue;

			access = gpu_array_ref_group_access_relation(
							array->groups[j], 1, 1);
			access = isl_union_map_apply_domain(access,
						isl_union_map_copy(sched));

			acc = isl_map_from_union_map(access);
			pma = isl_pw_multi_aff_from_map(acc);
			isl_pw_multi_aff_foreach_piece(pma,
							&check_unroll, unroll);

			isl_pw_multi_aff_free(pma);
		}
	}

	for (i = gen->shared_len; i < len; ++i)
		if (unroll[i])
			break;

	if (i >= len)
		return sched;

	for (i = len; i < gen->thread_tiled_len; ++i)
		if (unroll[i])
			return sched;

	if (kernel->any_force_private) {
		remove_private_tiles(gen);
		return sched;
	}

	j = 0;
	for (i = 0; i < gen->shared_len; ++i)
		perm[i] = j++;
	for (i = gen->shared_len; i < gen->thread_tiled_len; ++i)
		if (!unroll[i])
			perm[i] = j++;
	gen->first_unroll = j - gen->shared_len;
	for (i = gen->shared_len; i < len; ++i)
		if (unroll[i])
			perm[i] = j++;

	dim = isl_union_map_get_space(sched);
	permute = permutation(dim, perm, gen->thread_tiled_len);
	sched = isl_union_map_apply_range(sched,
					  isl_union_map_from_map(permute));

	return sched;
}

/* Construct a map with input the shared tile loops and the loops that
 * will be wrapped around the threads that relates these later loops
 * to the thread indices and then projects them out.
 */
static __isl_give isl_map *compute_privatization(struct gpu_gen *gen)
{
	struct ppcg_kernel *kernel = gen->kernel;
	isl_map *priv;
	isl_map *tiling;
	isl_map *proj;
	isl_set *par;
	isl_space *dim;

	dim = isl_union_map_get_space(gen->shared_sched);

	if (gen->options->wrap)
		tiling = wrap(isl_space_copy(dim),
			gen->shared_len + kernel->n_block,
			gen->shared_len, kernel->n_block, kernel->block_dim);
	else
		tiling = tile(isl_space_copy(dim),
			gen->shared_len + kernel->n_block,
			gen->shared_len, kernel->n_block, kernel->block_dim);

	priv = tiling;

	par = parametrization(dim, gen->shared_len + 2 * kernel->n_block,
		gen->tile_first + kernel->tile_len +
		kernel->n_grid + kernel->n_block, kernel->thread_ids);

	priv = isl_map_align_params(priv, isl_set_get_space(par));
	priv = isl_map_intersect_range(priv, par);

	dim = isl_map_get_space(priv);
	dim = isl_space_drop_dims(dim, isl_dim_in, 0, isl_space_dim(dim, isl_dim_in));
	dim = isl_space_drop_dims(dim, isl_dim_out, 0, isl_space_dim(dim, isl_dim_out));
	proj = projection(dim, gen->shared_len + 2 * kernel->n_block,
			  gen->shared_len);

	priv = isl_map_apply_range(priv, proj);

	return priv;
}

/* If max_shared_memory is not set to infinity (-1), then make
 * sure that the total amount of shared memory required by the
 * array reference groups mapped to shared memory by "kernel"
 * is no larger than this maximum.
 *
 * We apply a greedy approach and discard (keep in global memory)
 * those groups that would result in a total memory size that
 * is larger than the maximum.
 *
 * This function should be called after any function that may
 * affect the decision on whether to place a reference group
 * in private, shared or global memory.
 */
static void check_shared_memory_bound(struct ppcg_kernel *kernel)
{
	int i, j;
	isl_val *left, *size;

	if (kernel->options->max_shared_memory < 0)
		return;

	left = isl_val_int_from_si(kernel->ctx,
				    kernel->options->max_shared_memory);

	for (i = 0; i < kernel->n_array; ++i) {
		struct gpu_local_array_info *local = &kernel->array[i];

		for (j = 0; j < local->n_group; ++j) {
			struct gpu_array_ref_group *group;

			group = local->groups[j];
			if (group->private_tile)
				continue;
			if (!group->shared_tile)
				continue;

			size = gpu_array_tile_size(group->shared_tile);
			size = isl_val_mul_ui(size, local->array->size);

			if (isl_val_le(size, left)) {
				left = isl_val_sub(left, size);
				continue;
			}
			isl_val_free(size);

			group->shared_tile =
					gpu_array_tile_free(group->shared_tile);
		}
	}

	isl_val_free(left);
}

/* Compute a tiling for all the array reference groups in "kernel".
 */
static void compute_group_tilings(struct ppcg_kernel *kernel)
{
	int i, j;

	for (i = 0; i < kernel->n_array; ++i) {
		struct gpu_local_array_info *array = &kernel->array[i];

		for (j = 0; j < array->n_group; ++j)
			gpu_array_ref_group_compute_tiling(array->groups[j]);
	}
}

/* Take tiled_sched, project it onto the shared tile loops and
 * the loops that will be wrapped over the threads and
 * store the result in gen->shared_sched.
 * Also compute a projection that projects out the loops that will be
 * wrapped over the threads and store this projection in gen->shared_proj.
 */
static void compute_shared_sched(struct gpu_gen *gen)
{
	isl_space *dim;
	isl_map *proj;
	isl_set *par;
	isl_union_map *sched;

	sched = isl_union_map_copy(gen->tiled_sched);

	dim = isl_union_map_get_space(sched);
	proj = projection(dim, gen->tiled_len,
				gen->shared_len + gen->kernel->n_block);
	sched = isl_union_map_apply_range(sched, isl_union_map_from_map(proj));

	dim = isl_union_map_get_space(sched);
	proj = projection(dim, gen->shared_len + gen->kernel->n_block,
			gen->shared_len);

	gen->shared_sched = sched;
	gen->shared_proj = isl_union_map_from_map(proj);
}

/* Compute the size of a bounding box around the origin and "set",
 * where "set" is assumed to contain only non-negative elements.
 * In particular, compute the maximal value of "set" in each direction
 * and add one.
 */
static __isl_give isl_multi_pw_aff *extract_size(__isl_take isl_set *set,
	__isl_take isl_set *context)
{
	int i, n;
	isl_multi_pw_aff *mpa;

	context = isl_set_params(context);
	n = isl_set_dim(set, isl_dim_set);
	mpa = isl_multi_pw_aff_zero(isl_set_get_space(set));
	for (i = 0; i < n; ++i) {
		isl_space *space;
		isl_aff *one;
		isl_pw_aff *bound;

		bound = isl_set_dim_max(isl_set_copy(set), i);
		bound = isl_pw_aff_coalesce(bound);
		bound = isl_pw_aff_gist(bound, isl_set_copy(context));

		space = isl_pw_aff_get_domain_space(bound);
		one = isl_aff_zero_on_domain(isl_local_space_from_space(space));
		one = isl_aff_add_constant_si(one, 1);
		bound = isl_pw_aff_add(bound, isl_pw_aff_from_aff(one));
		mpa = isl_multi_pw_aff_set_pw_aff(mpa, i, bound);
	}
	isl_set_free(set);
	isl_set_free(context);

	return mpa;
}

/* Compute the effective grid size as a list of the sizes in each dimension.
 *
 * The grid size specified by the user or set by default
 * in read_grid_sizes() and applied by the block filter,
 * may be too large for the given code in the sense that
 * it may contain blocks that don't need to execute anything.
 * We therefore don't return this grid size, but instead the
 * smallest grid size that ensures that all blocks that actually
 * execute code are included in the grid.
 *
 * We first extract a description of the grid, i.e., the possible values
 * of the block ids, from the domain elements in "domain" and
 * kernel->block_filter.
 * The block ids are parameters in kernel->block_filter.
 * We simply need to change them into set dimensions.
 *
 * Then, for each block dimension, we compute the maximal value of the block id
 * and add one.
 */
static __isl_give isl_multi_pw_aff *extract_grid_size(
	struct ppcg_kernel *kernel, __isl_take isl_union_set *domain)
{
	int i;
	isl_set *grid;

	domain = isl_union_set_intersect(domain,
				    isl_union_set_copy(kernel->block_filter));
	grid = isl_union_set_params(domain);
	grid = isl_set_from_params(grid);
	grid = isl_set_add_dims(grid, isl_dim_set, kernel->n_grid);
	for (i = 0; i < kernel->n_grid; ++i) {
		int pos;
		isl_id *id;

		id = isl_id_list_get_id(kernel->block_ids, i);
		pos = isl_set_find_dim_by_id(grid, isl_dim_param, id);
		isl_id_free(id);
		assert(pos >= 0);
		grid = isl_set_equate(grid, isl_dim_param, pos, isl_dim_set, i);
		grid = isl_set_project_out(grid, isl_dim_param, pos, 1);
	}

	return extract_size(grid, isl_set_copy(kernel->context));
}

/* Compute the size of a fixed bounding box around the origin and "set",
 * where "set" is assumed to contain only non-negative elements,
 * and store the results in "size".
 * In particular, compute the maximal value of "set" in each direction
 * and add one.
 */
static void extract_fixed_size(__isl_take isl_set *set, int *size)
{
	int i, n;
	isl_local_space *ls;
	isl_aff *obj;

	n = isl_set_dim(set, isl_dim_set);
	ls = isl_local_space_from_space(isl_set_get_space(set));
	obj = isl_aff_zero_on_domain(ls);
	for (i = 0; i < n; ++i) {
		isl_val *max;

		obj = isl_aff_set_coefficient_si(obj, isl_dim_in, i, 1);
		max = isl_set_max_val(set, obj);
		size[i] = isl_val_get_num_si(max) + 1;
		isl_val_free(max);
		obj = isl_aff_set_coefficient_si(obj, isl_dim_in, i, 0);
	}
	isl_aff_free(obj);
	isl_set_free(set);
}

/* Compute the effective block size as a list of the sizes in each dimension
 * and store the sizes in kernel->block_dim.
 *
 * The block size specified by the user or set by default
 * in read_block_sizes() and applied by the thread filter,
 * may be too large for the given code in the sense that
 * it may contain threads that don't need to execute anything.
 * We therefore update this block size in kernel->block_dim
 * to the smallest block size that ensures that all threads
 * that actually execute code are included in the block.
 *
 * The possible values of the thread ids is obtained from
 * the domain elements "domain" and kernel->thread_filter.
 * The current implementation eliminates all parameters, ensuring
 * that the size is a fixed constant in each dimension.
 * In principle we could also compute parametric sizes.
 * We would have to make sure to project out all b%d and t%d parameters,
 * however.
 */
static void extract_block_size(struct ppcg_kernel *kernel,
	__isl_take isl_union_set *domain)
{
	int i;
	int nparam;
	isl_set *block;

	domain = isl_union_set_intersect(domain,
				    isl_union_set_copy(kernel->thread_filter));
	block = isl_union_set_params(domain);
	block = isl_set_from_params(block);
	block = isl_set_add_dims(block, isl_dim_set, kernel->n_block);
	for (i = 0; i < kernel->n_block; ++i) {
		int pos;
		isl_id *id;

		id = isl_id_list_get_id(kernel->thread_ids, i);
		pos = isl_set_find_dim_by_id(block, isl_dim_param, id);
		isl_id_free(id);
		assert(pos >= 0);
		block = isl_set_equate(block, isl_dim_param, pos,
					isl_dim_set, i);
	}
	nparam = isl_set_dim(block, isl_dim_param);
	block = isl_set_project_out(block, isl_dim_param, 0, nparam);

	extract_fixed_size(block, kernel->block_dim);
}

struct ppcg_kernel *ppcg_kernel_free(struct ppcg_kernel *kernel)
{
	int i, j;

	if (!kernel)
		return NULL;

	isl_id_list_free(kernel->block_ids);
	isl_id_list_free(kernel->thread_ids);
	isl_multi_pw_aff_free(kernel->grid_size);
	isl_set_free(kernel->context);
	isl_union_set_free(kernel->core);
	isl_union_set_free(kernel->arrays);
	isl_space_free(kernel->space);
	isl_ast_node_free(kernel->tree);
	isl_union_set_free(kernel->block_filter);
	isl_union_set_free(kernel->thread_filter);

	for (i = 0; i < kernel->n_array; ++i) {
		struct gpu_local_array_info *array = &kernel->array[i];

		for (j = 0; j < array->n_group; ++j)
			gpu_array_ref_group_free(array->groups[j]);
		free(array->groups);

		isl_pw_aff_list_free(array->bound);
	}
	free(kernel->array);

	for (i = 0; i < kernel->n_var; ++i) {
		free(kernel->var[i].name);
		isl_vec_free(kernel->var[i].size);
	}
	free(kernel->var);
	free(kernel->tile_size);

	free(kernel);

	return NULL;
}

/* Wrapper around ppcg_kernel_free for use as a isl_id_set_free_user callback.
 */
static void ppcg_kernel_free_wrap(void *user)
{
	struct ppcg_kernel *kernel = user;

	ppcg_kernel_free(kernel);
}

static void create_kernel_var(isl_ctx *ctx, struct gpu_array_ref_group *group,
	struct ppcg_kernel_var *var)
{
	int j;
	struct gpu_array_tile *tile;
	isl_printer *p;
	char *name;

	var->array = group->array;

	tile = group->private_tile;
	var->type = ppcg_access_private;
	if (!tile) {
		tile = group->shared_tile;
		var->type = ppcg_access_shared;
	}

	p = isl_printer_to_str(ctx);
	p = gpu_array_ref_group_print_name(group, p);
	var->name = isl_printer_get_str(p);
	isl_printer_free(p);

	var->size = isl_vec_alloc(ctx, group->array->n_index);

	for (j = 0; j < group->array->n_index; ++j)
		var->size = isl_vec_set_element_val(var->size, j,
					    isl_val_copy(tile->bound[j].size));
}

static void create_kernel_vars(struct ppcg_kernel *kernel)
{
	int i, j, n;

	n = 0;
	for (i = 0; i < kernel->n_array; ++i) {
		struct gpu_local_array_info *array = &kernel->array[i];

		for (j = 0; j < array->n_group; ++j) {
			struct gpu_array_ref_group *group = array->groups[j];
			if (group->private_tile || group->shared_tile)
				++n;
		}
	}

	kernel->n_var = n;
	kernel->var = isl_calloc_array(kernel->ctx, struct ppcg_kernel_var, n);
	assert(kernel->var);

	n = 0;
	for (i = 0; i < kernel->n_array; ++i) {
		struct gpu_local_array_info *array = &kernel->array[i];

		for (j = 0; j < array->n_group; ++j) {
			struct gpu_array_ref_group *group = array->groups[j];
			if (!group->private_tile && !group->shared_tile)
				continue;
			create_kernel_var(kernel->ctx, group, &kernel->var[n]);
			++n;
		}
	}
}

/* Replace "pa" by the zero function defined over the universe domain
 * in the space of "pa".
 */
static __isl_give isl_pw_aff *set_universally_zero(__isl_take isl_pw_aff *pa)
{
	isl_space *space;
	isl_aff *zero;

	space = isl_space_domain(isl_pw_aff_get_space(pa));
	isl_pw_aff_free(pa);
	zero = isl_aff_zero_on_domain(isl_local_space_from_space(space));

	return isl_pw_aff_from_aff(zero);
}

/* The sizes of the arrays on the host that have been computed by
 * extract_array_info may depend on the parameters.  Use the extra
 * constraints on the parameters that are valid at "host_domain"
 * to simplify these expressions and store the results in kernel->array.
 *
 * We only need these localized bounds for arrays that are accessed
 * by the current kernel.  If we have found at least one reference group
 * then the array is accessed by the kernel.  If the array has compound
 * elements then we skipped the construction of array reference groups.
 *
 * The resulting sizes may be functions that are nowhere defined
 * in case the access function cannot possibly access anything inside
 * the kernel for some reason.  If so, they are replaced by the zero
 * function.  Since the access function cannot actually access anything,
 * there is no harm in printing the array sizes as zero.
 */
static void localize_bounds(struct gpu_gen *gen, struct ppcg_kernel *kernel,
	__isl_keep isl_set *host_domain)
{
	int i, j;
	isl_set *context;

	context = isl_set_copy(host_domain);
	context = isl_set_params(context);

	for (i = 0; i < kernel->n_array; ++i) {
		struct gpu_local_array_info *local = &kernel->array[i];
		isl_pw_aff_list *bound;
		int n_index;

		if (local->n_group == 0 && !local->array->has_compound_element)
			continue;

		n_index = local->array->n_index;
		bound = isl_pw_aff_list_alloc(gen->ctx, n_index);

		for (j = 0; j < n_index; ++j) {
			isl_pw_aff *pwaff;
			int empty;

			pwaff = isl_pw_aff_copy(local->array->bound[j]);
			pwaff = isl_pw_aff_gist(pwaff, isl_set_copy(context));
			empty = isl_pw_aff_is_empty(pwaff);
			if (empty < 0)
				pwaff = isl_pw_aff_free(pwaff);
			else if (empty)
				pwaff = set_universally_zero(pwaff);
			bound = isl_pw_aff_list_add(bound, pwaff);
		}

		local->n_index = n_index;
		local->bound = bound;
	}
	isl_set_free(context);
}

/* Create the array of gpu_local_array_info structures "array"
 * inside "kernel".  The number of elements in this array is
 * the same as the number of arrays in "prog".
 * Initialize the "array" field of each local array to point
 * to the corresponding array in "prog".
 */
static struct ppcg_kernel *ppcg_kernel_create_local_arrays(
	struct ppcg_kernel *kernel, struct gpu_prog *prog)
{
	int i;
	isl_ctx *ctx;

	ctx = isl_set_get_ctx(prog->context);
	kernel->array = isl_calloc_array(ctx,
			    struct gpu_local_array_info, prog->n_array);
	if (!kernel->array)
		return ppcg_kernel_free(kernel);
	kernel->n_array = prog->n_array;

	for (i = 0; i < prog->n_array; ++i)
		kernel->array[i].array = &prog->array[i];

	return kernel;
}

/* Find the element in gen->stmt that has the given "id".
 * Return NULL if no such gpu_stmt can be found.
 */
static struct gpu_stmt *find_stmt(struct gpu_prog *prog, __isl_keep isl_id *id)
{
	int i;

	for (i = 0; i < prog->n_stmts; ++i) {
		if (id == prog->stmts[i].id)
			break;
	}

	return i < prog->n_stmts ? &prog->stmts[i] : NULL;
}

void ppcg_kernel_stmt_free(void *user)
{
	int i;
	struct ppcg_kernel_stmt *stmt = user;

	if (!stmt)
		return;

	switch (stmt->type) {
	case ppcg_kernel_copy:
		isl_ast_expr_free(stmt->u.c.index);
		isl_ast_expr_free(stmt->u.c.local_index);
		break;
	case ppcg_kernel_domain:
		isl_id_to_ast_expr_free(stmt->u.d.ref2expr);
		break;
	case ppcg_kernel_sync:
		break;
	}

	free(stmt);
}

/* Set the options of "context" to
 *
 *	{ space -> [x] : x >= first }
 */
static __isl_give isl_ast_build *set_unroll(
	__isl_take isl_ast_build *build, __isl_take isl_space *space,
	int first)
{
	isl_ctx *ctx;
	isl_map *unroll;
	isl_union_map *opt;

	ctx = isl_ast_build_get_ctx(build);

	space = isl_space_from_domain(space);
	space = isl_space_add_dims(space, isl_dim_out, 1);
	space = isl_space_set_tuple_name(space, isl_dim_out, "unroll");
	unroll = isl_map_universe(space);
	unroll = isl_map_lower_bound_si(unroll, isl_dim_out, 0, first);
	opt = isl_union_map_from_map(unroll);

	build = isl_ast_build_set_options(build, opt);

	return build;
}

/* Extend the schedule "schedule" with the part of "extension"
 * starting at "first" up to "len".
 */
static __isl_give isl_union_map *extend_schedule(
	__isl_take isl_union_map *schedule,
	__isl_take isl_union_map *extension, int first, int len)
{
	isl_space *space;
	isl_map *proj;
	isl_union_map *umap;
	isl_set *set;

	space = isl_union_map_get_space(schedule);
	space = isl_space_set_from_params(space);
	space = isl_space_add_dims(space, isl_dim_set, len);
	proj = isl_set_identity(isl_set_universe(space));
	proj = isl_map_project_out(proj, isl_dim_out, 0, first);
	extension = isl_union_map_apply_range(extension,
						isl_union_map_from_map(proj));

	schedule = isl_union_map_range_product(schedule, extension);

	return schedule;
}

/* Return the gpu_stmt_access in the list "accesses" that corresponds
 * to "ref_id".
 */
static struct gpu_stmt_access *find_access(struct gpu_stmt_access *accesses,
	__isl_keep isl_id *ref_id)
{
	struct gpu_stmt_access *access;

	for (access = accesses; access; access = access->next)
		if (access->ref_id == ref_id)
			return access;

	return NULL;
}

/* Return the index of the array called "name" in the list of arrays.
 */
static int find_array_index(struct gpu_gen *gen, const char *name)
{
	int i;

	for (i = 0; i < gen->prog->n_array; ++i)
		if (!strcmp(name, gen->prog->array[i].name))
			return i;

	return -1;
}

/* Internal data structure for the index and AST expression transformation
 * callbacks for pet_stmt_build_ast_exprs.
 *
 * "accesses" is the list of gpu_stmt_access in the statement.
 * "iterator_map" expresses the statement iterators in terms of
 * the AST loop iterators.
 * "sched2shared" expresses the first shared_len dimensions of
 * the computed schedule in terms of the AST loop iterators.
 *
 * The following fields are set in transform_index and used in transform_expr.
 * "array" is the array that is being accessed.
 * "global" is set if the global array is accessed (rather than
 * shared/private memory).
 * "local_array" refers to information on the array specialized
 * to the current kernel.
 */
struct ppcg_transform_data {
	struct gpu_gen *gen;
	struct gpu_stmt_access *accesses;
	isl_pw_multi_aff *iterator_map;
	isl_pw_multi_aff *sched2shared;

	struct gpu_array_info *array;
	int global;
	struct gpu_local_array_info *local_array;
};

/* Return the name of the outer array (of structs) accessed by "access".
 */
static const char *get_outer_array_name(__isl_keep isl_map *access)
{
	isl_space *space;
	const char *name;

	space = isl_space_range(isl_map_get_space(access));
	while (space && isl_space_is_wrapping(space))
		space = isl_space_domain(isl_space_unwrap(space));
	name = isl_space_get_tuple_name(space, isl_dim_set);
	isl_space_free(space);

	return name;
}

/* Return a pointer to the gpu_array_ref_group in "local"
 * that contains the reference "access".
 * Return NULL if no such group can be found.
 */
static struct gpu_array_ref_group *find_ref_group(
	struct gpu_local_array_info *local, struct gpu_stmt_access *access)
{
	int i, j;

	for (i = 0; i < local->n_group; ++i) {
		struct gpu_array_ref_group *group = local->groups[i];

		for (j = 0; j < group->n_ref; ++j)
			if (group->refs[j] == access)
				return group;
	}

	return NULL;
}

/* Index transformation callback for pet_stmt_build_ast_exprs.
 *
 * "index" expresses the array indices in terms of statement iterators
 *
 * We first reformulate "index" in terms of the AST loop iterators.
 * Then we check if we are accessing the global array or
 * a shared/private copy.  In the former case, we simply return
 * the updated index.  If "index" is an affine expression rather
 * than an array access, then we also return the updated index here.
 *
 * If no reference groups have been computed for the array,
 * then we can only be accessing the global array.
 *
 * Otherwise, we apply the tiling to the index.
 * This tiling is of the form
 *
 *	[D -> A] -> T
 *
 * The index is of the form
 *
 *	L -> A
 *
 * We update the tiling to refer to the AST loop iterators
 *
 *	[L -> A] -> T
 *
 * and modify index to keep track of those iterators
 *
 *	L -> [L -> A]
 *
 * Combining these two yields a tiled index expression in terms
 * of the AST loop iterators
 *
 *	L -> T
 */
static __isl_give isl_multi_pw_aff *transform_index(
	__isl_take isl_multi_pw_aff *index, __isl_keep isl_id *ref_id,
	void *user)
{
	struct ppcg_transform_data *data = user;
	struct gpu_stmt_access *access;
	struct gpu_array_ref_group *group;
	struct gpu_array_tile *tile;
	isl_pw_multi_aff *iterator_map;
	int i;
	const char *name;
	isl_space *space;
	isl_multi_pw_aff *tiling;
	isl_pw_multi_aff *pma;
	isl_multi_pw_aff *mpa;

	data->array = NULL;

	iterator_map = isl_pw_multi_aff_copy(data->iterator_map);
	index = isl_multi_pw_aff_pullback_pw_multi_aff(index, iterator_map);

	access = find_access(data->accesses, ref_id);
	if (!access)
		return index;
	if (!isl_map_has_tuple_name(access->access, isl_dim_out))
		return index;

	name = get_outer_array_name(access->access);
	i = find_array_index(data->gen, name);
	if (i < 0)
		isl_die(isl_multi_pw_aff_get_ctx(index), isl_error_internal,
			"cannot find array",
			return isl_multi_pw_aff_free(index));
	data->array = &data->gen->prog->array[i];
	data->local_array = &data->gen->kernel->array[i];

	group = find_ref_group(data->local_array, access);
	if (!group) {
		data->global = 1;
		return index;
	}

	tile = group->private_tile;
	if (!tile)
		tile = group->shared_tile;
	data->global = !tile;
	if (!tile)
		return index;

	space = isl_space_range(isl_multi_pw_aff_get_space(index));
	space = isl_space_map_from_set(space);
	pma = isl_pw_multi_aff_identity(space);
	pma = isl_pw_multi_aff_product(
			isl_pw_multi_aff_copy(data->sched2shared), pma);
	tiling = isl_multi_pw_aff_from_multi_aff(
				    isl_multi_aff_copy(tile->tiling));
	tiling = isl_multi_pw_aff_pullback_pw_multi_aff(tiling, pma);

	space = isl_space_domain(isl_multi_pw_aff_get_space(index));
	space = isl_space_map_from_set(space);
	mpa = isl_multi_pw_aff_identity(space);
	index = isl_multi_pw_aff_range_product(mpa, index);
	index = isl_multi_pw_aff_pullback_multi_pw_aff(tiling, index);

	return index;
}

/* Dereference "expr" by adding an index [0].
 * The original "expr" is assumed not to have any indices.
 *
 * If "expr" is a member access, then the dereferencing needs
 * to be applied to the structure argument of this member access.
 */
static __isl_give isl_ast_expr *dereference(__isl_take isl_ast_expr *expr)
{
	isl_ctx *ctx;
	isl_ast_expr *arg0, *res;
	isl_ast_expr_list *list;

	arg0 = isl_ast_expr_get_op_arg(expr, 0);
	if (!arg0)
		return isl_ast_expr_free(expr);
	if (isl_ast_expr_get_type(arg0) == isl_ast_expr_op &&
	    isl_ast_expr_get_op_type(arg0) == isl_ast_op_member) {
		isl_ast_expr *arg;

		arg = isl_ast_expr_get_op_arg(arg0, 0);
		arg = dereference(arg);
		arg0 = isl_ast_expr_set_op_arg(arg0, 0, arg);
		expr = isl_ast_expr_set_op_arg(expr, 0, arg0);

		return expr;
	}
	isl_ast_expr_free(arg0);

	ctx = isl_ast_expr_get_ctx(expr);
	res = isl_ast_expr_from_val(isl_val_zero(ctx));
	list = isl_ast_expr_list_from_ast_expr(res);
	res = isl_ast_expr_get_op_arg(expr, 0);
	res = isl_ast_expr_access(res, list);
	isl_ast_expr_free(expr);

	return res;
}

/* Linearize the index expression "expr" based on the array bounds
 * of "array".
 *
 * That is, transform expression
 *
 *	A[i_0][i_1]...[i_n]
 *
 * to
 *
 *	A[(..((i_0 * b_1 + i_1) ... ) * b_n + i_n]
 *
 * where b_0, b_1, ..., b_n are the bounds on the array.
 *
 * If the base of "expr" is a member access, then the linearization needs
 * to be applied to the structure argument of this member access.
 *
 * In the base case, if "expr" has no arguments (other than the name of
 * the array), then we are passing an entire array to a function.
 * In this case, there is nothing to linearize.
 * Note that at this point an expression with no arguments can
 * only be an entire array because the scalar case and
 * the case of single struct are handled by the caller.
 *
 * If the number of specified index expressions in "expr"
 * is smaller than the dimension of the accessed array,
 * then the missing i_j also do not appear in the linearized expression.
 * Furthermore, since such an expression does not refer to a single
 * element while the default linearized expression would refer to
 * a single element, we return the expression
 *
 *	A + (..((i_0 * b_1 + i_1) ... ) * b_n]
 *
 * instead.  Note that because of the special case handling above,
 * we can assume here that here that there is at least one index expression.
 */
__isl_give isl_ast_expr *gpu_local_array_info_linearize_index(
	struct gpu_local_array_info *array, __isl_take isl_ast_expr *expr)
{
	int i, n;
	isl_ctx *ctx;
	isl_set *context;
	isl_ast_expr *arg0;
	isl_ast_expr *res;
	isl_ast_expr_list *list;
	isl_ast_build *build;

	arg0 = isl_ast_expr_get_op_arg(expr, 0);
	if (isl_ast_expr_get_type(arg0) == isl_ast_expr_op &&
	    isl_ast_expr_get_op_type(arg0) == isl_ast_op_member) {
		isl_ast_expr *arg;

		arg = isl_ast_expr_get_op_arg(arg0, 0);
		arg = gpu_local_array_info_linearize_index(array, arg);
		arg0 = isl_ast_expr_set_op_arg(arg0, 0, arg);
		expr = isl_ast_expr_set_op_arg(expr, 0, arg0);

		return expr;
	}
	isl_ast_expr_free(arg0);

	if (isl_ast_expr_get_op_n_arg(expr) == 1)
		return expr;

	ctx = isl_ast_expr_get_ctx(expr);
	context = isl_set_universe(isl_space_params_alloc(ctx, 0));
	build = isl_ast_build_from_context(context);

	n = isl_ast_expr_get_op_n_arg(expr);
	res = isl_ast_expr_get_op_arg(expr, 1);
	for (i = 1; i < array->n_index; ++i) {
		isl_pw_aff *bound_i;
		isl_ast_expr *expr_i;

		bound_i = isl_pw_aff_list_get_pw_aff(array->bound, i);
		expr_i = isl_ast_build_expr_from_pw_aff(build, bound_i);
		res = isl_ast_expr_mul(res, expr_i);

		if (i + 1 >= n)
			continue;
		expr_i = isl_ast_expr_get_op_arg(expr, i + 1);
		res = isl_ast_expr_add(res, expr_i);
	}

	isl_ast_build_free(build);

	if (1 + array->n_index > n) {
		res = isl_ast_expr_add(isl_ast_expr_get_op_arg(expr, 0), res);
	} else {
		list = isl_ast_expr_list_from_ast_expr(res);
		res = isl_ast_expr_get_op_arg(expr, 0);
		res = isl_ast_expr_access(res, list);
	}

	isl_ast_expr_free(expr);

	return res;
}

/* AST expression transformation callback for pet_stmt_build_ast_exprs.
 *
 * If the AST expression refers to an array that is not accessed
 * at all, then this means the value of the expression is not used,
 * so we might as well print zero (NULL pointer) instead.
 *
 * If the AST expression refers to a global scalar that is not
 * a read-only scalar, then its address was passed to the kernel and
 * we need to dereference it.
 *
 * If the AST expression refers to an access to a global array,
 * then we linearize the access exploiting the bounds in data->local_array.
 */
static __isl_give isl_ast_expr *transform_expr(__isl_take isl_ast_expr *expr,
	__isl_keep isl_id *id, void *user)
{
	struct ppcg_transform_data *data = user;

	if (!data->array)
		return expr;
	if (!data->array->accessed) {
		isl_ctx *ctx;

		ctx = isl_ast_expr_get_ctx(expr);
		isl_ast_expr_free(expr);
		return isl_ast_expr_from_val(isl_val_zero(ctx));
	}
	if (gpu_array_is_read_only_scalar(data->array))
		return expr;
	if (!data->global)
		return expr;
	if (data->array->n_index == 0)
		return dereference(expr);
	if (!data->array->linearize)
		return expr;

	return gpu_local_array_info_linearize_index(data->local_array, expr);
}

/* This function is called for each instance of a user statement
 * in the kernel.
 *
 * We attach a struct ppcg_kernel_stmt to the "node", containing
 * a computed AST expression for each access.
 * These AST expressions are computed from iterator_map,
 * which expresses the domain
 * elements in terms of the generated loops, and sched2shared,
 * which expresses the first shared_len dimensions of the schedule
 * computed by PPCG in terms of the generated loops.
 */
static __isl_give isl_ast_node *at_each_domain(__isl_take isl_ast_node *node,
	__isl_keep isl_ast_build *build, void *user)
{
	struct ppcg_transform_data data;
	struct gpu_gen *gen = (struct gpu_gen *) user;
	struct ppcg_kernel_stmt *stmt;
	isl_id *id;
	isl_pw_multi_aff *sched2shared;
	isl_map *map;
	isl_pw_multi_aff *iterator_map;
	isl_ast_expr *expr, *arg;
	isl_union_map *schedule;

	stmt = isl_calloc_type(gen->ctx, struct ppcg_kernel_stmt);
	if (!stmt)
		return isl_ast_node_free(node);

	expr = isl_ast_node_user_get_expr(node);
	arg = isl_ast_expr_get_op_arg(expr, 0);
	id = isl_ast_expr_get_id(arg);

	schedule = isl_ast_build_get_schedule(build);
	map = isl_map_reverse(isl_map_from_union_map(schedule));
	iterator_map = isl_pw_multi_aff_from_map(map);
	sched2shared = compute_sched_to_shared(gen,
					isl_pw_multi_aff_copy(iterator_map));

	stmt->type = ppcg_kernel_domain;
	stmt->u.d.stmt = find_stmt(gen->prog, id);
	if (!stmt->u.d.stmt)
		isl_die(gen->ctx, isl_error_internal,
			"statement not found", goto error);

	data.gen = gen;
	data.accesses = stmt->u.d.stmt->accesses;
	data.iterator_map = iterator_map;
	data.sched2shared = sched2shared;
	stmt->u.d.ref2expr = pet_stmt_build_ast_exprs(stmt->u.d.stmt->stmt,
					    build, &transform_index, &data,
					    &transform_expr, &data);

	isl_id_free(id);
	isl_pw_multi_aff_free(iterator_map);
	isl_pw_multi_aff_free(sched2shared);
	isl_ast_expr_free(arg);
	isl_ast_expr_free(expr);

	id = isl_id_alloc(gen->ctx, NULL, stmt);
	id = isl_id_set_free_user(id, &ppcg_kernel_stmt_free);
	return isl_ast_node_set_annotation(node, id);
error:
	isl_id_free(id);
	isl_pw_multi_aff_free(iterator_map);
	ppcg_kernel_stmt_free(stmt);
	isl_pw_multi_aff_free(sched2shared);
	return isl_ast_node_free(node);
}

/* This function is called when code has been generated for the shared
 * tile loops.  The "schedule" refers only to the original statements.
 *
 * We extend the schedule with that part of gen->local_sched that hasn't
 * been taken into account yet.  This introduces parameters referring
 * to thread ids in the schedule, so we add them (with the appropriate
 * bounds to the context as well).
 * Finally, we set the appropriate unrolling options
 * if gen->first_unroll is set.
 */
static __isl_give isl_ast_node *create_domain_leaf(
	__isl_take isl_union_map *schedule, __isl_take isl_ast_build *build,
	void *user)
{
	struct gpu_gen *gen = (struct gpu_gen *) user;
	isl_space *space;
	isl_union_map *sched;
	isl_ast_node *tree;
	isl_set *set;
	isl_id_list *iterators;
	int n;

	schedule = extend_schedule(schedule,
			isl_union_map_copy(gen->local_sched),
			gen->shared_len, gen->thread_tiled_len);

	space = isl_ast_build_get_schedule_space(build);
	set = isl_set_universe(space);
	set = add_bounded_parameters(set, gen->kernel->block_dim,
					gen->kernel->thread_ids);
	build = isl_ast_build_restrict(build, set);

	n = gen->thread_tiled_len - gen->shared_len;

	if (gen->first_unroll >= 0) {
		space = isl_space_set_alloc(gen->ctx, 0, n);
		build = set_unroll(build, space, gen->first_unroll);
	}
	iterators = ppcg_scop_generate_names(gen->prog->scop, n, "c");
	build = isl_ast_build_set_iterators(build, iterators);
	build = isl_ast_build_set_at_each_domain(build, &at_each_domain, gen);
	tree = isl_ast_build_node_from_schedule_map(build, schedule);
	isl_ast_build_free(build);

	return tree;
}

/* This function is called for each statement node in the AST of the code
 * for copying to or from shared/private memory.
 * Attach a pointer to a ppcg_kernel_stmt representing the copy
 * statement to the node.
 * The statement name is "read" or "write", depending on whether we are
 * reading from global memory or writing to global memory.
 * The name of the T space is {shared,private}_<array>.
 *
 * The schedule is of the form
 *
 *	type[A -> T] -> L
 *
 * where A refers to a piece of an array and T to the corresponding
 * shifted tile.  We split this schedule into mappings L -> A and L -> T
 * and store the corresponding expressions in stmt->index and stmt->local_index,
 * where stmt points to the ppcg_kernel_stmt that is attached to the node.
 */
static __isl_give isl_ast_node *attach_copy_stmt(__isl_take isl_ast_node *node,
	__isl_keep isl_ast_build *build, void *user)
{
	struct gpu_gen *gen = (struct gpu_gen *) user;
	struct ppcg_kernel_stmt *stmt;
	isl_id *id;
	isl_ast_expr *expr;
	isl_space *space;
	isl_map *access, *local_access, *map;
	isl_pw_multi_aff *pma;
	const char *type;
	int array_index;

	stmt = isl_calloc_type(gen->ctx, struct ppcg_kernel_stmt);
	if (!stmt)
		return isl_ast_node_free(node);

	access = isl_map_from_union_map(isl_ast_build_get_schedule(build));
	type = isl_map_get_tuple_name(access, isl_dim_in);
	stmt->u.c.read = !strcmp(type, "read");
	access = isl_map_reverse(access);
	space = isl_space_unwrap(isl_space_range(isl_map_get_space(access)));
	local_access = isl_map_copy(access);

	map = isl_map_domain_map(isl_map_universe(isl_space_copy(space)));
	id = isl_map_get_tuple_id(access, isl_dim_out);
	map = isl_map_set_tuple_id(map, isl_dim_in, id);
	access = isl_map_apply_range(access, map);
	pma = isl_pw_multi_aff_from_map(access);
	expr = isl_ast_build_access_from_pw_multi_aff(build, pma);
	stmt->u.c.index = expr;

	map = isl_map_range_map(isl_map_universe(space));
	id = isl_map_get_tuple_id(local_access, isl_dim_out);
	map = isl_map_set_tuple_id(map, isl_dim_in, id);
	local_access = isl_map_apply_range(local_access, map);
	pma = isl_pw_multi_aff_from_map(local_access);
	expr = isl_ast_build_access_from_pw_multi_aff(build, pma);
	stmt->u.c.local_index = expr;

	stmt->u.c.array = gen->copy_group->array;
	array_index = stmt->u.c.array - gen->prog->array;
	stmt->u.c.local_array = &gen->kernel->array[array_index];
	stmt->type = ppcg_kernel_copy;

	id = isl_id_alloc(gen->ctx, NULL, stmt);
	id = isl_id_set_free_user(id, &ppcg_kernel_stmt_free);
	return isl_ast_node_set_annotation(node, id);
}

/* Given a schedule of the form
 *
 *	[S -> A] -> L
 *
 * (with S the first shared_len dimensions of the computed schedule,
 * A the array and L the schedule correponding to the generated loops),
 * indicating where to copy the array elements that need to be copied,
 * construct code for performing the copying.
 *
 * "group" is the array reference group that is being copied
 * "type" is either "read" or "write"
 * private is set if copying needs to be performed to/from registers
 *
 * We first construct a mapping to a shifted tile of the array,
 *
 *	[S -> A] -> T(S,A)					(1)
 *
 * If private is set, then we also use this mapping as a schedule
 * (which is already thread-specific and will be completely unrolled).
 * Otherwise, we wrap/tile the range over the threads.
 * The result is
 *
 *	[S -> A] -> T'(S,A)
 *
 * Combined with the given schedule, we have
 *
 *	[S -> A] -> [L -> T'(S,A)]				(2)
 *
 * From the shifted tile mapping, we construct a mapping
 *
 *	[S -> A] -> [A -> T(S,A)]
 *
 * and apply it to the schedule (2), obtaining
 *
 *	[A -> T(S(L),A)] -> [L -> T'(S(L),A)]
 *
 * Note that we can project out S because it is uniquely defined by L.
 */
static __isl_give isl_ast_node *copy_access(struct gpu_gen *gen,
	__isl_take isl_map *sched,
	const char *type, struct gpu_array_ref_group *group,
	__isl_take isl_ast_build *build, int private)
{
	isl_space *space;
	isl_ast_node *tree;
	isl_map *schedule, *shift, *map;
	isl_set *set;
	isl_id_list *iterators;
	int n;

	shift = shift_access(group);

	schedule = isl_map_copy(shift);
	schedule = isl_map_reset_tuple_id(schedule, isl_dim_out);
	if (!private)
		schedule = tile_access_schedule(gen, schedule);

	n = isl_map_dim(schedule, isl_dim_out);
	set = isl_set_universe(isl_ast_build_get_schedule_space(build));
	set = add_bounded_parameters(set, gen->kernel->block_dim,
					gen->kernel->thread_ids);

	schedule = isl_map_range_product(sched, schedule);

	space = isl_space_domain(isl_map_get_space(shift));
	map = isl_map_range_map(isl_map_universe(isl_space_unwrap(space)));
	map = isl_map_range_product(map, shift);

	schedule = isl_map_apply_domain(schedule, map);

	schedule = isl_map_set_tuple_name(schedule, isl_dim_in, type);

	build = isl_ast_build_restrict(build, set);

	gen->copy_group = group;

	if (private) {
		space = isl_space_range(isl_map_get_space(schedule));
		space = isl_space_range(isl_space_unwrap(space));
		build = set_unroll(build, space, 0);
	}
	iterators = ppcg_scop_generate_names(gen->prog->scop, n, "c");
	build = isl_ast_build_set_iterators(build, iterators);
	build = isl_ast_build_set_at_each_domain(build, &attach_copy_stmt, gen);
	tree = isl_ast_build_node_from_schedule_map(build,
					    isl_union_map_from_map(schedule));
	isl_ast_build_free(build);

	return tree;
}

/* Return code for reading into or writing from shared memory
 * the given array reference group.
 *
 * If we are performing a read from global memory to shared memory and
 * if the array involved is not a scalar, then we copy
 * the entire tile to shared memory.  This may result in some extra
 * elements getting copied, but it should lead to simpler code
 * (which means that fewer registers may be needed) and less divergence.
 *
 * Otherwise, we only copy the elements that will be read or have been written
 * in the kernel.
 *
 *
 * The input "sched" is of the form.
 *
 *	type[S -> A] -> L
 *
 * with S the first shared_len dimensions of the computed schedule,
 * A the array and L the schedule correponding to the generated loops.
 *
 * We first drop "type",
 *
 *	[S -> A] -> L
 *
 * If the above conditions are satisfied, we project out A,
 * resulting in
 *
 *	S -> L
 *
 * and then introduce the group tile [S -> T], resulting in
 *
 *	[S -> T] -> L
 */
static __isl_give isl_ast_node *copy_group_shared_accesses(
	struct gpu_gen *gen, struct gpu_array_ref_group *group,
	__isl_take isl_map *sched, __isl_take isl_ast_build *build)
{
	const char *type;
	int read;
	isl_union_map *access;

	type = isl_map_get_tuple_name(sched, isl_dim_in);
	read = !strcmp(type, "read");

	sched = isl_map_reset_tuple_id(sched, isl_dim_in);

	if (read && !gpu_array_is_scalar(group->array)) {
		isl_space *space;
		isl_map *map;

		space = isl_space_domain(isl_map_get_space(sched));
		space = isl_space_unwrap(space);
		map = isl_map_domain_map(isl_map_universe(space));
		sched = isl_map_apply_domain(sched, map);

		map = group_tile(group);
		map = isl_map_reverse(isl_map_domain_map(map));
		sched = isl_map_apply_domain(sched, map);
	}

	return copy_access(gen, sched, type, group, build, 0);
}

/* Return code for reading into or writing from private memory
 * the given array reference group.
 *
 * Let S be the first shared_len dimensions of the computed schedule,
 * D the iteration domains, A the array and L the schedule correponding
 * to the generated loops.
 * "sched" is of the form
 *
 *	type[S -> A] -> L
 *
 * where type is either "read" or "write".
 * We apply the privatization D -> S(t), with t the thread ids,
 * to the access relation D -> A to obtain the privatized access relation
 *
 *	S(t) -> A
 *
 * We drop the type from "sched" and intersect with the privatized access
 * relation to obtain
 *
 *	[S(t) -> A] -> L
 */
static __isl_give isl_ast_node *copy_group_private_accesses(
	struct gpu_gen *gen, struct gpu_array_ref_group *group,
	__isl_take isl_map *sched, __isl_take isl_ast_build *build)
{
	const char *type;
	int read;
	isl_union_map *priv;
	isl_union_map *access;
	isl_map *access_map;

	type = isl_map_get_tuple_name(sched, isl_dim_in);
	read = !strcmp(type, "read");

	priv = isl_union_map_from_map(isl_map_copy(gen->privatization));
	priv = isl_union_map_apply_range(isl_union_map_copy(gen->shared_sched),
					priv);

	access = gpu_array_ref_group_access_relation(group, read, !read);
	access = isl_union_map_apply_domain(access, priv);
	access_map = isl_map_from_union_map(access);

	sched = isl_map_reset_tuple_id(sched, isl_dim_in);
	sched = isl_map_intersect_domain(sched, isl_map_wrap(access_map));

	return copy_access(gen, sched, type, group, build, 1);
}

/* Return code for reading into or writing from shared or private memory.
 *
 * "schedule" is of the form
 *
 *	type[S -> A] -> L
 *
 * with S be the first shared_len dimensions of the computed schedule,
 * A the array and L the schedule correponding to the generated loops.
 * The array reference group is attached to "type".
 */
static __isl_give isl_ast_node *create_access_leaf(
	struct gpu_gen *gen, __isl_take isl_map *schedule,
	__isl_take isl_ast_build *build)
{
	struct gpu_array_ref_group *group;
	isl_id *id;

	id = isl_map_get_tuple_id(schedule, isl_dim_in);
	group = isl_id_get_user(id);
	isl_id_free(id);

	if (group->private_tile)
		return copy_group_private_accesses(gen, group, schedule,
							build);
	else
		return copy_group_shared_accesses(gen, group, schedule,
							build);
}

/* Create a domain node representing a synchronization.
 */
static __isl_give isl_ast_node *create_sync_leaf(
	struct gpu_gen *gen, __isl_take isl_map *schedule,
	__isl_take isl_ast_build *build)
{
	struct ppcg_kernel_stmt *stmt;
	isl_id *id;
	isl_space *space;
	isl_ast_node *node;
	isl_ast_expr *expr;

	isl_map_free(schedule);

	stmt = isl_calloc_type(gen->ctx, struct ppcg_kernel_stmt);
	if (!stmt)
		return NULL;

	stmt->type = ppcg_kernel_sync;

	space = isl_ast_build_get_schedule_space(build);
	space = isl_space_from_domain(space);
	space = isl_space_set_tuple_name(space, isl_dim_out, "sync");
	expr = isl_ast_build_call_from_pw_multi_aff(build,
		    isl_pw_multi_aff_from_multi_aff(isl_multi_aff_zero(space)));
	node = isl_ast_node_alloc_user(expr);
	isl_ast_build_free(build);

	id = isl_id_alloc(gen->ctx, NULL, stmt);
	id = isl_id_set_free_user(id, &ppcg_kernel_stmt_free);
	return isl_ast_node_set_annotation(node, id);
}

/* This function is called during the code generation at the point
 * where the schedule domain element is completely determined by
 * the generated code.  The input schedule contains the original
 * statements as well as synchronization and copy "statements".
 * The latter are scheduled at different points than any of the original
 * statements, so they will only arrive here in isolation.
 *
 * If the current schedule only refers to a single statement,
 * we check if it is a copy or synchronization statement and
 * call the appropriate functions.
 * Otherwise, we assume we are dealing with the original statements
 * and we call create_domain_leaf.
 */
static __isl_give isl_ast_node *create_kernel_leaf(
	__isl_take isl_ast_build *build, void *user)
{
	struct gpu_gen *gen = (struct gpu_gen *) user;
	isl_map *map;
	isl_union_map *schedule;
	const char *name;

	schedule = isl_ast_build_get_schedule(build);

	if (isl_union_map_n_map(schedule) != 1)
		return create_domain_leaf(schedule, build, user);

	map = isl_map_from_union_map(schedule);
	name = isl_map_get_tuple_name(map, isl_dim_in);
	if (!strcmp(name, "read") || !strcmp(name, "write"))
		return create_access_leaf(gen, map, build);
	if (!strcmp(name, "sync"))
		return create_sync_leaf(gen, map, build);

	return create_domain_leaf(isl_union_map_from_map(map), build, user);
}

/* Mark all odd schedule dimensions as "atomic" (when the even dimensions
 * have value 0) and all even schedule dimensions as "unroll".
 *
 * That is, the options look as follows
 *
 *	{ [0, b, 0, d, ..., 0] -> atomic[i] : exists a : i = 2 a + 1;
 *	  [a, b, c, d, ..., z] -> unroll[i] : exists a : i = 2 a }
 *
 * The even positions are used to be able to schedule copying blocks
 * and synchronization before or after each level of the shared memory
 * tile loops and we want to make sure that code for these is generated
 * separately (within each level).
 */
static __isl_give isl_ast_build *set_atomic_and_unroll(
	__isl_take isl_ast_build *build,
	__isl_take isl_space *space, int sched_len)
{
	isl_ctx *ctx;
	isl_map *map;
	isl_constraint *c;
	isl_union_map *opt;
	isl_local_space *ls;
	int i, n;

	ctx = isl_ast_build_get_ctx(build);

	space = isl_space_params(space);
	space = isl_space_add_dims(space, isl_dim_set, sched_len);
	space = isl_space_from_domain(space);
	space = isl_space_add_dims(space, isl_dim_out, 2);
	map = isl_map_universe(isl_space_copy(space));
	for (i = 0; i < sched_len; i += 2)
		map = isl_map_fix_si(map, isl_dim_in, i, 0);
	ls = isl_local_space_from_space(isl_map_get_space(map));
	c = isl_equality_alloc(ls);
	c = isl_constraint_set_coefficient_si(c, isl_dim_out, 0, 1);
	c = isl_constraint_set_coefficient_si(c, isl_dim_out, 1, 2);
	c = isl_constraint_set_constant_si(c, 1);
	map = isl_map_add_constraint(map, c);
	map = isl_map_project_out(map, isl_dim_out, 1, 1);
	map = isl_map_set_tuple_name(map, isl_dim_out, "atomic");
	opt = isl_union_map_from_map(map);

	map = isl_map_universe(space);
	ls = isl_local_space_from_space(isl_map_get_space(map));
	c = isl_equality_alloc(ls);
	c = isl_constraint_set_coefficient_si(c, isl_dim_out, 0, 1);
	c = isl_constraint_set_coefficient_si(c, isl_dim_out, 1, 2);
	map = isl_map_add_constraint(map, c);
	map = isl_map_project_out(map, isl_dim_out, 1, 1);
	map = isl_map_set_tuple_name(map, isl_dim_out, "unroll");
	opt = isl_union_map_add_map(opt, map);

	build = isl_ast_build_set_options(build, opt);

	return build;
}

/* Return a map that maps a space of dimension gen->shared_len
 * to its last dimensions starting at gen->tile_first.
 * The range is of dimension
 *
 *	2 * (gen->shared_len - gen->tile_first) + 1
 *
 * The input dimensions are mapped to the odd dimensions in the output,
 * while the even dimensions (except 2*pos) are fixed to 0.
 * Output dimension 2*pos (if pos >= 0) is fixed to "val".
 * If pos >= 0, then only the pos first dimensions starting at gen->tile_first
 * are mapped to the output.  The remaining input dimensions are projected
 * out and the corresponding output dimensions are fixed to 0.
 */
static __isl_give isl_map *insert_even(struct gpu_gen *gen,
	__isl_take isl_space *space, int pos, int val)
{
	int i, n;
	isl_map *proj;

	space = isl_space_set_from_params(space);
	space = isl_space_add_dims(space, isl_dim_set, gen->shared_len);
	space = isl_space_map_from_set(space);
	proj = isl_map_identity(space);
	proj = isl_map_project_out(proj, isl_dim_out, 0, gen->tile_first);
	n = gen->shared_len - gen->tile_first;
	for (i = 0; i <= n; ++i) {
		proj = isl_map_insert_dims(proj, isl_dim_out, 2 * i, 1);
		if (i == pos)
			proj = isl_map_fix_si(proj, isl_dim_out, 2 * i, val);
		else
			proj = isl_map_fix_si(proj, isl_dim_out, 2 * i, 0);
	}

	if (pos < 0)
		return proj;

	proj = isl_map_eliminate(proj, isl_dim_in, gen->tile_first + pos,
				gen->shared_len - (gen->tile_first + pos));
	for (i = pos; i < n; ++i)
		proj = isl_map_fix_si(proj, isl_dim_out, 2 * i + 1, 0);

	return proj;
}

/* Given the AST context schedule "schedule" and the mapping from
 * domains to the shared tile loops "shared_sched", add a schedule
 * for a synchronization operation at position "val" of loop level "pos".
 *
 * schedule is of the form
 *
 *	D -> L
 *
 * (with D the iteration domains and L the already generated loops),
 * while shared_sched is of the form
 *
 *	D -> S
 *
 * We combine them into
 *
 *	L -> S
 *
 * apply a mapping
 *
 *	[s_0,...] -> [0,s_{tile_first},0,..., val, 0, 0, ... 0]
 *
 * and use the result as a schedule for "sync".
 */
static __isl_give isl_union_map *add_sync_schedule(struct gpu_gen *gen,
	__isl_take isl_union_map *res, __isl_keep isl_union_map *schedule,
	__isl_keep isl_union_map *shared_sched, int pos, int val)
{
	isl_space *space;
	isl_map *proj, *map;

	shared_sched = isl_union_map_copy(shared_sched);
	schedule = isl_union_map_copy(schedule);

	space = isl_union_map_get_space(shared_sched);
	schedule = isl_union_map_apply_domain(shared_sched, schedule);
	map = isl_map_from_union_map(schedule);

	proj = insert_even(gen, space, pos, val);
	map = isl_map_apply_range(map, proj);
	map = isl_map_from_range(isl_map_wrap(map));
	map = isl_map_set_tuple_name(map, isl_dim_in, "sync");

	res = isl_union_map_add_map(res, map);

	return res;
}

/* Given a set of wrapped references "ref", return the corresponding
 * access relations based on the tagged access relations "tagged".
 *
 * The elements of "ref" are of the form
 *
 *	[D -> R]
 *
 * with D an iteration domains and R a reference.
 * The elements of "tagged" are of the form
 *
 *	[D -> R] -> A
 *
 * with A an array.
 *
 * Extend "tagged" to include the iteration domain in the range, i.e.,
 *
 *	[D -> R] -> [D -> A]
 *
 * apply the result to "ref" and then unwrap the resulting set
 * to obtain relations of the form
 *
 *	D -> A
 */
static __isl_give isl_union_map *wrapped_reference_to_access(
	__isl_take isl_union_set *ref, __isl_take isl_union_map *tagged)
{
	isl_union_map *tag2access;

	tag2access = isl_union_map_copy(tagged);
	tag2access = isl_union_map_universe(tag2access);
	tag2access = isl_union_set_unwrap(isl_union_map_domain(tag2access));
	tag2access = isl_union_map_domain_map(tag2access);
	tag2access = isl_union_map_range_product(tag2access, tagged);

	ref = isl_union_set_coalesce(ref);
	ref = isl_union_set_apply(ref, tag2access);

	return isl_union_set_unwrap(ref);
}

/* Given an access relation "access" from "group", remove those reads
 * if ("read" is 1) or writes (if "read" is 0) that are only needed to
 * communicate data within the same iteration of the last_shared dimension
 * of the group.
 *
 * If the access is a read then it is either an element of
 *
 *	live_in union (range flow)
 *
 * where live_in and flow may be overapproximations, or
 * it reads an uninitialized value (that is not live-in because
 * there is an intermediate kill) or it reads a value that was
 * written within the same (compound) statement instance.
 * If the access is a write then it is either an element of
 *
 *	live_out union (domain flow)
 *
 * or it writes a value that is never read (and is not live-out
 * because of an intermediate kill) or only
 * within the same (compound) statement instance.
 * In both cases, the access relation is also a subset of
 * the group access relation.
 *
 * The cases where an uninitialized value is read or a value is written
 * that is never read or where the dataflow occurs within a statement
 * instance are also considered local and may also be removed.
 *
 * Essentially, we compute the intersection of "access" with either
 *
 *	live_in union (range non-local-flow)
 *
 * or
 *
 *	live_out union (domain non-local-flow)
 *
 * We first construct a relation "local"
 *
 *	[[D -> R] -> [D' -> R']]
 *
 * of pairs of domain iterations accessing the reference group
 * and references in the group that are scheduled to the same iteration
 * of the last_shared dimension.
 *
 * If this relation does not intersect the dataflow dependences,
 * then there is nothing we can possibly remove, unless the dataflow
 * dependences themselves only relate a subset of the accesses.
 * In particular, the accesses may not be involved in any dataflow
 * dependences, either because they are uninitialized reads/dead writes
 * or because the dataflow occurs inside a statement instance.
 *
 * Since the computation below may break up the access relation
 * into smaller pieces, we only perform the intersection with
 * the non-local dependent accesses if the local pairs
 * intersect the dataflow dependences.  Otherwise, we intersect
 * with the universe of the non-local dependent accesses.
 * This should at least remove accesses from statements that
 * do not participate in any dependences.
 *
 * In particular, we remove the "local" dataflow dependences from
 * the set of all dataflow dependences.
 * Note that if the potential dataflow dependences are an overapproximation
 * of the actual dataflow dependences, then the result remains an
 * overapproximation of the non-local dataflow dependences.
 * Copying to/from global memory is only needed for the references
 * in the domain/range of the result or for accesses that are live out/in
 * for the entire scop.
 *
 * We therefore map the domain/range of the "external" relation
 * to the corresponding access relation and take the union with
 * the live out/in relation.
 */
static __isl_give isl_union_map *remove_local_accesses(struct gpu_gen *gen,
	struct gpu_array_ref_group *group, __isl_take isl_union_map *access,
	int read)
{
	int empty;
	isl_union_pw_multi_aff *tagger;
	isl_union_set *domain;
	isl_space *space;
	isl_union_map *sched, *local, *tagged, *external;
	isl_union_set *tag_set;
	isl_map *proj;

	if (isl_union_map_is_empty(access))
		return access;

	tagged = group_tagged_access_relation(group);

	sched = isl_union_map_copy(gen->sched);

	space = isl_union_map_get_space(sched);
	proj = projection(space, gen->untiled_len, group->last_shared + 1);
	sched = isl_union_map_apply_range(sched, isl_union_map_from_map(proj));

	tagger = isl_union_pw_multi_aff_copy(gen->prog->scop->tagger);
	domain = isl_union_map_domain(isl_union_map_copy(tagged));
	tagger = isl_union_pw_multi_aff_intersect_domain(tagger, domain);
	sched = isl_union_map_preimage_domain_union_pw_multi_aff(sched, tagger);

	local = isl_union_map_apply_range(sched,
			    isl_union_map_reverse(isl_union_map_copy(sched)));
	local = isl_union_map_intersect(local,
			isl_union_map_copy(gen->prog->scop->tagged_dep_flow));

	empty = isl_union_map_is_empty(local);

	external = isl_union_map_copy(gen->prog->scop->tagged_dep_flow);
	external = isl_union_map_intersect_params(external,
				isl_set_copy(gen->prog->scop->context));
	external = isl_union_map_subtract(external, local);

	if (read) {
		tag_set = isl_union_map_range(external);
		external = wrapped_reference_to_access(tag_set, tagged);
		external = isl_union_map_union(external,
				isl_union_map_copy(gen->prog->scop->live_in));
	} else {
		tag_set = isl_union_map_domain(external);
		external = wrapped_reference_to_access(tag_set, tagged);
		external = isl_union_map_union(external,
				isl_union_map_copy(gen->prog->scop->live_out));
	}

	if (empty < 0)
		external = isl_union_map_free(external);
	else if (empty)
		external = isl_union_map_universe(external);

	access = isl_union_map_intersect(access, external);

	return access;
}

/* Given the AST context schedule "schedule" and the mapping from
 * domains to the shared tile loops "shared_sched", add a schedule
 * for copying an array reference group to/from shared/private memory.
 * "read" is set if data should be copied from global memory
 * to shared/private memory.
 * "k" represents the current group
 * "s" is the total number of groups
 *
 * We schedule an operation before or after the innermost loop
 * of "shared_sched" that affects the tile of the array reference group.
 *
 * schedule is of the form
 *
 *	D -> L
 *
 * (with D the iteration domains and L the already generated loops),
 * while shared_sched is of the form
 *
 *	D -> S
 *
 * We first compute the access relation for the reference group
 *
 *	D -> A
 *
 * and remove from this access relation those reads or writes
 * that only needed to communicate data within the same iteration
 * of the last_shared dimension of the group.
 * We then combine what is left with shared_sched into
 *
 *	D -> [S -> A]
 *
 * If this results in an empty relation, no copying needs to be performed
 * at this point.
 * Otherwise, we invert the relation and combine it with "schedule" into
 *
 *	[S -> A] -> L
 *
 * The actual additional piece of the schedule is obtained from combining
 *
 *	[S -> A] -> S
 *
 * with a mapping
 *
 *	[s_0,...] -> [0,s_{tile_first},0,..., val, 0, 0, ... 0]
 *
 * The position of "val" corresponds to the innermost loop that affects
 * the tile and the value indicates where the copying is scheduled
 * with respect to the actual kernel code (at value 0).
 * Reads are schedule before the code, writes to global memory from
 * private memory are scheduled at values 1 to s, writes to global
 * memory from shared memory are scheduled at values s + 2 to 2 * s + 1.
 *
 * If we are scheduling a read from global memory to shared memory,
 * we insert a synchronization before the kernel code (at the innermost
 * level).
 * If we are scheduling a write to global memory, then we add
 * a synchronization after all writes (at value 2 *s + 2).
 * However, there is no need for a synchronization after the outermost loop.
 * A write to global memory from private memory at the innermost level
 * does not require a synchronization, because it is covered by
 * the synchronization after the kernel inserted by body_schedule.
 */
static __isl_give isl_union_map *add_group_schedule(struct gpu_gen *gen,
	__isl_take isl_union_map *res, __isl_keep isl_union_map *schedule,
	__isl_keep isl_union_map *shared_sched,
	struct gpu_array_ref_group *group, int read, int k, int s)
{
	int n;
	int pos, val;
	isl_space *space;
	isl_union_map *access;
	isl_map *map, *proj, *access_map;
	isl_id *id;

	access = gpu_array_ref_group_access_relation(group, read, !read);
	access = remove_local_accesses(gen, group, access, read);
	access = isl_union_map_range_product(isl_union_map_copy(shared_sched),
						access);

	if (isl_union_map_is_empty(access)) {
		isl_union_map_free(access);
		return res;
	}

	access = isl_union_map_reverse(access);
	access = isl_union_map_apply_range(access,
					    isl_union_map_copy(schedule));
	access_map = isl_map_from_union_map(access);

	space = isl_space_copy(group->array->space);
	space = isl_space_from_range(space);
	space = isl_space_add_dims(space, isl_dim_in, gen->shared_len);
	map = isl_map_domain_map(isl_map_universe(space));

	space = isl_union_map_get_space(schedule);
	pos = group->last_shared + 1 - gen->tile_first;
	assert(pos >= 0);
	if (read)
		val = -2 - k;
	else if (group->private_tile)
		val = 1 + k;
	else
		val = 1 + s + 1 + k;
	proj = insert_even(gen, space, pos, val);
	map = isl_map_apply_range(map, proj);

	access_map = isl_map_range_product(access_map, map);

	id = isl_id_alloc(gen->ctx, read ? "read" : "write", group);
	access_map = isl_map_set_tuple_id(access_map, isl_dim_in, id);

	res = isl_union_map_add_map(res, access_map);

	n = gen->shared_len - gen->tile_first;
	if (read) {
		if (!group->private_tile)
			res = add_sync_schedule(gen, res, schedule,
						shared_sched, n, -1);
	} else {
		if (pos == 0)
			return res;
		if (pos == n && group->private_tile)
			return res;
		res = add_sync_schedule(gen, res, schedule, shared_sched,
					pos, 2 * s + 2);
	}

	return res;
}

/* Return a schedule for the shared tile loops based on the current
 * AST context schedule.
 *
 * We create a "shared_sched" that maps the domains to the first
 * shared_len dimensions of the computed schedule, project out the
 * first tile_first dimensions (as these are already covered by
 * the host code) and insert "statement-level" dimensions at even
 * positions so that we can schedule copy blocks and synchronization
 * before/after each level.
 *
 * In particular, copy blocks are inserted inside the innermost
 * level that affect the tile.  For the copying to global memory,
 * those from private memory are scheduled before those from shared
 * memory such that synchronization can be inserted between the two
 * at the innermost level.
 * Synchronization is inserted at the innermost level before the
 * actual kernel code if there is any copying from global memory
 * to shared memory.  It is inserted unconditionally at the innermost
 * level after the actual kernel code and the copying to global memory
 * from private memory (if any).  Finally, it is inserted after
 * any copying to global memory, except at the outermost level
 * and at the innermost level if there is no copying from shared
 * memory.  The copying from private memory is covered by the unconditional
 * synchronization at the innermost level.
 */
static __isl_give isl_union_map *body_schedule(struct gpu_gen *gen,
	__isl_take isl_union_map *schedule)
{
	isl_space *space;
	isl_union_map *res;
	isl_union_map *shared_sched;
	isl_union_map *sched;
	isl_map *proj, *map;
	int i, j, k, s;

	shared_sched = isl_union_map_copy(gen->tiled_sched);
	proj = projection(isl_union_map_get_space(shared_sched),
				gen->tiled_len, gen->shared_len);
	shared_sched = isl_union_map_apply_range(shared_sched,
				isl_union_map_from_map(proj));
	space = isl_union_map_get_space(shared_sched);
	proj = insert_even(gen, space, -1, 0);
	sched = isl_union_map_apply_range(isl_union_map_copy(shared_sched),
				isl_union_map_from_map(proj));

	res = isl_union_map_range_product(isl_union_map_copy(schedule), sched);

	s = 0;
	for (i = 0; i < gen->kernel->n_array; ++i)
		s += gen->kernel->array[i].n_group;

	k = 0;
	for (i = 0; i < gen->kernel->n_array; ++i) {
		struct gpu_local_array_info *array = &gen->kernel->array[i];

		for (j = 0; j < array->n_group; ++j) {
			struct gpu_array_ref_group *group;

			group = array->groups[j];
			if (!group->private_tile && !group->shared_tile)
				continue;
			res = add_group_schedule(gen, res, schedule,
						shared_sched, group, 0, k, s);
			res = add_group_schedule(gen, res, schedule,
						shared_sched, group, 1, k, s);
			++k;
		}
	}

	res = add_sync_schedule(gen, res, schedule, shared_sched,
			    gen->shared_len - gen->tile_first, 1 + s);

	isl_union_map_free(shared_sched);
	isl_union_map_free(schedule);

	return res;
}

/* Generate code for "kernel" in the given "context".
 *
 * We first generate code for the shared tile loops (T1T, T1P and T2)
 * in a context that includes the block ids.
 * Within each iteration of these loops an additional code generation
 * is performed (within create_kernel_leaf) for the rest of the schedule
 * in a context that includes the thread ids.
 */
static __isl_give isl_ast_node *generate_kernel(struct gpu_gen *gen,
	__isl_keep isl_ast_build *build, __isl_keep isl_set *host_domain,
	__isl_keep isl_multi_pw_aff *grid_size)
{
	isl_space *space;
	isl_set *set;
	isl_id_list *iterators;
	isl_union_map *schedule;
	isl_ast_node *tree;
	int sched_len;

	schedule = isl_ast_build_get_schedule(build);

	build = isl_ast_build_copy(build);
	build = isl_ast_build_restrict(build, isl_set_copy(host_domain));
	space = isl_ast_build_get_schedule_space(build);
	set = isl_set_universe(isl_space_copy(space));
	set = add_bounded_parameters_dynamic(set, grid_size,
						gen->kernel->block_ids);
	build = isl_ast_build_restrict(build, set);

	schedule = body_schedule(gen, schedule);

	sched_len = 2 * (gen->shared_len - gen->tile_first) + 1;

	build = set_atomic_and_unroll(build, space, sched_len);
	iterators = ppcg_scop_generate_names(gen->prog->scop, sched_len, "g");
	build = isl_ast_build_set_iterators(build, iterators);
	build = isl_ast_build_set_create_leaf(build, &create_kernel_leaf, gen);
	tree = isl_ast_build_node_from_schedule_map(build, schedule);
	isl_ast_build_free(build);

	return tree;
}

/* Attach "id" to the given node.
 */
static __isl_give isl_ast_node *attach_id(__isl_take isl_ast_node *node,
	__isl_keep isl_ast_build *build, void *user)
{
	isl_id *id = user;

	node = isl_ast_node_set_annotation(node, id);

	return node;
}

/* Construct an AST node for performing a kernel launch and attach
 * the information about the kernel to that node.
 * "kernel_id" has name "kernel" and contains a pointer
 * to the ppcg_kernel structure.
 *
 * The kernel AST has been constructed in the context of the range
 * of "schedule".  In particular, the grid size has been computed
 * in the context.  We therefore still need to make sure that these
 * constraints are expressed in the code.  We do this by creating a schedule
 *
 *	kernel[] -> [S -> []]
 *
 * where S is the schedule domain, i.e., the range of "schedule".
 * The AST generation will then create a single call surrounded by
 * all the condition in "S" that have not been expressed yet.
 *
 * The kernel information is attached to this node in attach_id.
 */
static __isl_give isl_ast_node *construct_launch(
	__isl_take isl_ast_build *build, __isl_take isl_union_map *schedule,
	__isl_take isl_id *kernel_id)
{
	isl_ctx *ctx;
	isl_union_set *domain;
	isl_set *set;
	isl_map *map;
	isl_ast_node *node;

	ctx = isl_ast_build_get_ctx(build);

	domain = isl_union_map_range(schedule);
	set = isl_set_from_union_set(domain);
	map = isl_map_from_domain(set);
	map = isl_map_from_range(isl_map_wrap(map));
	map = isl_map_set_tuple_name(map, isl_dim_in, "kernel");
	schedule = isl_union_map_from_map(map);

	build = isl_ast_build_set_at_each_domain(build, &attach_id, kernel_id);
	node = isl_ast_build_node_from_schedule_map(build, schedule);
	isl_ast_build_free(build);

	return node;
}

/* This function is called for each leaf in the AST of the host code.
 * We first specialize the schedule to the site of the leaf, compute
 * the size of shared memory and then construct the body of the host code
 * and the associated kernel.
 *
 * The necessary information for printing the kernel launch is
 * stored in the struct ppcg_kernel that was created in create_kernel and
 * attached to an outer mark node in the schedule tree.
 * Note that this assumes that a kernel is only launched once.
 * The kernel pointer itself is stored in gen->kernel by before_mark,
 * while the isl_id containing this pointer is stored in gen->kernel_mark.
 * The latter is attached to the leaf AST node created to represent the launch.
 */
static __isl_give isl_ast_node *create_host_leaf(
	__isl_take isl_ast_build *build, void *user)
{
	struct gpu_gen *gen = (struct gpu_gen *) user;
	isl_id *id;
	isl_ast_node *node;
	struct ppcg_kernel *kernel;
	isl_set *host_domain;
	isl_union_map *schedule;
	isl_union_map *local_sched;
	isl_union_set *domain;
	int i;

	schedule = isl_ast_build_get_schedule(build);

	kernel = gen->kernel;
	if (!kernel)
		goto error;

	domain = isl_union_map_domain(isl_union_map_copy(schedule));

	local_sched = isl_union_map_copy(gen->sched);
	local_sched = isl_union_map_intersect_domain(local_sched, domain);

	gen->tiled_sched = tile_schedule(gen, local_sched);
	gen->tiled_sched = parametrize_tiled_schedule(gen, gen->tiled_sched);
	gen->tiled_sched = scale_tile_loops(gen, gen->tiled_sched);

	gen->local_sched = isl_union_map_copy(gen->tiled_sched);
	gen->local_sched = thread_tile_schedule(gen, gen->local_sched);
	gen->local_sched = scale_thread_tile_loops(gen, gen->local_sched);

	kernel->space = isl_ast_build_get_schedule_space(build);

	compute_shared_sched(gen);
	gen->privatization = compute_privatization(gen);
	if (gpu_group_references(gen) < 0)
		schedule = isl_union_map_free(schedule);
	host_domain = isl_set_from_union_set(isl_union_map_range(
						isl_union_map_copy(schedule)));
	localize_bounds(gen, kernel, host_domain);

	gen->local_sched = interchange_for_unroll(gen, gen->local_sched);
	check_shared_memory_bound(gen->kernel);
	compute_group_tilings(gen->kernel);

	kernel->tree = generate_kernel(gen, build, host_domain,
					kernel->grid_size);
	create_kernel_vars(kernel);

	isl_map_free(gen->privatization);
	isl_union_map_free(gen->local_sched);
	isl_union_map_free(gen->tiled_sched);
	isl_union_map_free(gen->shared_sched);
	isl_union_map_free(gen->shared_proj);
	isl_set_free(host_domain);

	node = construct_launch(build, schedule, isl_id_copy(gen->kernel_mark));

	return node;
error:
	isl_union_map_free(schedule);
	return NULL;
}

/* This function is called before the AST generator starts traversing
 * the schedule subtree of a node with mark "mark".
 *
 * If the mark is called "kernel", store the mark itself in gen->kernel_mark
 * and the kernel pointer in gen->kernel for use in create_host_leaf.
 */
static int before_mark(__isl_keep isl_id *mark,
	__isl_keep isl_ast_build *build, void *user)
{
	struct gpu_gen *gen = user;

	if (!mark)
		return -1;
	if (!strcmp(isl_id_get_name(mark), "kernel")) {
		gen->kernel_mark = isl_id_copy(mark);
		gen->kernel = isl_id_get_user(mark);
	}
	return 0;
}

/* This function is called after the AST generator has finished traversing
 * the schedule subtree of a mark node.  "node" points to the corresponding
 * mark AST node.
 *
 * If the mark is called "kernel", then clear kernel and gen->kernel_mark.
 */
static __isl_give isl_ast_node *after_mark(__isl_take isl_ast_node *node,
        __isl_keep isl_ast_build *build, void *user)
{
	struct gpu_gen *gen = user;
	isl_id *id;

	id = isl_ast_node_mark_get_id(node);
	if (!id)
		return isl_ast_node_free(node);
	if (!strcmp(isl_id_get_name(id), "kernel") && gen->kernel) {
		gen->kernel_mark = isl_id_free(gen->kernel_mark);
		gen->kernel = NULL;
	}

	isl_id_free(id);
	return node;
}

/* Use isl to generate host code from gen->host_schedule, which corresponds to
 * the outer gen->tile_first loops of the global schedule in gen->sched.
 * Within each iteration of this partial schedule, i.e., for each kernel
 * launch, create_host_leaf takes care of generating the kernel code.
 * The ppcg_kernel objects are stored in mark nodes in the schedule
 * tree and are extracted in before_mark.
 */
static __isl_give isl_ast_node *generate_host_code(struct gpu_gen *gen)
{
	isl_ast_build *build;
	isl_ast_node *tree;
	isl_schedule *schedule;
	isl_id_list *iterators;

	isl_options_set_ast_build_group_coscheduled(gen->ctx, 1);
	build = isl_ast_build_from_context(isl_set_copy(gen->prog->context));
	iterators = ppcg_scop_generate_names(gen->prog->scop,
						gen->tile_first, "h");
	build = isl_ast_build_set_iterators(build, iterators);
	build = isl_ast_build_set_create_leaf(build, &create_host_leaf, gen);
	build = isl_ast_build_set_before_each_mark(build, &before_mark, gen);
	build = isl_ast_build_set_after_each_mark(build, &after_mark, gen);
	schedule = isl_schedule_copy(gen->host_schedule);
	tree = isl_ast_build_node_from_schedule(build, schedule);
	isl_ast_build_free(build);

	return tree;
}

__isl_give isl_union_map *extract_sizes_from_str(isl_ctx *ctx, const char *str)
{
	if (!str)
		return NULL;
	return isl_union_map_read_from_str(ctx, str);
}

/* Information about the outermost tilable bands in the forest of bands.
 *
 * prefix is the (padded) schedule leading up to the outermost tilable bands.
 *
 * tile_first is the number of schedule dimensions in prefix.
 *
 * suffix is the schedule of the outermost tilable bands and their descendants.
 */
struct band_info {
	struct gpu_gen *gen;
	int tile_first;
	isl_union_map *prefix;
	isl_union_map *suffix;
};

/* Construct an isl_multi_val for use as tile sizes for tiling "node"
 * from the elements in "tile_size".
 */
static __isl_give isl_multi_val *construct_band_tiles_sizes(
	__isl_keep isl_schedule_node *node, int *tile_size)
{
	int i, n;
	isl_ctx *ctx;
	isl_space *space;
	isl_multi_val *mv;

	if (!node)
		return NULL;

	ctx = isl_schedule_node_get_ctx(node);
	space = isl_schedule_node_band_get_space(node);
	n = isl_schedule_node_band_n_member(node);
	mv = isl_multi_val_zero(space);
	for (i = 0; i < n; ++i) {
		isl_val *v;

		v = isl_val_int_from_si(ctx, tile_size[i]);
		mv = isl_multi_val_set_val(mv, i, v);
	}

	return mv;
}

/* Replace the partial schedule S of the band node "node" by
 *
 *	floor(S/f)
 *
 * or
 *
 *	f * floor(S/f)
 *
 * if scale_tile_loops is set, with f the integers in "factor".
 * The list that "factor" points to is assumed to contain at least
 * as many elements as the number of members in the band.
 */
static __isl_give isl_schedule_node *snap_band_to_sizes(
	__isl_take isl_schedule_node *node, int *factor,
	struct ppcg_options *options)
{
	isl_multi_val *mv;

	mv = construct_band_tiles_sizes(node, factor);
	node = isl_schedule_node_band_scale_down(node, isl_multi_val_copy(mv));
	if (options->scale_tile_loops)
		node = isl_schedule_node_band_scale(node,
							isl_multi_val_copy(mv));
	isl_multi_val_free(mv);

	return node;
}

/* Tile "band" with tile size specified by "sizes".
 *
 * Since the tile loops will be mapped to block ids, we forcibly
 * turn off tile loop scaling.  We may want to enable tile loop scaling
 * at some later point, but then we would have to support the detection
 * of strides during the mapping to block ids.
 * Similarly, since the point loops will be mapped to thread ids,
 * we forcibly shift the point loops so that they start at zero.
 */
static __isl_give isl_schedule_node *tile_band(
	__isl_take isl_schedule_node *node, __isl_take isl_multi_val *sizes)
{
	isl_ctx *ctx = isl_schedule_node_get_ctx(node);
	int scale_tile;
	int shift_point;

	scale_tile = isl_options_get_tile_scale_tile_loops(ctx);
	isl_options_set_tile_scale_tile_loops(ctx, 0);
	shift_point = isl_options_get_tile_shift_point_loops(ctx);
	isl_options_set_tile_shift_point_loops(ctx, 1);

	node = isl_schedule_node_band_tile(node, sizes);

	isl_options_set_tile_scale_tile_loops(ctx, scale_tile);
	isl_options_set_tile_shift_point_loops(ctx, shift_point);

	return node;
}

/* Extract the set of parameter values and outer schedule dimensions
 * for which any statement instance
 * in the kernel inserted at "node" needs to be executed.
 * Intersect the set of parameter values derived from the host schedule
 * relation with the context of "prog".
 */
static __isl_give isl_set *extract_context(__isl_keep isl_schedule_node *node,
	struct gpu_prog *prog)
{
	isl_union_map *schedule;
	isl_union_set *schedule_domain;
	isl_set *context;
	int empty;

	schedule = isl_schedule_node_get_prefix_schedule_relation(node);
	schedule_domain = isl_union_map_range(schedule);
	empty = isl_union_set_is_empty(schedule_domain);
	if (empty < 0) {
		isl_union_set_free(schedule_domain);
		return NULL;
	}
	if (empty) {
		int depth;
		isl_space *space;

		space = isl_union_set_get_space(schedule_domain);
		isl_union_set_free(schedule_domain);
		space = isl_space_set_from_params(space);
		depth = isl_schedule_node_get_schedule_depth(node);
		space = isl_space_add_dims(space, isl_dim_set, depth);
		context = isl_set_empty(space);
	} else {
		context = isl_set_from_union_set(schedule_domain);
	}
	context = isl_set_intersect_params(context,
					    isl_set_copy(prog->context));

	return context;
}

/* Return the set of outer array elements accessed by
 * by the statement instance in "domain" in "prog".
 */
static __isl_give isl_union_set *accessed_by_domain(
	__isl_take isl_union_set *domain, struct gpu_prog *prog)
{
	isl_union_map *access;
	isl_union_set *arrays;

	access = isl_union_map_union(isl_union_map_copy(prog->read),
				     isl_union_map_copy(prog->may_write));
	access = isl_union_map_intersect_domain(access, domain);
	arrays = isl_union_map_range(access);
	arrays = isl_union_set_apply(arrays,
				isl_union_map_copy(prog->to_outer));

	return arrays;
}

/* Return the number of outer band members of the band node "node"
 * that are marked coincident.
 */
static int n_outer_coincidence(__isl_keep isl_schedule_node *node)
{
	int i, n;

	n = isl_schedule_node_band_n_member(node);

	for (i = 0; i < n; ++i)
		if (!isl_schedule_node_band_member_get_coincident(node, i))
			break;

	return i;
}

/* If the band node "node" has more than "n" members, then split off
 * the first "n" of them.
 */
static __isl_give isl_schedule_node *split_band(
	__isl_take isl_schedule_node *node, int n)
{
	int dim;

	dim = isl_schedule_node_band_n_member(node);
	if (n < dim)
		node = isl_schedule_node_band_split(node, n);

	return node;
}

/* Scale a band node that may have been split by split_band.
 * "sizes" are the scaling factors for the original node.
 * "node" either points to the original band node, or the outer
 * of the two pieces after splitting.
 *
 * If the number of elements in "node" is smaller than the number of
 * elements in "sizes", then some splitting has occurred and we split
 * "sizes" in the same way.
 */
static __isl_give isl_schedule_node *scale_band(
	__isl_take isl_schedule_node *node, __isl_take isl_multi_val *sizes)
{
	int n, dim;

	n = isl_multi_val_dim(sizes, isl_dim_set);
	dim = isl_schedule_node_band_n_member(node);
	if (n > dim) {
		isl_multi_val *sizes2;

		sizes2 = isl_multi_val_copy(sizes);
		sizes = isl_multi_val_drop_dims(sizes,
						isl_dim_set, dim, n - dim);
		sizes2 = isl_multi_val_drop_dims(sizes2, isl_dim_set, 0, dim);
		node = isl_schedule_node_child(node, 0);
		node = isl_schedule_node_band_scale(node, sizes2);
		node = isl_schedule_node_parent(node);
	}

	return isl_schedule_node_band_scale(node, sizes);
}

/* Return an isl_multi_aff, with as elements the parameters in "space"
 * that have the names specified by the elements in "names".
 * If (some of) these parameters do not already appear in "space",
 * then they are added first.
 */
static __isl_give isl_multi_aff *parameter_vector(__isl_take isl_space *space,
	__isl_keep isl_id_list *names)
{
	int i, n;
	isl_local_space *ls;
	isl_multi_aff *ma;

	if (!names)
		space = isl_space_free(space);

	n = isl_id_list_n_id(names);
	for (i = 0; i < n; ++i) {
		int pos;
		isl_id *id;

		id = isl_id_list_get_id(names, i);
		pos = isl_space_find_dim_by_id(space, isl_dim_param, id);
		if (pos >= 0) {
			isl_id_free(id);
			continue;
		}
		pos = isl_space_dim(space, isl_dim_param);
		space = isl_space_add_dims(space, isl_dim_param, 1);
		space = isl_space_set_dim_id(space, isl_dim_param, pos, id);
	}
	ma = isl_multi_aff_zero(isl_space_copy(space));
	ls = isl_local_space_from_space(isl_space_domain(space));
	for (i = 0; i < n; ++i) {
		int pos;
		isl_id *id;
		isl_aff *aff;

		id = isl_id_list_get_id(names, i);
		pos = isl_space_find_dim_by_id(space, isl_dim_param, id);
		isl_id_free(id);
		aff = isl_aff_var_on_domain(isl_local_space_copy(ls),
					    isl_dim_param, pos);
		ma = isl_multi_aff_set_aff(ma, i, aff);
	}
	isl_local_space_free(ls);

	return ma;
}

/* Return constraints on the domain elements that equate a sequence of
 * parameters called "names", to the partial schedule
 * of "node" modulo the integers in "size".
 * The number of elements in the array "size" should be equal
 * to the number of members of the band node "node" and
 * to the number of elements in "names".
 */
static __isl_give isl_union_set *set_schedule_modulo(
	__isl_keep isl_schedule_node *node, __isl_keep isl_id_list *names,
	int *size)
{
	isl_space *space;
	isl_multi_aff *ma;
	isl_multi_union_pw_aff *mupa, *mupa2;
	isl_multi_val *mv;
	isl_union_set *domain;

	if (!node)
		return NULL;
	if (isl_schedule_node_band_n_member(node) == 0)
		return isl_schedule_node_get_universe_domain(node);

	mupa = isl_schedule_node_band_get_partial_schedule(node);
	mv = construct_band_tiles_sizes(node, size);
	mupa = isl_multi_union_pw_aff_mod_multi_val(mupa, mv);

	space = isl_multi_union_pw_aff_get_space(mupa);
	ma = parameter_vector(space, names);

	domain = isl_schedule_node_get_universe_domain(node);

	mupa2 = isl_multi_union_pw_aff_multi_aff_on_domain(domain, ma);
	mupa = isl_multi_union_pw_aff_sub(mupa, mupa2);

	return isl_multi_union_pw_aff_zero_union_set(mupa);
}

/* Insert a context node at "node" introducing the block and thread
 * identifiers along with their bounds, which are stored in kernel->grid_size
 * and kernel->block_dim.
 * Note that the bounds on the block identifiers may implicitly impose
 * constraints on the parameters.  A guard needs to be inserted
 * in the schedule tree to ensure that those bounds hold at "node".
 * This guard is inserted in insert_guard.
 */
static __isl_give isl_schedule_node *insert_context(struct ppcg_kernel *kernel,
	__isl_take isl_schedule_node *node)
{
	isl_set *context;

	context = isl_set_universe(isl_set_get_space(kernel->context));

	context = add_bounded_parameters_dynamic(context,
					kernel->grid_size, kernel->block_ids);
	context = add_bounded_parameters(context,
					kernel->block_dim, kernel->thread_ids);

	node = isl_schedule_node_insert_context(node, context);

	return node;
}

/* Insert a guard that eliminates kernel launches where the kernel
 * obviously does not have any work to do.
 *
 * In particular, eliminate kernel launches where there are obviously
 * zero blocks.
 * Use the same block size constraints that are used to create the context
 * to ensure that all constraints implicit in the constructed context
 * are imposed by the guard.
 *
 * Additionally, add other constraints that are valid
 * for each executed instance ("context"), as long as this does not result
 * in a disjunction.
 */
static __isl_give isl_schedule_node *insert_guard(
	__isl_take isl_schedule_node *node, __isl_keep isl_set *context,
	__isl_keep isl_multi_pw_aff *size, struct ppcg_scop *scop)
{
	unsigned nparam, n;
	isl_set *guard;
	isl_id_list *ids;

	guard = isl_set_copy(context);
	guard = isl_set_compute_divs(guard);
	guard = isl_set_from_basic_set(isl_set_simple_hull(guard));

	nparam = isl_set_dim(guard, isl_dim_param);
	n = isl_multi_pw_aff_dim(size, isl_dim_out);
	ids = ppcg_scop_generate_names(scop, n, "__ppcg_tmp");
	guard = add_bounded_parameters_dynamic(guard, size, ids);
	isl_id_list_free(ids);
	guard = isl_set_project_out(guard, isl_dim_param, nparam, n);

	node = isl_schedule_node_insert_guard(node, guard);

	return node;
}

/* Mark all dimensions in the current band node atomic.
 */
static __isl_give isl_schedule_node *atomic(__isl_take isl_schedule_node *node)
{
	int i, n;

	n = isl_schedule_node_band_n_member(node);
	for (i = 0; i < n; ++i)
		node = isl_schedule_node_band_member_set_ast_loop_type(node, i,
							isl_ast_loop_atomic);

	return node;
}

/* Mark "node" atomic, if it is a band node.
 * Do the same for all ancestors.
 * Return a pointer to "node" (in the updated schedule tree).
 */
static __isl_give isl_schedule_node *atomic_ancestors(
	__isl_take isl_schedule_node *node)
{
	int pos;

	if (!node)
		return NULL;
	if (!isl_schedule_node_has_parent(node))
		return node;

	pos = isl_schedule_node_get_child_position(node);
	node = isl_schedule_node_parent(node);
	if (isl_schedule_node_get_type(node) == isl_schedule_node_band)
		node = atomic(node);
	node = atomic_ancestors(node);
	node = isl_schedule_node_child(node, pos);

	return node;
}

/* Group the domain elements into a single space, named kernelX,
 * with X the kernel sequence number "kernel_id".
 */
static __isl_give isl_schedule_node *group_statements(
	__isl_take isl_schedule_node *node, int kernel_id)
{
	char buffer[20];
	isl_id *id;

	if (!node)
		return NULL;

	snprintf(buffer, sizeof(buffer), "kernel%d", kernel_id);
	id = isl_id_alloc(isl_schedule_node_get_ctx(node), buffer, NULL);
	return isl_schedule_node_group(node, id);
}

/* Create a ppcg_kernel representing the domain instances that reach "node"
 * and replace the subtree at "node" by a mark node pointing
 * to the ppcg_kernel.
 * The band that "node" points to is the band that needs to be mapped
 * to block identifiers.  The band that needs to be mapped to thread
 * identifiers should be marked by a "thread" mark by the caller.
 * If "scale" is set, then the band that "node" points to is scaled
 * by "sizes".
 *
 * Mark all outer band nodes as atomic to ensure each kernel is only
 * scheduled once.
 * If the domain elements that reach "node" live in more than one space,
 * then group the domain elements into a single space, named kernelX,
 * with X the kernel sequence number.
 *
 * Insert a guard node governing the kernel node to ensure that
 * no kernels with zero blocks are launched.
 *
 * Temporarily adjust the schedule tree underneath the kernel mark as follows.
 * Insert a context node describing the block and thread
 * identifiers inside the kernel mark.
 * The context node needs to be inserted after the effective block size
 * has been determined such that the bounds on the thread identifiers
 * would reflect the effective block size.
 * Insert a filter node inside the context node mapping the statement
 * instances to block identifiers.  In particular, the block identifiers
 * are equated to the partial schedule of band that was marked for mapping
 * to blocks modulo the grid size.
 * Insert a filter node inside the "thread" mark mapping the statement
 * instances to thread identifiers.  In particular, the thread identifiers
 * are equated to the partial schedule of band that was marked for mapping
 * to threads modulo the block size.
 *
 * Store a pointer to the created ppcg_kernel in gen->kernel.
 *
 * We keep a copy of the isl_id that points to the kernel to ensure
 * that the kernel does not get destroyed if the schedule node
 * is freed due to some error condition.
 */
static __isl_give isl_schedule_node *create_kernel(struct gpu_gen *gen,
	__isl_take isl_schedule_node *node, int scale,
	__isl_keep isl_multi_val *sizes)
{
	struct ppcg_kernel *kernel;
	isl_id *id;
	isl_schedule_node *node_thread;
	isl_union_set *domain;
	int single_statement;

	kernel = isl_calloc_type(gen->ctx, struct ppcg_kernel);
	kernel = ppcg_kernel_create_local_arrays(kernel, gen->prog);
	if (!kernel)
		return isl_schedule_node_free(node);

	domain = isl_schedule_node_get_domain(node);
	single_statement = isl_union_set_n_set(domain) == 1;

	kernel->ctx = gen->ctx;
	kernel->options = gen->options;
	kernel->context = extract_context(node, gen->prog);
	kernel->core = isl_union_set_universe(isl_union_set_copy(domain));
	kernel->arrays = accessed_by_domain(isl_union_set_copy(domain),
						gen->prog);
	kernel->tile_len = isl_schedule_node_band_n_member(node);
	kernel->n_parallel = n_outer_coincidence(node);
	kernel->n_grid = kernel->n_parallel;
	node_thread = isl_schedule_node_copy(node);
	node_thread = gpu_tree_move_down_to_thread(node_thread, kernel->core);
	node_thread = isl_schedule_node_child(node_thread, 0);
	kernel->n_block = n_outer_coincidence(node_thread);
	isl_schedule_node_free(node_thread);
	kernel->id = gen->kernel_id++;
	read_grid_and_block_sizes(kernel, gen);

	gen->kernel = kernel;

	node = atomic_ancestors(node);

	id = isl_id_alloc(gen->ctx, "kernel", kernel);
	id = isl_id_set_free_user(id, &ppcg_kernel_free_wrap);
	node = isl_schedule_node_insert_mark(node, isl_id_copy(id));

	if (!single_statement)
		node = group_statements(node, kernel->id);

	node = isl_schedule_node_child(node, 0);
	node = split_band(node, kernel->n_grid);
	kernel->block_ids = ppcg_scop_generate_names(gen->prog->scop,
						kernel->n_grid, "b");
	kernel->block_filter = set_schedule_modulo(node, kernel->block_ids,
						kernel->grid_dim);
	kernel->grid_size = extract_grid_size(kernel,
						isl_union_set_copy(domain));
	if (!kernel->options->wrap)
		node = snap_band_to_sizes(node, kernel->grid_dim,
						kernel->options);
	if (scale)
		node = scale_band(node, isl_multi_val_copy(sizes));
	node = isl_schedule_node_parent(node);
	if (!single_statement)
		node = isl_schedule_node_parent(node);
	node = insert_guard(node, kernel->context, kernel->grid_size,
				gen->prog->scop);
	node = gpu_tree_move_down_to_thread(node, kernel->core);
	node = isl_schedule_node_child(node, 0);
	node = split_band(node, kernel->n_block);
	kernel->thread_ids = ppcg_scop_generate_names(gen->prog->scop,
						kernel->n_block, "t");
	kernel->thread_filter = set_schedule_modulo(node, kernel->thread_ids,
						kernel->block_dim);
	extract_block_size(kernel, domain);

	node = gpu_tree_move_up_to_kernel(node);
	node = isl_schedule_node_child(node, 0);
	node = insert_context(kernel, node);
	node = isl_schedule_node_child(node, 0);
	node = isl_schedule_node_insert_filter(node,
				    isl_union_set_copy(kernel->block_filter));

	node = gpu_tree_move_down_to_thread(node, kernel->core);
	node = isl_schedule_node_child(node, 0);
	if (!kernel->options->wrap)
		node = snap_band_to_sizes(node, kernel->block_dim,
						kernel->options);
	node = isl_schedule_node_insert_filter(node,
				    isl_union_set_copy(kernel->thread_filter));

	node = gpu_tree_move_up_to_kernel(node);

	node = isl_schedule_node_child(node, 0);
	node = isl_schedule_node_cut(node);
	node = isl_schedule_node_parent(node);

	if (!single_statement)
		node = isl_schedule_node_parent(node);
	node = isl_schedule_node_parent(node);

	isl_id_free(id);
	return node;
}

/* Insert a zero-dimensional permutable band at "node".
 */
static __isl_give isl_schedule_node *insert_empty_permutable_band(
	__isl_take isl_schedule_node *node)
{
	isl_space *space;
	isl_schedule *schedule;
	isl_union_set *domain;
	isl_multi_union_pw_aff *mupa;

	schedule = isl_schedule_node_get_schedule(node);
	domain = isl_schedule_get_domain(schedule);
	space = isl_union_set_get_space(domain);
	isl_union_set_free(domain);
	isl_schedule_free(schedule);

	space = isl_space_set_from_params(space);
	mupa = isl_multi_union_pw_aff_zero(space);
	node = isl_schedule_node_insert_partial_schedule(node, mupa);
	node = isl_schedule_node_band_set_permutable(node, 1);

	return node;
}

/* Mark "node" as outer permutable.
 *
 * If "node" originally points to a leaf, then insert a zero-dimensional
 * permutable band such that we can assume that "node" always
 * points to a band node.
 *
 * Tile "node" using user specified tile sizes, after splitting the band
 * if the number of specified tile sizes is smaller than the dimension
 * of the band.  Mark the point band of this tiling as the band that
 * needs to be mapped to threads.
 * Create a kernel representing the domain instances that reach "node" and
 * replace the band node with a mark node pointing to the kernel.
 */
static __isl_give isl_schedule_node *mark_outer_permutable(
	struct gpu_gen *gen, __isl_take isl_schedule_node *node)
{
	struct ppcg_kernel *kernel;
	int scale;
	int tile_len;
	int *tile_size;
	isl_id *id;
	isl_multi_val *sizes;

	if (isl_schedule_node_get_type(node) == isl_schedule_node_leaf)
		node = insert_empty_permutable_band(node);

	tile_len = isl_schedule_node_band_n_member(node);
	tile_size = read_tile_sizes(gen, &tile_len);
	if (!tile_size)
		return isl_schedule_node_free(node);
	if (tile_len < isl_schedule_node_band_n_member(node))
		node = isl_schedule_node_band_split(node, tile_len);
	sizes = construct_band_tiles_sizes(node, tile_size);
	node = tile_band(node, isl_multi_val_copy(sizes));
	node = isl_schedule_node_child(node, 0);
	id = isl_id_alloc(gen->ctx, "thread", NULL);
	node = isl_schedule_node_insert_mark(node, id);
	node = isl_schedule_node_parent(node);

	scale = gen->options->scale_tile_loops;
	node = create_kernel(gen, node, scale, sizes);
	isl_multi_val_free(sizes);
	if (!node)
		return NULL;
	kernel = gen->kernel;
	kernel->tile_len = tile_len;
	kernel->tile_size = tile_size;

	return node;
}

static __isl_give isl_schedule_node *select_outer_band(struct gpu_gen *gen,
	__isl_take isl_schedule_node *node, int pos, struct band_info *info);

/* Check if this band node is tilable and has any parallel loops.  If so,
 * take it as the outermost tilable band.  If not, continue looking for the
 * outermost tilable band in the children of the current band.
 * Return a pointer to the same node in a tree where all outermost tilable
 * bands in the current subtree have been replaced by mark nodes
 * containing a pointer to a ppcg_kernel object.
 */
static __isl_give isl_schedule_node *band_select_outer_band(struct gpu_gen *gen,
	__isl_take isl_schedule_node *node, int pos, struct band_info *info)
{
	int n = isl_schedule_node_band_n_member(node);
	int n_parallel;

	n_parallel = n_outer_coincidence(node);

	if (!isl_schedule_node_band_get_permutable(node) || n_parallel == 0) {
		node = isl_schedule_node_child(node, 0);
		node = select_outer_band(gen, node, pos + n, info);
		return isl_schedule_node_parent(node);
	}

	gen->any_parallelism = 1;
	info->gen = gen;
	info->tile_first = pos;
	info->prefix = isl_schedule_node_get_prefix_schedule_union_map(node);
	info->suffix = isl_schedule_node_get_subtree_schedule_union_map(node);

	node = mark_outer_permutable(gen, node);

	return node;
}

/* Extend "umap" with coordinates with fixed value "val"
 * to a total length of "dst_len", assuming the original dimension is "src_len".
 */
static __isl_give isl_union_map *extend_range(
	__isl_take isl_union_map *umap, int src_len, int dst_len, int val)
{
	isl_space *dim;
	isl_map *map;
	int i;

	dim = isl_union_map_get_space(umap);
	map = isl_map_reverse(projection(dim, dst_len, src_len));
	for (i = src_len; i < dst_len; ++i)
		map = isl_map_fix_si(map, isl_dim_out, i, val);

	umap = isl_union_map_apply_range(umap, isl_union_map_from_map(map));

	return umap;
}

/* Select the outermost bands in the elements of the sequence or set
 * node "node", align their prefix schedules and combine the resulting
 * prefix and suffix schedules into a single pair of prefix and
 * suffix schedules for the entire list.
 * Return a pointer to the same node in a tree where all outermost tilable
 * bands in the current subtree have been replaced by mark nodes
 * containing a pointer to a ppcg_kernel object.
 */
static __isl_give isl_schedule_node *list_select_outer_band(
	struct gpu_gen *gen, __isl_take isl_schedule_node *node, int pos,
	struct band_info *list_info)
{
	int i;
	int n = isl_schedule_node_n_children(node);
	isl_ctx *ctx = isl_schedule_node_get_ctx(node);
	struct band_info *info;
	int max_tile_first;
	isl_union_map *prefix;
	isl_union_map *suffix;

	assert(n >= 1);
	info = isl_calloc_array(ctx, struct band_info, n);
	assert(info);

	max_tile_first = 0;
	for (i = 0; i < n; ++i) {
		node = isl_schedule_node_child(node, i);
		node = select_outer_band(gen, node, pos, &info[i]);
		if (info[i].tile_first > max_tile_first)
			max_tile_first = info[i].tile_first;
		node = isl_schedule_node_parent(node);
	}

	for (i = 0; i < n; ++i) {
		if (info[i].tile_first == max_tile_first)
			continue;
		info[i].prefix = extend_range(info[i].prefix,
					info[i].tile_first, max_tile_first, 0);
		info[i].tile_first = max_tile_first;
	}

	prefix = info[0].prefix;
	suffix = info[0].suffix;

	for (i = 1; i < n; ++i) {
		prefix = isl_union_map_union(prefix, info[i].prefix);
		suffix = isl_union_map_union(suffix, info[i].suffix);
	}

	list_info->tile_first = info[0].tile_first;
	list_info->prefix = prefix;
	list_info->suffix = suffix;

	free(info);
	return node;
}

/* If we reach a leaf node, then we have not found any outer tilable
 * band with parallel loops, so consider the leaf node as the outermost
 * tilable band.
 * Return a pointer to a mark node containing a pointer
 * to a ppcg_kernel object inserted at the original leaf node.
 */
static __isl_give isl_schedule_node *leaf_select_outer_band(struct gpu_gen *gen,
	__isl_take isl_schedule_node *node, int pos, struct band_info *info)
{
	info->gen = gen;
	info->tile_first = pos;
	info->prefix = isl_schedule_node_get_prefix_schedule_union_map(node);
	info->suffix = isl_schedule_node_get_subtree_schedule_union_map(node);

	node = mark_outer_permutable(gen, node);

	return node;
}

/* Select the outermost tilable band in the subtree that "node" points to and
 * return a pointer to the same node in a tree where all outermost tilable
 * bands in the current subtree have been replaced by mark nodes
 * containing a pointer to a ppcg_kernel object.
 */
static __isl_give isl_schedule_node *select_outer_band(struct gpu_gen *gen,
	__isl_take isl_schedule_node *node, int pos, struct band_info *info)
{
	enum isl_schedule_node_type type;

	type = isl_schedule_node_get_type(node);
	switch (type) {
	case isl_schedule_node_domain:
	case isl_schedule_node_filter:
		node = isl_schedule_node_child(node, 0);
		node = select_outer_band(gen, node, pos, info);
		return isl_schedule_node_parent(node);
	case isl_schedule_node_leaf:
		return leaf_select_outer_band(gen, node, pos, info);
	case isl_schedule_node_band:
		return band_select_outer_band(gen, node, pos, info);
	case isl_schedule_node_set:
	case isl_schedule_node_sequence:
		return list_select_outer_band(gen, node, pos, info);
	default:
		isl_die(isl_schedule_node_get_ctx(node),
			isl_error_unsupported, "unhandled schedule node type",
			node = node);
	case isl_schedule_node_error:
		info->prefix = NULL;
		info->suffix = NULL;
		break;
	}

	return isl_schedule_node_free(node);
}

/* Select the outermost tilable band that (by construction)
 * has at least one parallel loop.
 * The starting position of the aligned band is stored in the pair
 * gen->tile_first.
 * The sizes and number of parallel loops may be different in different
 * parts of the band forest and are therefore stored in the gpu_stmts.
 *
 * Return the complete schedule, with the tilable bands aligned
 * at gen->tile_first and padded with zero, if needed.
 * Store a schedule tree corresponding to the outer gen->tile_first
 * dimensions, with mark nodes containing pointers to ppcg_kernel objects,
 * in gen->host_schedule.
 */
static __isl_give isl_union_map *select_outer_tilable_band(struct gpu_gen *gen,
	__isl_keep isl_schedule *schedule)
{
	isl_schedule_node *node;
	struct band_info info;

	node = isl_schedule_get_root(schedule);
	node = select_outer_band(gen, node, 0, &info);
	gen->host_schedule = isl_schedule_node_get_schedule(node);
	isl_schedule_node_free(node);

	gen->tile_first = info.tile_first;
	info.suffix = align_range(info.suffix);

	return isl_union_map_flat_range_product(info.prefix, info.suffix);
}

/* Set gen->untiled_len to the number of scheduling dimensions
 * for the schedule of the first domain.
 * We assume here that this number is the same for all domains.
 */
static int set_untiled_len(__isl_take isl_map *map, void *user)
{
	unsigned *untiled_len = user;

	*untiled_len = isl_map_dim(map, isl_dim_out);

	isl_map_free(map);
	return -1;
}

/* Compute an appropriate schedule based on the accesses in
 * gen->read and gen->write.
 *
 * We use the dependences in gen->prog->scop to compute
 * a schedule that has a parallel loop in each tilable band.
 * Finally, we select the outermost tilable band.
 *
 * If live range reordering is allowed, then we need to make sure
 * that live ranges on arrays are not run in parallel since doing
 * so would require array expansion.  We therefore add the array
 * order dependences to the coincidence dependences.  Non-zero array
 * order dependences will then prevent a schedule dimension from being
 * considered parallel.
 * Live ranges derived from scalars are allowed to be run in parallel
 * since we force the scalars to be mapped to private memory in
 * check_scalar_live_ranges.
 * If live range reordering is allowed, then the false dependences
 * are not added to the validity constraints as that would prevent
 * reordering.  Instead, the external false dependences that enforce that reads
 * from potentially live-in data precede any later write and
 * that writes of potentially live-out data follow any other earlier write
 * are added to the validity and the coincidence constraints.
 * The false dependences are still added to the proximity constraints
 * for consistency with the case where live range reordering is not allowed.
 * The coincidence constraints then consist of flow dependences,
 * external false dependences and array order dependences.
 * The independences can be filtered out from the first two sets.
 * They have already been filtered out from the array order dependences
 * on a per array basis in collect_order_dependences.
 * There is no need for a per array handling of the other two sets
 * as there should be no flow or external false dependence on local
 * variables that can be filtered out.
 */
static void compute_schedule(struct gpu_gen *gen)
{
	isl_union_set *domain;
	isl_union_map *dep_raw, *dep;
	isl_union_map *validity, *proximity, *coincidence;
	isl_union_map *sched;
	isl_schedule_constraints *sc;
	isl_schedule *schedule;

	domain = isl_union_set_copy(gen->prog->scop->domain);
	sc = isl_schedule_constraints_on_domain(isl_union_set_copy(domain));
	sc = isl_schedule_constraints_set_context(sc,
				isl_set_copy(gen->prog->scop->context));
	if (gen->options->live_range_reordering) {
		sc = isl_schedule_constraints_set_conditional_validity(sc,
			isl_union_map_copy(gen->prog->scop->tagged_dep_flow),
			isl_union_map_copy(gen->prog->scop->tagged_dep_order));
		proximity = isl_union_map_copy(gen->prog->scop->dep_flow);
		validity = isl_union_map_copy(proximity);
		validity = isl_union_map_union(validity,
			    isl_union_map_copy(gen->prog->scop->dep_forced));
		proximity = isl_union_map_union(proximity,
			    isl_union_map_copy(gen->prog->scop->dep_false));
		coincidence = isl_union_map_copy(validity);
		coincidence = isl_union_map_subtract(coincidence,
			isl_union_map_copy(gen->prog->scop->independence));
		coincidence = isl_union_map_union(coincidence,
				isl_union_map_copy(gen->prog->array_order));
	} else {
		dep_raw = isl_union_map_copy(gen->prog->scop->dep_flow);
		dep = isl_union_map_copy(gen->prog->scop->dep_false);
		dep = isl_union_map_union(dep, dep_raw);
		dep = isl_union_map_coalesce(dep);
		proximity = isl_union_map_copy(dep);
		coincidence = isl_union_map_copy(dep);
		validity = dep;
	}
	sc = isl_schedule_constraints_set_validity(sc, validity);
	sc = isl_schedule_constraints_set_coincidence(sc, coincidence);
	sc = isl_schedule_constraints_set_proximity(sc, proximity);

	if (gen->options->debug->dump_schedule_constraints)
		isl_schedule_constraints_dump(sc);
	schedule = isl_schedule_constraints_compute_schedule(sc);
	if (gen->options->debug->dump_schedule)
		isl_schedule_dump(schedule);

	sched = select_outer_tilable_band(gen, schedule);

	isl_union_map_foreach_map(sched, &set_untiled_len, &gen->untiled_len);
	sched = isl_union_map_intersect_domain(sched, domain);
	gen->sched = sched;

	isl_schedule_free(schedule);
}

/* Compute the sets of outer array elements that need to be copied in and out.
 *
 * In particular, for each array that is possibly written anywhere in
 * gen->prog and that is visible outside the corresponding scop,
 * we copy out its entire extent.
 *
 * Any array elements that is read without first being written needs
 * to be copied in. Furthermore, if there are any array elements that
 * are copied out, but that may not be written inside gen->prog, then
 * they also need to be copied in to ensure that the value after execution
 * is the same as the value before execution, at least for those array
 * elements that may have their values preserved by the scop.
 * In case the array elements are structures, we need to take into
 * account that all members of the structures need to be written
 * by gen->prog before we can avoid copying the data structure in.
 *
 * While computing the set of array elements that are copied out but
 * not necessarily written, we intersect both sets with the context.
 * This helps in those cases where the arrays are declared with a fixed size,
 * while the accesses are parametric and the context assigns a fixed value
 * to the parameters.
 *
 * If an element from a local array is read without first being written,
 * then there is no point in copying it in since it cannot have been
 * written prior to the scop.  Warn about the uninitialized read instead.
 */
static void compute_copy_in_and_out(struct gpu_gen *gen)
{
	int i;
	isl_union_set *local;
	isl_union_set *may_write, *must_write;
	isl_union_set *copy_in, *copy_out;
	isl_union_set *not_written;
	isl_union_map *uninitialized;
	isl_union_map *local_uninitialized;

	must_write = isl_union_map_range(
				isl_union_map_copy(gen->prog->must_write));
	must_write = isl_union_set_intersect_params(must_write,
					    isl_set_copy(gen->prog->context));
	may_write = isl_union_map_range(
				isl_union_map_copy(gen->prog->may_write));
	may_write = isl_union_set_intersect_params(may_write,
					    isl_set_copy(gen->prog->context));
	may_write = isl_union_set_universe(may_write);
	may_write = isl_union_set_apply(may_write,
				    isl_union_map_copy(gen->prog->to_outer));
	copy_out = isl_union_set_empty(isl_union_set_get_space(may_write));
	local = isl_union_set_copy(copy_out);

	for (i = 0; i < gen->prog->n_array; ++i) {
		isl_space *space;
		isl_set *write_i;
		int empty;

		space = isl_space_copy(gen->prog->array[i].space);

		if (gen->prog->array[i].local) {
			isl_set *set;

			set = isl_set_universe(space);
			local = isl_union_set_add_set(local, set);
			continue;
		}

		write_i = isl_union_set_extract_set(may_write, space);
		empty = isl_set_plain_is_empty(write_i);
		isl_set_free(write_i);
		if (empty)
			continue;

		write_i = isl_set_copy(gen->prog->array[i].extent);
		copy_out = isl_union_set_add_set(copy_out, write_i);
	}
	isl_union_set_free(may_write);

	copy_out = isl_union_set_intersect_params(copy_out,
					    isl_set_copy(gen->prog->context));

	gen->prog->copy_out = isl_union_set_copy(copy_out);

	copy_out = isl_union_set_apply(copy_out,
				    isl_union_map_copy(gen->prog->to_inner));
	copy_out = isl_union_set_intersect(copy_out,
				    isl_union_set_copy(gen->prog->may_persist));
	not_written = isl_union_set_subtract(copy_out, must_write);

	uninitialized = isl_union_map_copy(gen->prog->scop->live_in);
	local_uninitialized = isl_union_map_copy(uninitialized);

	local = isl_union_set_apply(local,
				    isl_union_map_copy(gen->prog->to_inner));
	local_uninitialized = isl_union_map_intersect_range(local_uninitialized,
							    local);
	if (!isl_union_map_is_empty(local_uninitialized)) {
		fprintf(stderr,
			"possibly uninitialized reads (not copied in):\n");
		isl_union_map_dump(local_uninitialized);
	}
	uninitialized = isl_union_map_subtract(uninitialized,
						local_uninitialized);
	copy_in = isl_union_map_range(uninitialized);
	copy_in = isl_union_set_union(copy_in, not_written);
	copy_in = isl_union_set_apply(copy_in,
				    isl_union_map_copy(gen->prog->to_outer));

	gen->prog->copy_in = copy_in;
}

/* Internal data structure for extract_access.
 * "next_access" points to the end of a linked list that is extended
 * by extract_access.
 * "single_expression" is set if the access expressions belong to
 * an expression statement (i.e., a statement without internal control).
 * "any_to_outer" maps all intermediate arrays to their outer arrays.
 */
struct ppcg_extract_access_data {
	struct gpu_stmt_access **next_access;
	int single_expression;
	isl_union_map *any_to_outer;
};

/* Given a tagged access relation to a single array "tagged", extract it
 * as a map, taking into account that the input may be empty.
 * If the access relation is empty, then it does not contain
 * any space information, so we try to recover it from the index
 * expression.
 * The space of the index expression is of the form I -> A,
 * with I the statement instances and A the array, or [I -> F] -> A,
 * with F the filters corresponding to arguments.
 * We first drop F, if present, obtaining I -> A.
 * Then we construct I -> R, with R the reference tag,
 * combine the two into I -> [R -> A] and uncurry to obtain
 * the final result [I -> R] -> A.
 * Note that the index expression may have a lower dimension
 * than that of the array, but this dimension is not used
 * if the access relation is empty.
 */
static __isl_give isl_map *extract_single_tagged_access(
	__isl_take isl_union_map *tagged, __isl_keep pet_expr *expr)
{
	int empty;
	isl_id *id;
	isl_space *space, *space2;
	isl_multi_pw_aff *index;

	empty = isl_union_map_is_empty(tagged);
	if (empty < 0)
		goto error;
	if (!empty)
		return isl_map_from_union_map(tagged);
	isl_union_map_free(tagged);

	index = pet_expr_access_get_index(expr);
	space = isl_multi_pw_aff_get_space(index);
	isl_multi_pw_aff_free(index);
	if (isl_space_domain_is_wrapping(space))
		space = isl_space_domain_factor_domain(space);
	space2 = isl_space_copy(space);
	space2 = isl_space_from_domain(isl_space_domain(space));
	id = pet_expr_access_get_ref_id(expr);
	space2 = isl_space_set_tuple_id(space2, isl_dim_out, id);
	space = isl_space_range_product(space2, space);
	space = isl_space_uncurry(space);

	return isl_map_empty(space);
error:
	isl_union_map_free(tagged);
	return NULL;
}

/* Extract a gpu_stmt_access from "expr", append it to the list
 * that ends in *data->next_access and update the end of the list.
 * If the access expression performs a write, then it is considered
 * exact only if it appears in a single expression statement and
 * if its may access relation is equal to its must access relation.
 *
 * The combined set of may accesses may be union if member accesses
 * are involved, but the entire set is derived from a single reference and
 * therefore from a single index expression.  These accesses therefore
 * all map to the same outer array.
 */
static int extract_access(__isl_keep pet_expr *expr, void *user)
{
	struct ppcg_extract_access_data *data = user;
	isl_union_map *tagged;
	struct gpu_stmt_access *access;
	isl_ctx *ctx = pet_expr_get_ctx(expr);
	isl_multi_pw_aff *index;

	access = isl_alloc_type(ctx, struct gpu_stmt_access);
	assert(access);
	access->next = NULL;
	access->read = pet_expr_access_is_read(expr);
	access->write = pet_expr_access_is_write(expr);
	tagged = pet_expr_access_get_tagged_may_read(expr);
	tagged = isl_union_map_union(tagged,
				pet_expr_access_get_tagged_may_write(expr));
	tagged = isl_union_map_apply_range(tagged,
					isl_union_map_copy(data->any_to_outer));
	if (!access->write) {
		access->exact_write = 1;
	} else if (!data->single_expression) {
		access->exact_write = 0;
	} else {
		isl_union_map *must, *may;
		may = isl_union_map_copy(tagged);
		may = isl_union_map_domain_factor_domain(may);
		must = pet_expr_access_get_must_write(expr);
		access->exact_write = isl_union_map_is_equal(must, may);
		isl_union_map_free(must);
		isl_union_map_free(may);
	}
	index = pet_expr_access_get_index(expr);
	access->n_index = isl_multi_pw_aff_dim(index, isl_dim_out);
	isl_multi_pw_aff_free(index);
	access->ref_id = pet_expr_access_get_ref_id(expr);
	access->tagged_access = extract_single_tagged_access(tagged, expr);
	access->access = isl_map_copy(access->tagged_access);
	access->access = isl_map_domain_factor_domain(access->access);

	*data->next_access = access;
	data->next_access = &(*data->next_access)->next;

	if (!access->access)
		return -1;

	return 0;
}

/* Construct a linked list of gpu_stmt_access objects,
 * one for each access expression in the statement body.
 * "any_to_outer" maps all intermediate arrays to their outer arrays.
 */
static int pet_stmt_extract_accesses(struct gpu_stmt *stmt,
	__isl_keep isl_union_map *any_to_outer)
{
	struct ppcg_extract_access_data data;

	stmt->accesses = NULL;
	data.next_access = &stmt->accesses;
	data.single_expression =
		pet_tree_get_type(stmt->stmt->body) == pet_tree_expr;
	data.any_to_outer = any_to_outer;
	return pet_tree_foreach_access_expr(stmt->stmt->body,
						&extract_access, &data);
}

/* Return an array of gpu_stmt representing the statements in "scop".
 */
static struct gpu_stmt *extract_stmts(isl_ctx *ctx, struct ppcg_scop *scop,
	__isl_keep isl_set *context, __isl_keep isl_union_map *any_to_outer)
{
	int i;
	struct gpu_stmt *stmts;

	stmts = isl_calloc_array(ctx, struct gpu_stmt, scop->pet->n_stmt);
	if (!stmts)
		return NULL;

	for (i = 0; i < scop->pet->n_stmt; ++i) {
		struct gpu_stmt *s = &stmts[i];

		s->id = isl_set_get_tuple_id(scop->pet->stmts[i]->domain);
		s->stmt = scop->pet->stmts[i];
		if (pet_stmt_extract_accesses(s, any_to_outer) < 0)
			return free_stmts(stmts, i + 1);
	}

	return stmts;
}

/* Callback for ppcg_print_guarded that calls the callback for generate_gpu.
 */
static __isl_give isl_printer *print_gpu(__isl_take isl_printer *p, void *user)
{
	struct gpu_gen *gen = user;

	return gen->print(p, gen->prog, gen->tree, &gen->types,
			    gen->print_user);
}

/* Generate CUDA code for "scop" and print it to "p".
 * After generating an AST for the transformed scop as explained below,
 * we call "gen->print" to print the AST in the desired output format
 * to "p".
 *
 * If it turns out that it does not make sense to generate GPU code,
 * then we generate CPU code instead.
 *
 * The GPU code is generated in a context where at least one
 * statement instance is executed.  The corresponding guard (if any) is printed
 * around the entire generated GPU code, except for the declaration
 * of the arrays that are visible outside of the scop and that therefore
 * cannot be declared inside the body of any possible guard.
 *
 * We first compute a schedule that respects the dependences
 * of the original program and select the outermost band
 * of tilable dimensions that has at least one parallel loop.
 * We then have three blocks of dimensions
 *
 *	H		B			G
 *
 * The tilable band "B" is first tiled according to "tile" sizes, resulting
 * in
 *
 *	H	T		P		G
 *
 * For each iteration of the T loop and for each array, we compute
 * the array elements accessed by that iteration, construct a rectangular
 * box around it and shift it to the origin.  The result is used
 * as shared memory for the array.
 *
 * We then split off at most 2 parallel loops from the T loops and
 * at most 3 parallel loops from the P loops
 *
 *	H	T1	T2	P1	P2	G
 *
 * The T1/P1 loops are then tiled or "wrapped" over the blocks/threads,
 * according to "grid"/"block" sizes.
 *
 *	H	T1T T1P	T2	P1T P1P	P2	G
 *
 * Finally, the T1P and P1P iterators are equated to the block and
 * thread dimensions respectively and so are effectively removed.
 * The H loops are run on the host.  The T1T, T2, P1T, P2 and G loops
 * are run on the GPU.
 *
 * Code is generated in three stages.  We first generate code for the
 * host (the H loops), with iterators h%d.  Then, for each leaf node
 * of the resulting AST, we generate code for the shared loops (up to
 * and including T2), with iterators g%d and after equating the H loops
 * to h%d parameters and the T1P loops to the block dimensions.
 * Finally, we generate code for the remaining loops in a similar fashion.
 */
static __isl_give isl_printer *generate(__isl_take isl_printer *p,
	struct gpu_gen *gen, struct ppcg_scop *scop,
	struct ppcg_options *options)
{
	struct gpu_prog *prog;
	isl_ctx *ctx;
	isl_set *context, *guard;

	if (!scop)
		return isl_printer_free(p);

	ctx = isl_printer_get_ctx(p);
	prog = gpu_prog_alloc(ctx, scop);
	if (!prog)
		return isl_printer_free(p);

	context = isl_set_copy(prog->context);
	guard = isl_union_set_params(isl_union_set_copy(prog->scop->domain));
	prog->context = isl_set_intersect(prog->context, isl_set_copy(guard));

	gen->prog = prog;
	gen->any_parallelism = 0;
	compute_schedule(gen);

	if (!gen->any_parallelism) {
		isl_set_free(context);
		isl_set_free(guard);
		p = print_cpu(p, scop, options);
	} else {
		compute_copy_in_and_out(gen);
		gen->tree = generate_host_code(gen);
		p = ppcg_print_exposed_declarations(p, prog->scop);
		p = ppcg_print_guarded(p, guard, context, &print_gpu, gen);
		isl_ast_node_free(gen->tree);
	}

	isl_union_map_free(gen->sched);
	isl_schedule_free(gen->host_schedule);

	gpu_prog_free(prog);

	return p;
}

/* Wrapper around generate for use as a ppcg_transform callback.
 */
static __isl_give isl_printer *generate_wrap(__isl_take isl_printer *p,
	struct ppcg_scop *scop, void *user)
{
	struct gpu_gen *gen = user;

	return generate(p, gen, scop, gen->options);
}

/* Transform the code in the file called "input" by replacing
 * all scops by corresponding GPU code and write the results to "out".
 */
int generate_gpu(isl_ctx *ctx, const char *input, FILE *out,
	struct ppcg_options *options,
	__isl_give isl_printer *(*print)(__isl_take isl_printer *p,
		struct gpu_prog *prog, __isl_keep isl_ast_node *tree,
		struct gpu_types *types, void *user), void *user)
{
	struct gpu_gen gen;
	int r;
	int i;

	gen.ctx = ctx;
	gen.sizes = extract_sizes_from_str(ctx, options->sizes);
	gen.options = options;
	gen.kernel_id = 0;
	gen.print = print;
	gen.print_user = user;
	gen.types.n = 0;
	gen.types.name = NULL;

	if (options->debug->dump_sizes) {
		isl_space *space = isl_space_params_alloc(ctx, 0);
		gen.used_sizes = isl_union_map_empty(space);
	}

	r = ppcg_transform(ctx, input, out, options, &generate_wrap, &gen);

	if (options->debug->dump_sizes) {
		isl_union_map_dump(gen.used_sizes);
		isl_union_map_free(gen.used_sizes);
	}

	isl_union_map_free(gen.sizes);
	for (i = 0; i < gen.types.n; ++i)
		free(gen.types.name[i]);
	free(gen.types.name);

	return r;
}

/* Compute the set of inner array elements that may have their values
 * preserved by "prog".  In particular, collect the array elements of
 * arrays that are not local to "prog" and remove those elements that
 * are definitely killed or definitely written by "prog".
 */
static __isl_give isl_union_set *compute_may_persist(struct gpu_prog *prog)
{
	int i;
	isl_union_set *may_persist, *killed;
	isl_union_map *must_kill;

	may_persist = isl_union_set_empty(isl_set_get_space(prog->context));
	for (i = 0; i < prog->n_array; ++i) {
		isl_set *extent;

		if (prog->array[i].local)
			continue;

		extent = isl_set_copy(prog->array[i].extent);
		may_persist = isl_union_set_add_set(may_persist, extent);
	}

	may_persist = isl_union_set_intersect_params(may_persist,
						isl_set_copy(prog->context));
	may_persist = isl_union_set_apply(may_persist,
					isl_union_map_copy(prog->to_inner));
	must_kill = isl_union_map_copy(prog->tagged_must_kill);
	killed = isl_union_map_range(must_kill);
	must_kill = isl_union_map_copy(prog->must_write);
	killed = isl_union_set_union(killed, isl_union_map_range(must_kill));

	may_persist = isl_union_set_subtract(may_persist, killed);
	return may_persist;
}

struct gpu_prog *gpu_prog_alloc(isl_ctx *ctx, struct ppcg_scop *scop)
{
	struct gpu_prog *prog;
	isl_space *space;
	isl_map *id;

	if (!scop)
		return NULL;

	prog = isl_calloc_type(ctx, struct gpu_prog);
	assert(prog);

	prog->ctx = ctx;
	prog->scop = scop;
	prog->context = isl_set_copy(scop->context);
	prog->n_stmts = scop->pet->n_stmt;
	prog->any_to_outer = pet_scop_compute_outer_to_any(scop->pet);
	prog->any_to_outer = isl_union_map_reverse(prog->any_to_outer);
	space = isl_union_map_get_space(prog->any_to_outer);
	space = isl_space_set_from_params(space);
	space = isl_space_add_dims(space, isl_dim_set, 1);
	space = isl_space_map_from_set(space);
	id = isl_map_identity(space);
	prog->any_to_outer = isl_union_map_add_map(prog->any_to_outer, id);
	prog->stmts = extract_stmts(ctx, scop,
					prog->context, prog->any_to_outer);
	prog->read = isl_union_map_copy(scop->reads);
	prog->may_write = isl_union_map_copy(scop->may_writes);
	prog->must_write = isl_union_map_copy(scop->must_writes);
	prog->tagged_must_kill = isl_union_map_copy(scop->tagged_must_kills);
	prog->to_inner = pet_scop_compute_outer_to_inner(scop->pet);
	prog->to_outer = isl_union_map_copy(prog->to_inner);
	prog->to_outer = isl_union_map_reverse(prog->to_outer);

	if (!prog->stmts)
		return gpu_prog_free(prog);

	if (collect_array_info(prog) < 0)
		return gpu_prog_free(prog);
	prog->may_persist = compute_may_persist(prog);

	return prog;
}

void *gpu_prog_free(struct gpu_prog *prog)
{
	if (!prog)
		return NULL;
	free_array_info(prog);
	free_stmts(prog->stmts, prog->n_stmts);
	isl_union_map_free(prog->any_to_outer);
	isl_union_map_free(prog->to_outer);
	isl_union_map_free(prog->to_inner);
	isl_union_set_free(prog->copy_in);
	isl_union_set_free(prog->copy_out);
	isl_union_map_free(prog->read);
	isl_union_map_free(prog->may_write);
	isl_union_map_free(prog->must_write);
	isl_union_map_free(prog->tagged_must_kill);
	isl_union_map_free(prog->array_order);
	isl_union_set_free(prog->may_persist);
	isl_set_free(prog->context);
	free(prog);
	return NULL;
}

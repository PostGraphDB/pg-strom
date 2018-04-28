/*
 * cuda_gpujoin.h
 *
 * GPU accelerated parallel relations join based on hash-join or
 * nested-loop logic.
 * --
 * Copyright 2011-2018 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2018 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef CUDA_GPUJOIN_H
#define CUDA_GPUJOIN_H

/*
 * definition of the inner relations structure. it can load multiple
 * kern_data_store or kern_hash_table.
 */
typedef struct
{
	cl_uint			pg_crc32_table[256];	/* used to hashjoin */
	cl_ulong		kmrels_length;	/* length of kern_multirels */
	cl_ulong		ojmaps_length;	/* length of outer-join map, if any */
	cl_uint			cuda_dindex;	/* device index of PG-Strom */
	cl_uint			nrels;			/* number of inner relations */
	struct
	{
		cl_ulong	chunk_offset;	/* offset to KDS or Hash */
		cl_ulong	ojmap_offset;	/* offset to outer-join map, if any */
		cl_bool		is_nestloop;	/* true, if NestLoop. */
		cl_bool		left_outer;		/* true, if JOIN_LEFT or JOIN_FULL */
		cl_bool		right_outer;	/* true, if JOIN_RIGHT or JOIN_FULL */
		cl_char		__padding__[5];
	} chunks[FLEXIBLE_ARRAY_MEMBER];
} kern_multirels;

#define KERN_MULTIRELS_INNER_KDS(kmrels, depth)	\
	((kern_data_store *)						\
	 ((char *)(kmrels) + (kmrels)->chunks[(depth)-1].chunk_offset))

#define KERN_MULTIRELS_OUTER_JOIN_MAP(kmrels, depth)					\
	((cl_bool *)((kmrels)->chunks[(depth)-1].right_outer				\
				 ? ((char *)(kmrels) +									\
					(size_t)(kmrels)->kmrels_length +					\
					(size_t)(kmrels)->cuda_dindex *						\
					(size_t)(kmrels)->ojmaps_length +					\
					(size_t)(kmrels)->chunks[(depth)-1].ojmap_offset)	\
				 : NULL))

#define KERN_MULTIRELS_LEFT_OUTER_JOIN(kmrels, depth)	\
	__ldg(&((kmrels)->chunks[(depth)-1].left_outer))

#define KERN_MULTIRELS_RIGHT_OUTER_JOIN(kmrels, depth)	\
	__ldg(&((kmrels)->chunks[(depth)-1].right_outer))

/*
 * kern_gpujoin - control object of GpuJoin
 *
 * The control object of GpuJoin has four segments as follows:
 * +------------------+
 * | 1. kern_gpujoin  |
 * +------------------+
 * | 2. kern_parambuf |
 * +------------------+
 * | 3. pseudo stack  |
 * +------------------+
 * | 4. saved context |
 * | for suspend and  |
 * | resume           |
 * +------------------+
 *
 * The first segment is the control object of GpuJoin itself, and the second
 * one is buffer of contrant variables.
 * The third segment is used to save the combination of joined rows as
 * intermediate results, performs like a pseudo-stack area. Individual SMs
 * have exclusive pseudo-stack, thus, can be utilized as a large but slow
 * shared memory. (If depth is low, we may be able to use the actual shared
 * memory instead.)
 * The 4th segment is used to save the execution context when GPU kernel
 * gets suspended. Both of shared memory contents (e.g, read_pos, write_pos)
 * and thread's private variables (e.g, depth, l_state, matched) are saved,
 * then, these state variables shall be restored on restart.
 * GpuJoin kernel will suspend the execution if and when destination buffer
 * gets filled up. Host code is responsible to detach the current buffer
 * and allocates a new one, then resume the GPU kernel.
 * The 4th segment for suspend / resume shall not be in-use unless destination
 * buffer does not run out, thus, it shall not consume devuce physical pages
 * because we allocate the control segment using unified managed memory.
 */
struct kern_gpujoin
{
	kern_errorbuf	kerror;				/* kernel error information */
	cl_uint			kparams_offset;		/* offset to the kparams */
	cl_uint			pstack_offset;		/* offset to the pseudo-stack */
	cl_uint			pstack_nrooms;		/* size of pseudo-stack */
	cl_uint			suspend_offset;		/* offset to the suspend-backup */
	cl_uint			num_rels;			/* number of inner relations */
	cl_bool			resume_context;		/* resume context from suspend */
	cl_uint			src_read_pos;		/* position to read from kds_src */
	/* error status to be backed (OUT) */
	cl_uint			source_nitems;		/* out: # of source rows */
	cl_uint			outer_nitems;		/* out: # of filtered source rows */
	cl_uint			stat_nitems[FLEXIBLE_ARRAY_MEMBER]; /* out: stat nitems */
	/*-- pseudo-stack and suspend/resume context --*/
	/*-- kernel param/const buffer --*/
};
typedef struct kern_gpujoin		kern_gpujoin;

#define KERN_GPUJOIN_PARAMBUF(kgjoin)					\
	((kern_parambuf *)((char *)(kgjoin) + (kgjoin)->kparams_offset))
#define KERN_GPUJOIN_PARAMBUF_LENGTH(kgjoin)			\
	STROMALIGN(KERN_GPUJOIN_PARAMBUF(kgjoin)->length)
#define KERN_GPUJOIN_HEAD_LENGTH(kgjoin)				\
	STROMALIGN((char *)KERN_GPUJOIN_PARAMBUF(kgjoin) +	\
			   KERN_GPUJOIN_PARAMBUF_LENGTH(kgjoin) -	\
			   (char *)(kgjoin))
#define KERN_GPUJOIN_PSEUDO_STACK(kgjoin)					\
	((cl_uint *)((char *)(kgjoin) + (kgjoin)->pstack_offset))
#define KERN_GPUJOIN_SUSPEND_BLOCK(kgjoin)					\
	((struct gpujoin_suspend_block *)						\
	 ((char *)(kgjoin) + (kgjoin)->suspend_offset +			\
	  get_global_index() *									\
	  STROMALIGN(offsetof(gpujoin_suspend_block,			\
						  threads[get_local_size()]))))

#ifdef __CUDACC__

/* utility macros for automatically generated code */
#define GPUJOIN_REF_HTUP(chunk,offset)			\
	((offset) == 0								\
	 ? NULL										\
	 : (HeapTupleHeaderData *)((char *)(chunk) + (size_t)(offset)))
/* utility macros for automatically generated code */
#define GPUJOIN_REF_DATUM(colmeta,htup,colidx)	\
	(!(htup) ? NULL : kern_get_datum_tuple((colmeta),(htup),(colidx)))

/*
 * gpujoin_join_quals
 *
 * Evaluation of join qualifier in the given depth. It shall return true
 * if supplied pair of the rows matches the join condition.
 *
 * NOTE: if x-axil (outer input) or y-axil (inner input) are out of range,
 * we expect outer_index or inner_htup are NULL. Don't skip to call this
 * function, because nested-loop internally uses __syncthread operation
 * to reduce DRAM accesses.
 */
STATIC_FUNCTION(cl_bool)
gpujoin_join_quals(kern_context *kcxt,
				   kern_data_store *kds,
				   kern_multirels *kmrels,
				   int depth,
				   cl_uint *x_buffer,
				   HeapTupleHeaderData *inner_htup,
				   cl_bool *joinquals_matched);

/*
 * gpujoin_hash_value
 *
 * Calculation of hash value if this depth uses hash-join logic.
 */
STATIC_FUNCTION(cl_uint)
gpujoin_hash_value(kern_context *kcxt,
				   cl_uint *pg_crc32_table,
				   kern_data_store *kds,
				   kern_multirels *kmrels,
				   cl_int depth,
				   cl_uint *x_buffer,
				   cl_bool *p_is_null_keys);

/*
 * gpujoin_projection
 *
 * Implementation of device projection. Extract a pair of outer/inner tuples
 * on the tup_values/tup_isnull array.
 */
STATIC_FUNCTION(void)
gpujoin_projection(kern_context *kcxt,
				   kern_data_store *kds_src,
				   kern_multirels *kmrels,
				   cl_uint *r_buffer,
				   kern_data_store *kds_dst,
				   Datum *tup_values,
				   cl_bool *tup_isnull,
				   cl_bool *use_extra_buf,
				   cl_char *extra_buf,
				   cl_uint *extra_len);
/*
 * static shared variables
 */
static __shared__ cl_bool	scan_done;
static __shared__ cl_int	base_depth;
static __shared__ cl_uint	src_read_pos;
static __shared__ cl_uint	dst_base_index;
static __shared__ cl_uint	dst_base_usage;
static __shared__ cl_uint	wip_count[GPUJOIN_MAX_DEPTH+1];
static __shared__ cl_uint	read_pos[GPUJOIN_MAX_DEPTH+1];
static __shared__ cl_uint	write_pos[GPUJOIN_MAX_DEPTH+1];
static __shared__ cl_uint	stat_source_nitems;
static __shared__ cl_uint	stat_nitems[GPUJOIN_MAX_DEPTH+1];
static __shared__ cl_uint	pg_crc32_table[256];

/*
 * per block suspended context
 */
struct gpujoin_suspend_block
{
	cl_int			depth;
	cl_bool			scan_done;
	cl_uint			src_read_pos;
	cl_uint			wip_count[GPUJOIN_MAX_DEPTH+1];
	cl_uint			read_pos[GPUJOIN_MAX_DEPTH+1];
	cl_uint			write_pos[GPUJOIN_MAX_DEPTH+1];
	cl_uint			stat_source_nitems;
	cl_uint			stat_nitems[GPUJOIN_MAX_DEPTH+1];
	struct {
		cl_uint		l_state[GPUJOIN_MAX_DEPTH+1];
		cl_bool		matched[GPUJOIN_MAX_DEPTH+1];
	} threads[FLEXIBLE_ARRAY_MEMBER];
};
typedef struct gpujoin_suspend_block	gpujoin_suspend_block;

/*
 * gpujoin_suspend_context
 */
STATIC_FUNCTION(void)
gpujoin_suspend_context(kern_gpujoin *kgjoin,
						cl_int depth, cl_uint *l_state, cl_bool *matched)
{
	gpujoin_suspend_block *sb = KERN_GPUJOIN_SUSPEND_BLOCK(kgjoin);

	if (get_local_id() == 0)
	{
		sb->depth = depth;
		sb->scan_done = scan_done;
		sb->src_read_pos = src_read_pos;
		memcpy(sb->wip_count, wip_count, sizeof(wip_count));
		memcpy(sb->read_pos, read_pos, sizeof(read_pos));
		memcpy(sb->write_pos, write_pos, sizeof(write_pos));
		sb->stat_source_nitems = stat_source_nitems;
		memcpy(sb->stat_nitems, stat_nitems, sizeof(stat_nitems));
	}
	__syncthreads();
	memcpy(sb->threads[get_local_id()].l_state, l_state,
		   sizeof(cl_uint) * (GPUJOIN_MAX_DEPTH + 1));
	memcpy(sb->threads[get_local_id()].matched, matched,
		   sizeof(cl_bool) * (GPUJOIN_MAX_DEPTH + 1));
}

/*
 * gpujoin_resume_context
 */
STATIC_FUNCTION(cl_int)
gpujoin_resume_context(kern_gpujoin *kgjoin,
					   cl_uint *l_state, cl_bool *matched)
{
	gpujoin_suspend_block *sb = KERN_GPUJOIN_SUSPEND_BLOCK(kgjoin);
	cl_int		depth = sb->depth;

	if (get_local_id() == 0)
	{
		scan_done = sb->scan_done;
		src_read_pos = sb->src_read_pos;
		memcpy(wip_count, sb->wip_count, sizeof(wip_count));
		memcpy(read_pos, sb->read_pos, sizeof(read_pos));
		memcpy(write_pos, sb->write_pos, sizeof(write_pos));
		stat_source_nitems = sb->stat_source_nitems;
		memcpy(stat_nitems, sb->stat_nitems, sizeof(stat_nitems));
	}
	__syncthreads();
	memcpy(l_state, sb->threads[get_local_id()].l_state,
		   sizeof(cl_uint) * (GPUJOIN_MAX_DEPTH + 1));
	memcpy(matched, sb->threads[get_local_id()].matched,
		   sizeof(cl_bool) * (GPUJOIN_MAX_DEPTH + 1));
	return depth;
}

/*
 * gpujoin_rewind_stack
 */
STATIC_INLINE(cl_int)
gpujoin_rewind_stack(cl_int depth, cl_uint *l_state, cl_bool *matched)
{
	static __shared__ cl_int	__depth;

	assert(depth >= base_depth && depth <= GPUJOIN_MAX_DEPTH);
	__syncthreads();
	if (get_local_id() == 0)
	{
		__depth = depth;
		for (;;)
		{
			/*
			 * At the time of rewind, all the upper tuples (outer combinations
			 * from the standpoint of deeper depth) are already processed.
			 * So, we can safely rewind the read/write index of this depth.
			 */
			read_pos[__depth] = 0;
			write_pos[__depth] = 0;

			/*
			 * If any of outer combinations are in progress to find out
			 * matching inner tuple, we have to resume the task, prior
			 * to the increment of read pointer.
			 */
			if (wip_count[__depth] > 0)
				break;
			if (__depth == base_depth ||
				read_pos[__depth-1] < write_pos[__depth-1])
				break;
			__depth--;
		}
	}
	__syncthreads();
	depth = __depth;
	if (depth < GPUJOIN_MAX_DEPTH)
	{
		memset(l_state + depth + 1, 0,
			   sizeof(cl_uint) * (GPUJOIN_MAX_DEPTH - depth));
		memset(matched + depth + 1, 0,
			   sizeof(cl_bool) * (GPUJOIN_MAX_DEPTH - depth));
	}
	if (scan_done && depth == base_depth)
		return -1;
	return depth;
}

/*
 * gpujoin_load_source
 */
STATIC_FUNCTION(cl_int)
gpujoin_load_source(kern_context *kcxt,
					kern_gpujoin *kgjoin,
					kern_data_store *kds_src,
					cl_uint *wr_stack,
					cl_uint *l_state)
{
	cl_uint		t_offset = UINT_MAX;
	cl_bool		visible = false;
	cl_uint		count;
	cl_uint		wr_index;

	/* extract a HeapTupleHeader */
	if (__ldg(&kds_src->format) == KDS_FORMAT_ROW)
	{
		kern_tupitem   *tupitem;
		cl_uint			row_index;

		/* fetch next window */
		if (get_local_id() == 0)
			src_read_pos = atomicAdd(&kgjoin->src_read_pos,
									 get_local_size());
		__syncthreads();
		row_index = src_read_pos + get_local_id();

		if (row_index < __ldg(&kds_src->nitems))
		{
			tupitem = KERN_DATA_STORE_TUPITEM(kds_src, row_index);
			t_offset = (cl_uint)((char *)&tupitem->htup - (char *)kds_src);

			visible = gpuscan_quals_eval(kcxt,
										 kds_src,
										 &tupitem->t_self,
										 &tupitem->htup);
		}
		assert(wip_count[0] == 0);
	}
	else if (__ldg(&kds_src->format) == KDS_FORMAT_BLOCK)
	{
		cl_uint		part_sz = KERN_DATA_STORE_PARTSZ(kds_src);
		cl_uint		n_parts = get_local_size() / part_sz;
		cl_uint		part_id;
		cl_uint		line_no;
		cl_uint		n_lines;
		cl_uint		loops = l_state[0]++;

		/* fetch next window, if needed */
		if (loops == 0 && get_local_id() == 0)
			src_read_pos = atomicAdd(&kgjoin->src_read_pos, n_parts);
		__syncthreads();
		part_id = src_read_pos + get_local_id() / part_sz;
		line_no = get_local_id() % part_sz + loops * part_sz + 1;

		if (part_id < __ldg(&kds_src->nitems) &&
			get_local_id() < part_sz * n_parts)
		{
			PageHeaderData *pg_page;
			BlockNumber		block_nr;
			ItemPointerData	t_self;
			HeapTupleHeaderData *htup;

			pg_page = KERN_DATA_STORE_BLOCK_PGPAGE(kds_src, part_id);
			n_lines = PageGetMaxOffsetNumber(pg_page);
			block_nr = KERN_DATA_STORE_BLOCK_BLCKNR(kds_src, part_id);

			if (line_no <= n_lines)
			{
				ItemIdData *lpp = PageGetItemId(pg_page, line_no);
				if (ItemIdIsNormal(lpp))
				{
					t_offset = (cl_uint)((char *)lpp - (char *)kds_src);
					t_self.ip_blkid.bi_hi = block_nr >> 16;
					t_self.ip_blkid.bi_lo = block_nr & 0xffff;
					t_self.ip_posid = line_no;

					htup = PageGetItem(pg_page, lpp);

					visible = gpuscan_quals_eval(kcxt,
												 kds_src,
												 &t_self,
												 htup);
				}
			}
		}
	}
	else
	{
		assert(__ldg(&kds_src->format) == KDS_FORMAT_COLUMN);
		cl_uint			row_index;

		/* fetch next window */
		if (get_local_id() == 0)
			src_read_pos = atomicAdd(&kgjoin->src_read_pos,
									 get_local_size());
		__syncthreads();
		row_index = src_read_pos + get_local_id();

		if (row_index < __ldg(&kds_src->nitems))
		{
			t_offset = row_index + 1;
			visible = gpuscan_quals_eval_column(kcxt,
												kds_src,
												row_index);
		}
		assert(wip_count[0] == 0);
	}
	/* error checks */
	if (__syncthreads_count(kcxt->e.errcode) > 0)
		return -1;
	/* statistics */
	count = __syncthreads_count(t_offset != UINT_MAX);
	if (get_local_id() == 0)
	{
		if (__ldg(&kds_src->format) == KDS_FORMAT_BLOCK)
			wip_count[0] = count;
		stat_source_nitems += count;
	}

	/* store the source tuple if visible */
	wr_index = pgstromStairlikeBinaryCount(visible, &count);
	if (count > 0)
	{
		wr_index += write_pos[0];
		__syncthreads();
		if (get_local_id() == 0)
		{
			write_pos[0] += count;
			stat_nitems[0] += count;
		}
		if (visible)
			wr_stack[wr_index] = t_offset;
		__syncthreads();

		/*
		 * An iteration can fetch up to get_local_size() tuples
		 * at once, thus, we try to dive into deeper depth prior
		 * to the next outer tuples.
		 */
		if (write_pos[0] + get_local_size() > kgjoin->pstack_nrooms)
			return 1;
		__syncthreads();
	}
	else
	{
		/* no tuples we could fetch */
		assert(write_pos[0] + get_local_size() <= kgjoin->pstack_nrooms);
		l_state[0] = 0;
		__syncthreads();
	}

	/* End of the outer relation? */
	if (src_read_pos >= kds_src->nitems)
	{
		/* don't rewind the stack any more */
		if (get_local_id() == 0)
			scan_done = true;
		__syncthreads();

		/*
		 * We may have to dive into the deeper depth if we still have
		 * pending join combinations.
		 */
		if (write_pos[0] == 0)
		{
			for (cl_int depth=1; depth <= GPUJOIN_MAX_DEPTH; depth++)
			{
				if (read_pos[depth] < write_pos[depth])
					return depth+1;
			}
			return -1;
		}
		return 1;
	}
	return 0;
}

/*
 * gpujoin_load_outer
 */
STATIC_FUNCTION(cl_int)
gpujoin_load_outer(kern_context *kcxt,
				   kern_gpujoin *kgjoin,
				   kern_multirels *kmrels,
				   cl_int outer_depth,
				   cl_uint *wr_stack,
				   cl_uint *l_state)
{
	kern_data_store *kds_in = KERN_MULTIRELS_INNER_KDS(kmrels, outer_depth);
	cl_bool		   *ojmap = KERN_MULTIRELS_OUTER_JOIN_MAP(kmrels, outer_depth);
	HeapTupleHeaderData *htup = NULL;
	kern_tupitem   *tupitem;
	cl_uint			t_offset;
	cl_uint			row_index;
	cl_uint			wr_index;
	cl_uint			count;

	assert(ojmap != NULL);

	if (get_local_id() == 0)
		src_read_pos = atomicAdd(&kgjoin->src_read_pos,
								 get_local_size());
	__syncthreads();
	row_index = src_read_pos + get_local_id();

	/* pickup inner rows, if unreferenced */
	if (row_index < kds_in->nitems && !ojmap[row_index])
	{
		tupitem = KERN_DATA_STORE_TUPITEM(kds_in, row_index);
		t_offset = (cl_uint)((char *)&tupitem->htup - (char *)kds_in);
		htup = &tupitem->htup;
	}
	wr_index = write_pos[outer_depth];
	wr_index += pgstromStairlikeBinaryCount(htup != NULL, &count);
	__syncthreads();
	if (count > 0)
	{
		if (get_local_id() == 0)
		{
			write_pos[outer_depth] += count;
			stat_nitems[outer_depth] += count;
        }
		if (htup)
		{
			wr_stack += wr_index * (outer_depth + 1);
			memset(wr_stack, 0, sizeof(cl_uint) * outer_depth);
			wr_stack[outer_depth] = t_offset;
		}
		__syncthreads();
	}

	/* end of the inner relation? */
	if (src_read_pos >= kds_in->nitems)
	{
		/* don't rewind the stack any more */
		if (get_local_id() == 0)
			scan_done = true;
		__syncthreads();

		/*
		 * We may have to dive into the deeper depth if we still have
		 * pending join combinations.
		 */
		if (write_pos[outer_depth] == 0)
		{
			for (cl_int dep=outer_depth + 1; dep <= GPUJOIN_MAX_DEPTH; dep++)
			{
				if (read_pos[dep] < write_pos[dep])
					return dep+1;
			}
			return -1;
		}
		return outer_depth+1;
	}
	return outer_depth;
}

/*
 * gpujoin_projection_row
 */
STATIC_FUNCTION(cl_int)
gpujoin_projection_row(kern_context *kcxt,
					   kern_gpujoin *kgjoin,
					   kern_multirels *kmrels,
					   kern_data_store *kds_src,
					   kern_data_store *kds_dst,
					   cl_uint *rd_stack,
					   cl_uint *l_state,
					   cl_bool *matched)
{
	cl_uint		nrels = kgjoin->num_rels;
	cl_uint		read_index;
	cl_uint		dest_index;
	cl_uint		dest_offset;
	cl_uint		count;
	cl_uint		nvalids;
	cl_uint		required;
#if GPUJOIN_DEVICE_PROJECTION_NFIELDS > 0
	Datum		tup_values[GPUJOIN_DEVICE_PROJECTION_NFIELDS];
	cl_bool		tup_isnull[GPUJOIN_DEVICE_PROJECTION_NFIELDS];
	cl_bool		use_extra_buf[GPUJOIN_DEVICE_PROJECTION_NFIELDS];
#else
	Datum	   *tup_values = NULL;
	cl_bool	   *tup_isnull = NULL;
	cl_bool	   *use_extra_buf = NULL;
#endif
#if GPUJOIN_DEVICE_PROJECTION_EXTRA_SIZE > 0
	cl_char		extra_buf[GPUJOIN_DEVICE_PROJECTION_EXTRA_SIZE]
				__attribute__ ((aligned(MAXIMUM_ALIGNOF)));
#else
	cl_char	   *extra_buf = NULL;
#endif
	cl_uint		extra_len = 0;

	/* sanity checks */
	assert(rd_stack != NULL);

	/* Any more result rows to be written? */
	if (read_pos[nrels] >= write_pos[nrels])
		return gpujoin_rewind_stack(nrels, l_state, matched);

	/* pick up combinations from the pseudo-stack */
	nvalids = Min(write_pos[nrels] - read_pos[nrels],
				  get_local_size());
	read_index = read_pos[nrels] + get_local_id();
	__syncthreads();

	/* step.1 - compute length of the result tuple to be written */
	if (read_index < write_pos[nrels])
	{
		rd_stack += read_index * (nrels + 1);

		gpujoin_projection(kcxt,
						   kds_src,
						   kmrels,
						   rd_stack,
						   kds_dst,
						   tup_values,
						   tup_isnull,
						   use_extra_buf,
						   extra_buf,
						   &extra_len);
		assert(extra_len <= GPUJOIN_DEVICE_PROJECTION_EXTRA_SIZE);
		required = MAXALIGN(offsetof(kern_tupitem, htup) +
							compute_heaptuple_size(kcxt,
												   kds_dst,
												   tup_values,
												   tup_isnull));
	}
	else
		required = 0;

	if (__syncthreads_count(kcxt->e.errcode) > 0)
		return -1;		/* bailout */

	/* step.2 - increments nitems/usage of the kds_dst */
	dest_offset = pgstromStairlikeSum(required, &count);
	assert(count > 0);
	if (get_local_id() == 0)
	{
		union {
			struct {
				cl_uint	nitems;
				cl_uint	usage;
			} i;
			cl_ulong	v64;
		} oldval, curval, newval;

		curval.i.nitems	= kds_dst->nitems;
		curval.i.usage	= kds_dst->usage;
		do {
			newval = oldval = curval;
			newval.i.nitems	+= nvalids;
			newval.i.usage	+= count;

			if (KERN_DATA_STORE_HEAD_LENGTH(kds_dst) +
				STROMALIGN(sizeof(cl_uint) * newval.i.nitems) +
				newval.i.usage > kds_dst->length)
			{
				STROM_SET_ERROR(&kcxt->e, StromError_Suspend);
				break;
			}
		} while ((curval.v64 = atomicCAS((cl_ulong *)&kds_dst->nitems,
										 oldval.v64,
										 newval.v64)) != oldval.v64);
		dst_base_index = oldval.i.nitems;
		dst_base_usage = oldval.i.usage;
	}
	if (__syncthreads_count(kcxt->e.errcode) > 0)
	{
		/* No space left on the kds_dst, suspend the GPU kernel and bailout */
		gpujoin_suspend_context(kgjoin, nrels+1, l_state, matched);
		return -2;	/* <-- not to update statistics */
	}
	dest_index = dst_base_index + get_local_id();
	dest_offset += dst_base_usage + required;

	/* step.3 - write out HeapTuple on the destination buffer */
	if (required > 0)
	{
		cl_uint	   *row_index = KERN_DATA_STORE_ROWINDEX(kds_dst);
		kern_tupitem *tupitem = (kern_tupitem *)
			((char *)kds_dst + kds_dst->length - dest_offset);

		row_index[dest_index] = kds_dst->length - dest_offset;
		form_kern_heaptuple(tupitem,
							kds_dst->ncols,
							kds_dst->colmeta,
							NULL,		/* ItemPointerData */
							NULL,		/* HeapTupleFields */
							kds_dst->tdhasoid ? 0xffffffff : 0,
							tup_values,
							tup_isnull);
	}
	if (__syncthreads_count(kcxt->e.errcode) > 0)
		return -1;	/* bailout */

	/* step.4 - make advance the read position */
	if (get_local_id() == 0)
		read_pos[nrels] += nvalids;
	return nrels + 1;
}

#ifdef GPUPREAGG_COMBINED_JOIN
/* to be defined by gpupreagg.c */
STATIC_FUNCTION(void)
gpupreagg_projection_slot(kern_context *kcxt_gpreagg,
						  Datum *src_values,
						  cl_char *src_isnull,
						  Datum *dst_values,
						  cl_char *dst_isnull);

/*
 * gpujoin_projection_slot
 */
STATIC_FUNCTION(cl_int)
gpujoin_projection_slot(kern_context *kcxt,
						kern_context *kcxt_gpreagg,
						kern_gpujoin *kgjoin,
						kern_multirels *kmrels,
						kern_data_store *kds_src,
						kern_data_store *kds_dst,
						cl_uint *rd_stack,
						cl_uint *l_state,
						cl_bool *matched)
{
	cl_uint		nrels = kgjoin->num_rels;
	cl_uint		read_index;
	cl_uint		dest_index;
	cl_uint		dest_offset;
	cl_uint		count;
	cl_uint		nvalids;
	cl_bool		tup_is_valid = false;
#if GPUJOIN_DEVICE_PROJECTION_NFIELDS > 0
	Datum		tup_values[GPUJOIN_DEVICE_PROJECTION_NFIELDS];
	cl_bool		tup_isnull[GPUJOIN_DEVICE_PROJECTION_NFIELDS];
	cl_bool		use_extra_buf[GPUJOIN_DEVICE_PROJECTION_NFIELDS];
#else
	Datum	   *tup_values = NULL;
	cl_bool	   *tup_isnull = NULL;
	cl_bool	   *use_extra_buf = NULL;
#endif
#if GPUJOIN_DEVICE_PROJECTION_EXTRA_SIZE > 0
	cl_char		extra_buf[GPUJOIN_DEVICE_PROJECTION_EXTRA_SIZE]
				__attribute__ ((aligned(MAXIMUM_ALIGNOF)));
#else
	cl_char	   *extra_buf = NULL;
#endif
	cl_uint		extra_len = 0;

	/* sanity checks */
	assert(rd_stack != NULL);

	/* Any more result rows to be written? */
	if (read_pos[nrels] >= write_pos[nrels])
		return gpujoin_rewind_stack(nrels, l_state, matched);

	/* pick up combinations from the pseudo-stack */
	nvalids = Min(write_pos[nrels] - read_pos[nrels],
				  get_local_size());
	read_index = read_pos[nrels] + get_local_id();
	__syncthreads();

	/* step.1 - projection by GpuJoin */
	if (read_index < write_pos[nrels])
	{
		/*
		 * NOTE: We don't need to copy varlena datum, but pointer reference
		 * only, because pds_src / kmrels are still valid during GpuPreAgg.
		 */
		rd_stack += read_index * (nrels + 1);

		gpujoin_projection(kcxt,
						   kds_src,
						   kmrels,
						   rd_stack,
						   kds_dst,
						   tup_values,
						   tup_isnull,
						   use_extra_buf,
						   extra_buf,
						   &extra_len);
		assert(extra_len <= GPUJOIN_DEVICE_PROJECTION_EXTRA_SIZE);

		tup_is_valid = true;
	}

	/* step.2 - increments nitems/usage of the kds_dst */
	dest_offset = pgstromStairlikeSum(extra_len, &count);
	if (get_local_id() == 0)
	{
		union {
			struct {
				cl_uint nitems;
				cl_uint usage;
			} i;
			cl_ulong	v64;
		} oldval, curval, newval;

		curval.i.nitems = kds_dst->nitems;
        curval.i.usage  = kds_dst->usage;
		do {
			newval = oldval = curval;
			newval.i.nitems += nvalids;
			newval.i.usage  += count;

			if (KERN_DATA_STORE_SLOT_LENGTH(kds_dst, newval.i.nitems) +
				newval.i.usage > kds_dst->length)
			{
				STROM_SET_ERROR(&kcxt->e, StromError_Suspend);
				break;
			}
		} while ((curval.v64 = atomicCAS((cl_ulong *)&kds_dst->nitems,
										 oldval.v64,
										 newval.v64)) != oldval.v64);
		dst_base_index = oldval.i.nitems;
		dst_base_usage = oldval.i.usage;
	}
	if (__syncthreads_count(kcxt->e.errcode) > 0)
	{
		/* No space left on the kds_dst, suspend the GPU kernel and bailout */
		gpujoin_suspend_context(kgjoin, nrels+1, l_state, matched);
		return -2;	/* <-- not to update statistics */
	}
	dest_index = dst_base_index + get_local_id();
	dest_offset += dst_base_usage + extra_len;

	/* step.3 - projection by GpuPreAgg on the destination buffer */
	if (tup_is_valid)
	{
		Datum	   *dst_values = KERN_DATA_STORE_VALUES(kds_dst, dest_index);
		cl_bool	   *dst_isnull = KERN_DATA_STORE_ISNULL(kds_dst, dest_index);
		cl_char    *dst_extra;
		cl_int		i, offset;

		/*
		 * If varlena or indirect variables are stored in the extra buf,
		 * we have to move the body of variables to kds_dst, and update
		 * the pointers.
		 */
		if (extra_len > 0)
		{
			dst_extra = (char *)kds_dst + kds_dst->length - dest_offset;
			memcpy(dst_extra, extra_buf, extra_len);

			for (i=0; i < GPUJOIN_DEVICE_PROJECTION_NFIELDS; i++)
			{
				if (tup_isnull[i] || !use_extra_buf[i])
					continue;
				assert(DatumGetPointer(tup_values[i]) >= extra_buf &&
					   DatumGetPointer(tup_values[i]) < (extra_buf +
														 extra_len));
				offset = (cl_int)(DatumGetPointer(tup_values[i]) - extra_buf);
				tup_values[i] = PointerGetDatum(dst_extra) + offset;
			}
		}

		/*
		 * initial projection by GpuPreAgg
		 */
		gpupreagg_projection_slot(kcxt_gpreagg,
								  tup_values,
								  tup_isnull,
								  dst_values,
								  dst_isnull);
	}
	if (__syncthreads_count(kcxt->e.errcode) > 0)
		return -1;	/* bailout */

	/* step.4 - make advance the read position */
	if (get_local_id() == 0)
		read_pos[nrels] += nvalids; //get_local_size();
	return nrels + 1;
}
#endif /* GPUPREAGG_COMBINED_JOIN */

/*
 * gpujoin_exec_nestloop
 */
STATIC_FUNCTION(cl_int)
gpujoin_exec_nestloop(kern_context *kcxt,
					  kern_gpujoin *kgjoin,
					  kern_multirels *kmrels,
					  kern_data_store *kds_src,
					  cl_int depth,
					  cl_uint *rd_stack,
					  cl_uint *wr_stack,
					  cl_uint *l_state,
					  cl_bool *matched)
{
	kern_data_store *kds_in = KERN_MULTIRELS_INNER_KDS(kmrels, depth);
	cl_bool		   *oj_map = KERN_MULTIRELS_OUTER_JOIN_MAP(kmrels, depth);
	kern_tupitem   *tupitem = NULL;
	cl_uint			x_unitsz;
	cl_uint			y_unitsz;
	cl_uint			x_index;	/* outer index */
	cl_uint			y_index;	/* inner index */
	cl_uint			wr_index;
	cl_uint			count;
	cl_bool			result = false;
	__shared__ cl_bool matched_sync[MAXTHREADS_PER_BLOCK];

	assert(kds_in->format == KDS_FORMAT_ROW);
	assert(depth >= 1 && depth <= GPUJOIN_MAX_DEPTH);
	if (read_pos[depth-1] >= write_pos[depth-1])
	{
		/*
		 * When this depth has enough room (even if all the threads generate
		 * join combinations on the next try), upper depth may be able to
		 * generate more outer tuples; which shall be used to input for the
		 * next depth.
		 * It is mostly valuable to run many combinations on the next depth.
		 */
		assert(wip_count[depth] == 0);
		if (write_pos[depth] + get_local_size() <= kgjoin->pstack_nrooms)
		{
			cl_int	__depth = gpujoin_rewind_stack(depth-1, l_state, matched);

			if (__depth >= base_depth)
				return __depth;
		}
		/* elsewhere, dive into the deeper depth or projection */
		return depth + 1;
	}
	x_unitsz = Min(write_pos[depth-1] - read_pos[depth-1],
				   get_local_size());
	y_unitsz = get_local_size() / x_unitsz;

	x_index = get_local_id() % x_unitsz;
	y_index = get_local_id() / x_unitsz;

	if (y_unitsz * l_state[depth] >= kds_in->nitems)
	{
		/*
		 * In case of LEFT OUTER JOIN, we need to check whether the outer
		 * combination had any matched inner tuples, or not.
		 */
		if (KERN_MULTIRELS_LEFT_OUTER_JOIN(kmrels, depth))
		{
			if (get_local_id() < x_unitsz)
				matched_sync[get_local_id()] = false;
			__syncthreads();
			if (matched[depth])
				matched_sync[x_index] = true;
			if (__syncthreads_count(!matched_sync[x_index]) > 0)
			{
				if (y_index == 0 && y_index < y_unitsz)
					result = !matched_sync[x_index];
				else
					result = false;
				/* adjust x_index and rd_stack as usual */
				x_index += read_pos[depth-1];
				assert(x_index < write_pos[depth-1]);
				rd_stack += (x_index * depth);
				/* don't generate LEFT OUTER tuple any more */
				matched[depth] = true;
				goto left_outer;
			}
		}
		l_state[depth] = 0;
		matched[depth] = false;
		if (get_local_id() == 0)
		{
			wip_count[depth] = 0;
			read_pos[depth-1] += x_unitsz;
		}
		return depth;
	}

	x_index += read_pos[depth-1];
	assert(x_index < write_pos[depth-1]);
	rd_stack += (x_index * depth);
	if (y_index < y_unitsz)
	{
		y_index += y_unitsz * l_state[depth];
		if (y_index < kds_in->nitems)
		{
			tupitem = KERN_DATA_STORE_TUPITEM(kds_in, y_index);

			result = gpujoin_join_quals(kcxt,
										kds_src,
										kmrels,
										depth,
										rd_stack,
										&tupitem->htup,
										NULL);
			if (result)
			{
				matched[depth] = true;
				if (oj_map && !oj_map[y_index])
					oj_map[y_index] = true;
			}
		}
	}
	l_state[depth]++;

left_outer:
	wr_index = write_pos[depth];
	wr_index += pgstromStairlikeBinaryCount(result, &count);
	if (get_local_id() == 0)
	{
		wip_count[depth] = get_local_size();
		write_pos[depth] += count;
		stat_nitems[depth] += count;
	}
	wr_stack += wr_index * (depth + 1);
	if (result)
	{
		memcpy(wr_stack, rd_stack, sizeof(cl_uint) * depth);
		wr_stack[depth] = (!tupitem ? 0 : (cl_uint)((char *)&tupitem->htup -
													(char *)kds_in));
	}
	__syncthreads();
	/*
	 * If we have enough room to store the combinations more, execute this
	 * depth one more. Elsewhere, dive into a deeper level to flush results.
	 */
	if (write_pos[depth] + get_local_size() <= kgjoin->pstack_nrooms)
		return depth;
	return depth + 1;
}

/*
 * gpujoin_exec_hashjoin
 */
STATIC_FUNCTION(cl_int)
gpujoin_exec_hashjoin(kern_context *kcxt,
					  kern_gpujoin *kgjoin,
					  kern_multirels *kmrels,
					  kern_data_store *kds_src,
					  cl_int depth,
					  cl_uint *rd_stack,
					  cl_uint *wr_stack,
					  cl_uint *l_state,
					  cl_bool *matched)
{
	kern_data_store	   *kds_hash = KERN_MULTIRELS_INNER_KDS(kmrels, depth);
	cl_bool			   *oj_map = KERN_MULTIRELS_OUTER_JOIN_MAP(kmrels, depth);
	kern_hashitem	   *khitem = NULL;
	cl_uint				hash_value;
	cl_uint				rd_index;
	cl_uint				wr_index;
	cl_uint				count;
	cl_bool				result;

	assert(kds_hash->format == KDS_FORMAT_HASH);
	assert(depth >= 1 && depth <= GPUJOIN_MAX_DEPTH);

	if (__syncthreads_or(l_state[depth] != UINT_MAX) == 0)
	{
		/*
		 * OK, all the threads reached to the end of hash-slot chain
		 * Move to the next outer window.
		 */
		if (get_local_id() == 0)
			read_pos[depth-1] += get_local_size();
		l_state[depth] = 0;
		matched[depth] = false;
		return depth;
	}
	else if (read_pos[depth-1] >= write_pos[depth-1])
	{
		/*
		 * When this depth has enough room (even if all the threads generate
		 * join combinations on the next try), upper depth may be able to
		 * generate more outer tuples; which shall be used to input for the
		 * next depth.
		 * It is mostly valuable to run many combinations on the next depth.
		 */
		assert(wip_count[depth] == 0);
		if (write_pos[depth] + get_local_size() <= kgjoin->pstack_nrooms)
		{
			cl_int	__depth = gpujoin_rewind_stack(depth-1, l_state, matched);

			if (__depth >= base_depth)
				return __depth;
		}
		/* elsewhere, dive into the deeper depth or projection */
		return depth + 1;
	}
	rd_index = read_pos[depth-1] + get_local_id();
	rd_stack += (rd_index * depth);

	if (l_state[depth] == 0)
	{
		/* first touch to the hash-slot */
		if (rd_index < write_pos[depth-1])
		{
			cl_bool		is_null_keys;

			hash_value = gpujoin_hash_value(kcxt,
											pg_crc32_table,
											kds_src,
											kmrels,
											depth,
											rd_stack,
											&is_null_keys);
			if (hash_value >= kds_hash->hash_min &&
				hash_value <= kds_hash->hash_max)
			{
				/* MEMO: NULL-keys will never match to inner-join */
				if (!is_null_keys)
					khitem = KERN_HASH_FIRST_ITEM(kds_hash, hash_value);
			}
		}
		else
		{
			/*
			 * MEMO: We must ensure the threads without outer tuple don't
			 * generate any LEFT OUTER results.
			 */
			l_state[depth] = UINT_MAX;
		}
	}
	else if (l_state[depth] != UINT_MAX)
	{
		/* walks on the hash-slot chain */
		khitem = (kern_hashitem *)((char *)kds_hash
								   + l_state[depth]
								   - offsetof(kern_hashitem, t.htup));
		hash_value = khitem->hash;

		/* pick up next one if any */
		khitem = KERN_HASH_NEXT_ITEM(kds_hash, khitem);
	}

	while (khitem && khitem->hash != hash_value)
		khitem = KERN_HASH_NEXT_ITEM(kds_hash, khitem);

	if (khitem)
	{
		cl_bool		joinquals_matched;

		assert(khitem->hash == hash_value);

		result = gpujoin_join_quals(kcxt,
									kds_src,
									kmrels,
									depth,
									rd_stack,
									&khitem->t.htup,
									&joinquals_matched);
		assert(result == joinquals_matched);
		if (joinquals_matched)
		{
			/* No LEFT/FULL JOIN are needed */
			matched[depth] = true;
			/* No RIGHT/FULL JOIN are needed */
			assert(khitem->rowid < kds_hash->nitems);
			if (oj_map && !oj_map[khitem->rowid])
				oj_map[khitem->rowid] = true;
		}
	}
	else if (KERN_MULTIRELS_LEFT_OUTER_JOIN(kmrels, depth) &&
			 l_state[depth] != UINT_MAX &&
			 !matched[depth])
	{
		/* No matched outer rows, but LEFT/FULL OUTER */
		result = true;
	}
	else
		result = false;

	/* save the current hash item */
	l_state[depth] = (!khitem ? UINT_MAX : (cl_uint)((char *)&khitem->t.htup -
													 (char *)kds_hash));
	wr_index = write_pos[depth];
	wr_index += pgstromStairlikeBinaryCount(result, &count);
	if (get_local_id() == 0)
	{
		write_pos[depth] += count;
		stat_nitems[depth] += count;
	}
	wr_stack += wr_index * (depth + 1);
	if (result)
	{
		memcpy(wr_stack, rd_stack, sizeof(cl_uint) * depth);
		wr_stack[depth] = (!khitem ? 0U : (cl_uint)((char *)&khitem->t.htup -
													(char *)kds_hash));
	}
	/* count number of threads still in-progress */
	count = __syncthreads_count(khitem != NULL);
	if (get_local_id() == 0)
		wip_count[depth] = count;
	/* enough room exists on this depth? */
	if (write_pos[depth] + get_local_size() <= kgjoin->pstack_nrooms)
		return depth;
	return depth+1;
}

#define PSTACK_DEPTH(d)							\
	((d) >= 0 && (d) <= kgjoin->num_rels		\
	 ? (pstack_base + pstack_nrooms * ((d) * ((d) + 1)) / 2) : NULL)

/*
 * gpujoin_main
 */
KERNEL_FUNCTION(void)
gpujoin_main(kern_gpujoin *kgjoin,
			 kern_multirels *kmrels,
			 kern_data_store *kds_src,
			 kern_data_store *kds_dst,
			 kern_parambuf *kparams_gpreagg) /* only if combined GpuJoin */
{
	kern_parambuf  *kparams = KERN_GPUJOIN_PARAMBUF(kgjoin);
	kern_context	kcxt;
	kern_context	kcxt_gpreagg __attribute__((unused));
	cl_int			depth;
	cl_int			index;
	cl_uint			pstack_nrooms;
	cl_uint		   *pstack_base;
	cl_uint			l_state[GPUJOIN_MAX_DEPTH+1];
	cl_bool			matched[GPUJOIN_MAX_DEPTH+1];
	__shared__ cl_int depth_thread0 __attribute__((unused));

	INIT_KERNEL_CONTEXT(&kcxt, gpujoin_main, kparams);
	assert(__ldg(&kds_src->format) == KDS_FORMAT_ROW ||
		   __ldg(&kds_src->format) == KDS_FORMAT_BLOCK ||
		   __ldg(&kds_src->format) == KDS_FORMAT_COLUMN);
#ifndef GPUPREAGG_COMBINED_JOIN
	assert(__ldg(&kds_dst->format) == KDS_FORMAT_ROW);
	assert(kparams_gpreagg == NULL);
#else
	assert(__ldg(&kds_dst->format) == KDS_FORMAT_SLOT);
	assert(kparams_gpreagg != NULL);
	INIT_KERNEL_CONTEXT(&kcxt_gpreagg, gpujoin_main, kparams_gpreagg);
#endif

	/* setup private variables */
	pstack_nrooms = kgjoin->pstack_nrooms;
	pstack_base = (cl_uint *)((char *)kgjoin + kgjoin->pstack_offset)
		+ get_global_index() * pstack_nrooms * ((GPUJOIN_MAX_DEPTH+1) *
												(GPUJOIN_MAX_DEPTH+2)) / 2;
	/* setup crc32 table */
	for (index = get_local_id();
		 index < lengthof(pg_crc32_table);
		 index += get_local_size())
		pg_crc32_table[index] = kmrels->pg_crc32_table[index];
	__syncthreads();

	/* setup per-depth context */
	memset(l_state, 0, sizeof(l_state));
	memset(matched, 0, sizeof(matched));
	if (get_local_id() == 0)
	{
		src_read_pos = UINT_MAX;
		stat_source_nitems = 0;
		memset(stat_nitems, 0, sizeof(stat_nitems));
		memset(wip_count, 0, sizeof(wip_count));
		memset(read_pos, 0, sizeof(read_pos));
		memset(write_pos, 0, sizeof(write_pos));
		scan_done = false;
		base_depth = 0;
	}
	__syncthreads();
	if (kgjoin->resume_context)
		depth = gpujoin_resume_context(kgjoin, l_state, matched);
	else
		depth = 0;

	/* main logic of GpuJoin */
	while (depth >= 0)
	{
		if (depth == 0)
		{
			/* LOAD FROM KDS_SRC (ROW/BLOCK/COLUMN) */
			depth = gpujoin_load_source(&kcxt,
										kgjoin,
										kds_src,
										PSTACK_DEPTH(depth),
										l_state);
		}
		else if (depth > kgjoin->num_rels)
		{
			assert(depth == kmrels->nrels + 1);
#ifndef GPUPREAGG_COMBINED_JOIN
			/* PROJECTION (ROW) */
			depth = gpujoin_projection_row(&kcxt,
										   kgjoin,
										   kmrels,
										   kds_src,
										   kds_dst,
										   PSTACK_DEPTH(kgjoin->num_rels),
										   l_state,
										   matched);
#else
			/* PROJECTION (SLOT) */
			depth = gpujoin_projection_slot(&kcxt,
											&kcxt_gpreagg,
											kgjoin,
											kmrels,
											kds_src,
											kds_dst,
											PSTACK_DEPTH(kgjoin->num_rels),
											l_state,
											matched);
#endif
		}
		else if (kmrels->chunks[depth-1].is_nestloop)
		{
			/* NEST-LOOP */
			depth = gpujoin_exec_nestloop(&kcxt,
										  kgjoin,
										  kmrels,
										  kds_src,
										  depth,
										  PSTACK_DEPTH(depth-1),
										  PSTACK_DEPTH(depth),
										  l_state,
										  matched);
		}
		else
		{
			/* HASH-JOIN */
			depth = gpujoin_exec_hashjoin(&kcxt,
										  kgjoin,
										  kmrels,
										  kds_src,
										  depth,
										  PSTACK_DEPTH(depth-1),
										  PSTACK_DEPTH(depth),
										  l_state,
										  matched);
		}
		if (get_local_id() == 0)
			depth_thread0 = depth;
		__syncthreads();
		assert(depth_thread0 == depth);
	}

	/* update statistics only if normal exit */
	if (depth == -1 && get_local_id() == 0)
	{
		gpujoin_suspend_block *sb = KERN_GPUJOIN_SUSPEND_BLOCK(kgjoin);
		sb->depth = -1;		/* no more suspend/resume! */

		atomicAdd(&kgjoin->source_nitems, stat_source_nitems);
		atomicAdd(&kgjoin->outer_nitems, stat_nitems[0]);
		for (index=0; index < GPUJOIN_MAX_DEPTH; index++)
			atomicAdd(&kgjoin->stat_nitems[index],
					  stat_nitems[index+1]);
	}
	__syncthreads();
	kern_writeback_error_status(&kgjoin->kerror, &kcxt.e);
}

/*
 * gpujoin_collocate_outer_join_map
 *
 * it merges the result of other GPU devices and CPU fallback
 */
KERNEL_FUNCTION(void)
gpujoin_colocate_outer_join_map(kern_multirels *kmrels,
								cl_uint num_devices)
{
	size_t		nrooms = kmrels->ojmaps_length / sizeof(cl_uint);
	cl_uint	   *ojmaps = (cl_uint *)((char *)kmrels + kmrels->kmrels_length);
	cl_uint	   *destmap = ojmaps + kmrels->cuda_dindex * nrooms;
	cl_uint		i, j, map;

	for (i = get_global_id();
		 i < nrooms;
		 i += get_global_size())
	{
		map = 0;
		for (j = 0; j <= num_devices; j++)
		{
			map |= ojmaps[i];
			ojmaps += nrooms;
		}
		destmap[i] = map;
	}
}

/*
 * gpujoin_right_outer
 */
KERNEL_FUNCTION(void)
gpujoin_right_outer(kern_gpujoin *kgjoin,
					kern_multirels *kmrels,
					cl_int outer_depth,
					kern_data_store *kds_dst,
					kern_parambuf *kparams_gpreagg)
{
	kern_parambuf  *kparams = KERN_GPUJOIN_PARAMBUF(kgjoin);
	kern_context	kcxt;
	kern_context	kcxt_gpreagg __attribute__((unused));
	cl_int			depth;
	cl_int			index;
	cl_uint			pstack_nrooms;
	cl_uint		   *pstack_base;
	cl_uint			l_state[GPUJOIN_MAX_DEPTH+1];
	cl_bool			matched[GPUJOIN_MAX_DEPTH+1];
	__shared__ cl_int depth_thread0 __attribute__((unused));

	INIT_KERNEL_CONTEXT(&kcxt, gpujoin_right_outer, kparams);
	assert(KERN_MULTIRELS_RIGHT_OUTER_JOIN(kmrels, outer_depth));
#ifndef GPUPREAGG_COMBINED_JOIN
	assert(kds_dst->format == KDS_FORMAT_ROW);
	assert(kparams_gpreagg == NULL);
#else
	assert(kds_dst->format == KDS_FORMAT_SLOT);
	assert(kparams_gpreagg != NULL);
	INIT_KERNEL_CONTEXT(&kcxt_gpreagg, gpujoin_right_outer, kparams_gpreagg);
#endif

	/* setup private variables */
	pstack_nrooms = kgjoin->pstack_nrooms;
	pstack_base = (cl_uint *)((char *)kgjoin + kgjoin->pstack_offset)
		+ get_global_index() * pstack_nrooms * ((GPUJOIN_MAX_DEPTH+1) *
												(GPUJOIN_MAX_DEPTH+2)) / 2;
	/* setup crc32 table */
	for (index = get_local_id();
		 index < lengthof(pg_crc32_table);
		 index += get_local_size())
		pg_crc32_table[index] = kmrels->pg_crc32_table[index];
	__syncthreads();

	/* setup per-depth context */
	memset(l_state, 0, sizeof(l_state));
	memset(matched, 0, sizeof(matched));
	if (get_local_id() == 0)
	{
		src_read_pos = UINT_MAX;
		stat_source_nitems = 0;
		memset(stat_nitems, 0, sizeof(stat_nitems));
		memset(read_pos, 0, sizeof(read_pos));
		memset(write_pos, 0, sizeof(write_pos));
		scan_done = false;
		base_depth = outer_depth;
	}
	__syncthreads();

	/* main logic of GpuJoin */
	depth = outer_depth;
	while (depth >= outer_depth)
	{
		if (depth == outer_depth)
		{
			/* makes RIGHT OUTER combinations using OUTER JOIN map */
			depth = gpujoin_load_outer(&kcxt,
									   kgjoin,
									   kmrels,
									   outer_depth,
									   PSTACK_DEPTH(outer_depth),
									   l_state);
		}
		else if (depth > kgjoin->num_rels)
		{
			assert(depth == kmrels->nrels + 1);
#ifndef GPUPREAGG_COMBINED_JOIN
			/* PROJECTION (ROW) */
			depth = gpujoin_projection_row(&kcxt,
										   kgjoin,
										   kmrels,
										   NULL,
										   kds_dst,
										   PSTACK_DEPTH(kgjoin->num_rels),
										   l_state,
										   matched);
#else
			/* PROJECTION (SLOT) */
			depth = gpujoin_projection_slot(&kcxt,
											&kcxt_gpreagg,
											kgjoin,
											kmrels,
											NULL,
											kds_dst,
											PSTACK_DEPTH(kgjoin->num_rels),
											l_state,
											matched);
#endif		/* GPUPREAGG_COMBINED_JOIN */
		}
		else if (kmrels->chunks[depth-1].is_nestloop)
		{
			/* NEST-LOOP */
			depth = gpujoin_exec_nestloop(&kcxt,
										  kgjoin,
										  kmrels,
										  NULL,
										  depth,
										  PSTACK_DEPTH(depth-1),
										  PSTACK_DEPTH(depth),
										  l_state,
										  matched);
		}
		else
		{
			/* HASH-JOIN */
			depth = gpujoin_exec_hashjoin(&kcxt,
										  kgjoin,
										  kmrels,
										  NULL,
										  depth,
										  PSTACK_DEPTH(depth-1),
										  PSTACK_DEPTH(depth),
										  l_state,
										  matched);
		}
		if (get_local_id() == 0)
			depth_thread0 = depth;
		__syncthreads();
		assert(depth == depth_thread0);
	}
	/* write out statistics */
	if (get_local_id() == 0)
	{
		gpujoin_suspend_block *sb = KERN_GPUJOIN_SUSPEND_BLOCK(kgjoin);
		sb->depth = -1;		/* no more suspend/resume! */

		assert(stat_source_nitems == 0);
		assert(stat_nitems[0] == 0);
		for (index = outer_depth; index <= GPUJOIN_MAX_DEPTH; index++)
		{
			atomicAdd(&kgjoin->stat_nitems[index-1],
					  stat_nitems[index]);
		}
	}
	__syncthreads();
	kern_writeback_error_status(&kgjoin->kerror, &kcxt.e);
}

#endif	/* __CUDACC__ */
#endif	/* CUDA_GPUJOIN_H */

/*
 * codegen.c
 *
 * Routines for xPU code generator
 * ----
 * Copyright 2011-2022 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2022 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */
#include "pg_strom.h"

/* -------- static variables --------*/
#define DEVTYPE_INFO_NSLOTS		128
#define DEVFUNC_INFO_NSLOTS		1024
static MemoryContext	devinfo_memcxt = NULL;
static dlist_head		devtype_info_slot[DEVTYPE_INFO_NSLOTS];
static dlist_head		devfunc_info_slot[DEVFUNC_INFO_NSLOTS];



#define TYPE_OPCODE(NAME,OID,EXTENSION)				\
	{EXTENSION, #NAME, DEVKERNEL__ANY, devtype_##NAME##_hash },
static struct {
	const char	   *type_extension;
	const char	   *type_name;
	uint32_t		type_flags;
	devtype_hashfunc_f type_hashfunc;
} devtype_catalog[] = {
#include "xpu_opcodes.h"
	{NULL, NULL, 0, NULL}
};
#undef TYPE_OPCODE




static const char *
get_extension_name_by_object(Oid class_id, Oid object_id)
{
	Oid		ext_oid = getExtensionOfObject(class_id, object_id);

	if (OidIsValid(ext_oid))
		return get_extension_name(ext_oid);
	return NULL;
}

static devtype_info *
build_basic_devtype_info(TypeCacheEntry *tcache, const char *ext_name)
{
	devtype_info   *dtype = NULL;
	HeapTuple		htup;
	Form_pg_type	__type;
	const char	   *type_name;
	Oid				type_namespace;
	int				i;

	htup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(tcache->type_id));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for type %u", tcache->type_id);
    __type = (Form_pg_type) GETSTRUCT(htup);
    type_name = NameStr(__type->typname);
	type_namespace = __type->typnamespace;
	
	for (i=0; devtype_catalog[i].type_name != NULL; i++)
	{
		const char	   *__ext_name = devtype_catalog[i].type_extension;
		const char	   *__type_name = devtype_catalog[i].type_name;
		MemoryContext	oldcxt;

		if (ext_name
			? (__ext_name && strcmp(ext_name, __ext_name) == 0)
			: (!__ext_name && type_namespace == PG_CATALOG_NAMESPACE) &&
			strcmp(type_name, __type_name) == 0)
		{
			oldcxt = MemoryContextSwitchTo(devinfo_memcxt);
			dtype = palloc0(offsetof(devtype_info, comp_subtypes[0]));
			if (ext_name)
				dtype->type_extension = pstrdup(ext_name);
			dtype->type_oid = tcache->type_id;
			dtype->type_flags = devtype_catalog[i].type_flags;
			dtype->type_length = tcache->typlen;
			dtype->type_align = typealign_get_width(tcache->typalign);
			dtype->type_byval = tcache->typbyval;
			dtype->type_name = pstrdup(type_name);
			dtype->type_hashfunc = devtype_catalog[i].type_hashfunc;
			/* type equality functions */
			dtype->type_eqfunc = get_opcode(tcache->eq_opr);
			dtype->type_cmpfunc = tcache->cmp_proc;

			MemoryContextSwitchTo(oldcxt);
			break;
		}
	}
	ReleaseSysCache(htup);

	return dtype;
}

static devtype_info *
build_composite_devtype_info(TypeCacheEntry *tcache, const char *ext_name)
{
	TupleDesc		tupdesc = lookup_rowtype_tupdesc(tcache->type_id, -1);
	devtype_info  **subtypes = alloca(sizeof(devtype_info *) * tupdesc->natts);
	devtype_info   *dtype;
	MemoryContext	oldcxt;
	uint32_t		extra_flags = DEVKERNEL__ANY;
	int				j;

	for (j=0; j < tupdesc->natts; j++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, j);

		dtype = pgstrom_devtype_lookup(attr->atttypid);
		if (!dtype)
			return NULL;
		extra_flags &= dtype->type_flags;
		subtypes[j] = dtype;
	}
	oldcxt = MemoryContextSwitchTo(devinfo_memcxt);
	dtype = palloc0(offsetof(devtype_info,
							 comp_subtypes[tupdesc->natts]));
	if (ext_name)
		dtype->type_extension = pstrdup(ext_name);
	dtype->type_oid = tcache->type_id;
    dtype->type_flags = extra_flags;
	dtype->type_length = tcache->typlen;
	dtype->type_align = typealign_get_width(tcache->typalign);
	dtype->type_byval = tcache->typbyval;
    dtype->type_name = get_type_name(tcache->type_id, false);
	dtype->type_hashfunc = devtype_composite_hash;
	dtype->comp_nfields = tupdesc->natts;
	memcpy(dtype->comp_subtypes, subtypes,
		   sizeof(devtype_info *) * tupdesc->natts);
	MemoryContextSwitchTo(oldcxt);

	return dtype;
}

static devtype_info *
build_array_devtype_info(TypeCacheEntry *tcache, const char *ext_name)
{
	devtype_info   *elem;
	devtype_info   *dtype;
	MemoryContext	oldcxt;

	elem = pgstrom_devtype_lookup(tcache->typelem);
	if (!elem)
		return NULL;

	oldcxt = MemoryContextSwitchTo(devinfo_memcxt);
	dtype = palloc0(offsetof(devtype_info, comp_subtypes[0]));
	if (ext_name)
		dtype->type_extension = pstrdup(ext_name);
	dtype->type_oid = tcache->type_id;
	dtype->type_flags = elem->type_flags;
	dtype->type_length = tcache->typlen;
	dtype->type_align = typealign_get_width(tcache->typalign);
	dtype->type_byval = tcache->typbyval;
	dtype->type_name = psprintf("%s[]", elem->type_name);
	dtype->type_hashfunc = devtype_array_hash;
	/* type equality functions */
	dtype->type_eqfunc = get_opcode(tcache->eq_opr);
	dtype->type_cmpfunc = tcache->cmp_proc;

	MemoryContextSwitchTo(oldcxt);

	return dtype;
}

devtype_info *
pgstrom_devtype_lookup(Oid type_oid)
{
	devtype_info   *dtype;
	uint32_t		hash;
	uint32_t		index;
	dlist_iter		iter;
	const char	   *ext_name;
	TypeCacheEntry *tcache;

	hash = GetSysCacheHashValue(TYPEOID, ObjectIdGetDatum(type_oid), 0, 0, 0);
	index = hash % DEVTYPE_INFO_NSLOTS;
	dlist_foreach (iter, &devtype_info_slot[index])
	{
		dtype = dlist_container(devtype_info, chain, iter.cur);

		if (dtype->type_oid == type_oid)
		{
			Assert(dtype->hash == hash);
			goto found;
		}
	}
	/* try to build devtype_info entry */
	ext_name = get_extension_name_by_object(TypeRelationId, type_oid);
	tcache = lookup_type_cache(type_oid,
							   TYPECACHE_EQ_OPR |
							   TYPECACHE_CMP_PROC);
	if (OidIsValid(tcache->typrelid))
	{
		/* composite type */
		dtype = build_composite_devtype_info(tcache, ext_name);
	}
	else if (OidIsValid(tcache->typelem) && tcache->typlen == -1)
	{
		/* array type */
		dtype = build_array_devtype_info(tcache, ext_name);
	}
	else
	{
		/* base type */
		dtype = build_basic_devtype_info(tcache, ext_name);
	}

	/* make a negative entry, if not device executable */
	if (!dtype)
	{
		dtype = MemoryContextAllocZero(devinfo_memcxt,
									   sizeof(devtype_info));
		dtype->type_oid = type_oid;
		dtype->type_is_negative = true;
	}
	dtype->hash = hash;
	dlist_push_head(&devtype_info_slot[index], &dtype->chain);
found:
	if (dtype->type_is_negative)
		return NULL;
	return dtype;
}

static void
pgstrom_devcache_invalidator(Datum arg, int cacheid, uint32 hashvalue)
{
	int		i;

	MemoryContextReset(devinfo_memcxt);
	for (i=0; i < DEVTYPE_INFO_NSLOTS; i++)
		dlist_init(&devtype_info_slot[i]);
	for (i=0; i < DEVFUNC_INFO_NSLOTS; i++)
		dlist_init(&devfunc_info_slot[i]);
}

void
pgstrom_init_codegen(void)
{
	devinfo_memcxt = AllocSetContextCreate(CacheMemoryContext,
										   "device type/func info cache",
										   ALLOCSET_DEFAULT_SIZES);
	pgstrom_devcache_invalidator(0, 0, 0);
	CacheRegisterSyscacheCallback(TYPEOID, pgstrom_devcache_invalidator, 0);
	CacheRegisterSyscacheCallback(PROCOID, pgstrom_devcache_invalidator, 0);
}

/*-------------------------------------------------------------------------
 *
 * rumscan.c
 *	  routines to manage scans of inverted index relations
 *
 *
 * Portions Copyright (c) 2015-2016, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "pgstat.h"
#include "utils/memutils.h"

#include "rum.h"

IndexScanDesc
rumbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	RumScanOpaque so;

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* allocate private workspace */
	so = (RumScanOpaque) palloc(sizeof(RumScanOpaqueData));
	so->sortstate = NULL;
	so->keys = NULL;
	so->nkeys = 0;
	so->firstCall = true;
	so->totalentries = 0;
	so->sortedEntries = NULL;
	so->tempCtx = AllocSetContextCreate(CurrentMemoryContext,
										"Rum scan temporary context",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);
	so->keyCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "Rum scan key context",
									   ALLOCSET_DEFAULT_MINSIZE,
									   ALLOCSET_DEFAULT_INITSIZE,
									   ALLOCSET_DEFAULT_MAXSIZE);

	initRumState(&so->rumstate, scan->indexRelation);

	scan->opaque = so;

	return scan;
}

/*
 * Create a new RumScanEntry, unless an equivalent one already exists,
 * in which case just return it
 */
static RumScanEntry
rumFillScanEntry(RumScanOpaque so, OffsetNumber attnum,
				 StrategyNumber strategy, int32 searchMode,
				 Datum queryKey, RumNullCategory queryCategory,
				 bool isPartialMatch, Pointer extra_data)
{
	RumState   *rumstate = &so->rumstate;
	RumScanEntry scanEntry;
	uint32		i;

	/*
	 * Look for an existing equivalent entry.
	 *
	 * Entries with non-null extra_data are never considered identical, since
	 * we can't know exactly what the opclass might be doing with that.
	 */
	if (extra_data == NULL || !isPartialMatch)
	{
		for (i = 0; i < so->totalentries; i++)
		{
			RumScanEntry prevEntry = so->entries[i];

			if (prevEntry->extra_data == NULL &&
				prevEntry->isPartialMatch == isPartialMatch &&
				prevEntry->strategy == strategy &&
				prevEntry->searchMode == searchMode &&
				prevEntry->attnum == attnum &&
				rumCompareEntries(rumstate, attnum,
								  prevEntry->queryKey,
								  prevEntry->queryCategory,
								  queryKey,
								  queryCategory) == 0)
			{
				/* Successful match */
				return prevEntry;
			}
		}
	}

	/* Nope, create a new entry */
	scanEntry = (RumScanEntry) palloc(sizeof(RumScanEntryData));
	scanEntry->queryKey = queryKey;
	scanEntry->queryCategory = queryCategory;
	scanEntry->isPartialMatch = isPartialMatch;
	scanEntry->extra_data = extra_data;
	scanEntry->strategy = strategy;
	scanEntry->searchMode = searchMode;
	scanEntry->forceUseBitmap = false;
	scanEntry->attnum = scanEntry->attnumOrig = attnum;

	scanEntry->buffer = InvalidBuffer;
	RumItemSetMin(&scanEntry->curRumKey);
	scanEntry->matchBitmap = NULL;
	scanEntry->matchIterator = NULL;
	scanEntry->matchResult = NULL;
	scanEntry->stack = NULL;
	scanEntry->scanWithAddInfo = false;
	scanEntry->list = NULL;
	scanEntry->nlist = 0;
	scanEntry->offset = InvalidOffsetNumber;
	scanEntry->isFinished = false;
	scanEntry->reduceResult = false;
	scanEntry->useMarkAddInfo = false;
	scanEntry->scanDirection = ForwardScanDirection;
	ItemPointerSetMin(&scanEntry->markAddInfo.iptr);

	return scanEntry;
}

/*
 * Initialize the next RumScanKey using the output from the extractQueryFn
 */
static void
rumFillScanKey(RumScanOpaque so, OffsetNumber attnum,
			   StrategyNumber strategy, int32 searchMode,
			   Datum query, uint32 nQueryValues,
			   Datum *queryValues, RumNullCategory * queryCategories,
			   bool *partial_matches, Pointer *extra_data,
			   bool orderBy)
{
	RumScanKey	key = palloc0(sizeof(*key));
	RumState   *rumstate = &so->rumstate;
	uint32		nUserQueryValues = nQueryValues;
	uint32		i;

	so->keys[so->nkeys++] = key;
	/* Non-default search modes add one "hidden" entry to each key */
	if (searchMode != GIN_SEARCH_MODE_DEFAULT)
		nQueryValues++;
	key->orderBy = orderBy;

	key->query = query;
	key->queryValues = queryValues;
	key->queryCategories = queryCategories;
	key->extra_data = extra_data;
	key->strategy = strategy;
	key->searchMode = searchMode;
	key->attnum = key->attnumOrig = attnum;
	key->useAddToColumn = false;
	key->scanDirection = ForwardScanDirection;

	RumItemSetMin(&key->curItem);
	key->curItemMatches = false;
	key->recheckCurItem = false;
	key->isFinished = false;

	key->addInfoKeys = NULL;
	key->addInfoNKeys = 0;

	if (key->orderBy && key->attnum == rumstate->attrnOrderByColumn)
	{
		if (nQueryValues != 1)
			elog(ERROR, "extractQuery should return only one value");
		if (rumstate->canOuterOrdering[attnum - 1] == false)
			elog(ERROR, "doesn't support ordering as additional info");

		key->useAddToColumn = true;
		key->attnum = rumstate->attrnAddToColumn;
		key->nentries = 0;
		key->nuserentries = 0;

		key->outerAddInfoIsNull = true;

		key->scanEntry = NULL;
		key->entryRes = NULL;
		key->addInfo = NULL;
		key->addInfoIsNull = NULL;

		return;
	}

	key->nentries = nQueryValues;
	key->nuserentries = nUserQueryValues;
	key->scanEntry = (RumScanEntry *) palloc(sizeof(RumScanEntry) * nQueryValues);
	key->entryRes = (bool *) palloc0(sizeof(bool) * nQueryValues);
	key->addInfo = (Datum *) palloc0(sizeof(Datum) * nQueryValues);
	key->addInfoIsNull = (bool *) palloc(sizeof(bool) * nQueryValues);
	for (i = 0; i < nQueryValues; i++)
		key->addInfoIsNull[i] = true;

	for (i = 0; i < nQueryValues; i++)
	{
		Datum		queryKey;
		RumNullCategory queryCategory;
		bool		isPartialMatch;
		Pointer		this_extra;

		if (i < nUserQueryValues)
		{
			/* set up normal entry using extractQueryFn's outputs */
			queryKey = queryValues[i];
			queryCategory = queryCategories[i];
			isPartialMatch =
				(rumstate->canPartialMatch[attnum - 1] && partial_matches)
				? partial_matches[i] : false;
			this_extra = (extra_data) ? extra_data[i] : NULL;
		}
		else
		{
			/* set up hidden entry */
			queryKey = (Datum) 0;
			switch (searchMode)
			{
				case GIN_SEARCH_MODE_INCLUDE_EMPTY:
					queryCategory = RUM_CAT_EMPTY_ITEM;
					break;
				case GIN_SEARCH_MODE_ALL:
					queryCategory = RUM_CAT_EMPTY_QUERY;
					break;
				case GIN_SEARCH_MODE_EVERYTHING:
					queryCategory = RUM_CAT_EMPTY_QUERY;
					break;
				default:
					elog(ERROR, "unexpected searchMode: %d", searchMode);
					queryCategory = 0;	/* keep compiler quiet */
					break;
			}
			isPartialMatch = false;
			this_extra = NULL;

			/*
			 * We set the strategy to a fixed value so that rumFillScanEntry
			 * can combine these entries for different scan keys.  This is
			 * safe because the strategy value in the entry struct is only
			 * used for partial-match cases.  It's OK to overwrite our local
			 * variable here because this is the last loop iteration.
			 */
			strategy = InvalidStrategy;
		}

		key->scanEntry[i] = rumFillScanEntry(so, attnum,
											 strategy, searchMode,
											 queryKey, queryCategory,
											 isPartialMatch, this_extra);
	}
}

static void
freeScanEntries(RumScanEntry *entries, uint32 nentries)
{
	uint32		i;

	for (i = 0; i < nentries; i++)
	{
		RumScanEntry entry = entries[i];

		if (entry->gdi)
		{
			freeRumBtreeStack(entry->gdi->stack);
			pfree(entry->gdi);
		}
		else
		{
			if (entry->buffer != InvalidBuffer)
				ReleaseBuffer(entry->buffer);
		}
		if (entry->stack)
			freeRumBtreeStack(entry->stack);
		if (entry->list)
			pfree(entry->list);
		if (entry->matchIterator)
			tbm_end_iterate(entry->matchIterator);
		if (entry->matchBitmap)
			tbm_free(entry->matchBitmap);
		pfree(entry);
	}
}

static void
freeScanKeys(RumScanOpaque so)
{
	freeScanEntries(so->entries, so->totalentries);

	MemoryContextReset(so->keyCtx);
	so->keys = NULL;
	so->nkeys = 0;

	if (so->sortedEntries)
		pfree(so->sortedEntries);
	so->entries = NULL;
	so->sortedEntries = NULL;
	so->totalentries = 0;
}

static void
initScanKey(RumScanOpaque so, ScanKey skey, bool *hasNullQuery)
{
	Datum	   *queryValues;
	int32		nQueryValues = 0;
	bool	   *partial_matches = NULL;
	Pointer	   *extra_data = NULL;
	bool	   *nullFlags = NULL;
	int32		searchMode = GIN_SEARCH_MODE_DEFAULT;

	/*
	 * We assume that RUM-indexable operators are strict, so a null query
	 * argument means an unsatisfiable query.
	 */
	if (skey->sk_flags & SK_ISNULL)
	{
		so->isVoidRes = true;
		return;
	}

	/* OK to call the extractQueryFn */
	queryValues = (Datum *)
		DatumGetPointer(FunctionCall7Coll(&so->rumstate.extractQueryFn[skey->sk_attno - 1],
						   so->rumstate.supportCollation[skey->sk_attno - 1],
										  skey->sk_argument,
										  PointerGetDatum(&nQueryValues),
										  UInt16GetDatum(skey->sk_strategy),
										  PointerGetDatum(&partial_matches),
										  PointerGetDatum(&extra_data),
										  PointerGetDatum(&nullFlags),
										  PointerGetDatum(&searchMode)));

	/*
	 * If bogus searchMode is returned, treat as RUM_SEARCH_MODE_ALL; note in
	 * particular we don't allow extractQueryFn to select
	 * RUM_SEARCH_MODE_EVERYTHING.
	 */
	if (searchMode < GIN_SEARCH_MODE_DEFAULT ||
		searchMode > GIN_SEARCH_MODE_ALL)
		searchMode = GIN_SEARCH_MODE_ALL;

	/* Non-default modes require the index to have placeholders */
	if (searchMode != GIN_SEARCH_MODE_DEFAULT)
		*hasNullQuery = true;

	/*
	 * In default mode, no keys means an unsatisfiable query.
	 */
	if (queryValues == NULL || nQueryValues <= 0)
	{
		if (searchMode == GIN_SEARCH_MODE_DEFAULT)
		{
			so->isVoidRes = true;
			return;
		}
		nQueryValues = 0;		/* ensure sane value */
	}

	/*
	 * If the extractQueryFn didn't create a nullFlags array, create one,
	 * assuming that everything's non-null.  Otherwise, run through the array
	 * and make sure each value is exactly 0 or 1; this ensures binary
	 * compatibility with the RumNullCategory representation. While at it,
	 * detect whether any null keys are present.
	 */
	if (nullFlags == NULL)
		nullFlags = (bool *) palloc0(nQueryValues * sizeof(bool));
	else
	{
		int32		j;

		for (j = 0; j < nQueryValues; j++)
		{
			if (nullFlags[j])
			{
				nullFlags[j] = true;	/* not any other nonzero value */
				*hasNullQuery = true;
			}
		}
	}
	/* now we can use the nullFlags as category codes */

	rumFillScanKey(so, skey->sk_attno,
				   skey->sk_strategy, searchMode,
				   skey->sk_argument, nQueryValues,
				   queryValues, (RumNullCategory *) nullFlags,
				   partial_matches, extra_data,
				   (skey->sk_flags & SK_ORDER_BY) ? true : false);
}

static ScanDirection
lookupScanDirection(RumState *state, AttrNumber attno, StrategyNumber strategy)
{
	int			i;
	RumConfig	*rumConfig = state->rumConfig + attno - 1;

	for(i = 0; rumConfig->strategyInfo[i].strategy != InvalidStrategy &&
			   i < MAX_STRATEGIES; i++)
	{
		if (rumConfig->strategyInfo[i].strategy == strategy)
			return rumConfig->strategyInfo[i].direction;

	}

	return NoMovementScanDirection;
}

static void
fillMarkAddInfo(RumScanOpaque so, RumScanKey orderKey)
{
	int i;

	for(i=0; i<so->nkeys; i++)
	{
		RumScanKey		scanKey = so->keys[i];
		ScanDirection	scanDirection;

		if (scanKey->orderBy)
			continue;

		if (scanKey->attnum == so->rumstate.attrnAddToColumn &&
			orderKey->attnum == so->rumstate.attrnAddToColumn &&
			(scanDirection = lookupScanDirection(&so->rumstate,
								orderKey->attnumOrig,
								orderKey->strategy)) != NoMovementScanDirection)
		{
			int j;

			if (so->naturalOrder != NoMovementScanDirection &&
				so->naturalOrder != scanDirection)
				elog(ERROR, "Could not scan in differ directions at the same time");

			for(j=0; j<scanKey->nentries; j++)
			{
				RumScanEntry	scanEntry = scanKey->scanEntry[j];

				if (scanEntry->useMarkAddInfo)
					elog(ERROR,"could not order by more than one operator");
				scanEntry->useMarkAddInfo = true;
				scanEntry->markAddInfo.addInfoIsNull = false;
				scanEntry->markAddInfo.addInfo = orderKey->queryValues[0];
				scanEntry->scanDirection = scanDirection;
			}

			scanKey->scanDirection = scanDirection;
			so->naturalOrder = scanDirection;
		}
	}
}

static void
adjustScanDirection(RumScanOpaque so)
{
	int i;

	if (so->naturalOrder == NoMovementScanDirection)
		return;

	for(i=0; i<so->nkeys; i++)
	{
		RumScanKey	scanKey = so->keys[i];

		if (scanKey->orderBy)
			continue;

		if (scanKey->attnum == so->rumstate.attrnAddToColumn)
		{
			if (scanKey->scanDirection != so->naturalOrder)
			{
				int	j;

				if (scanKey->scanDirection != NoMovementScanDirection)
					elog(ERROR, "Could not scan in differ directions at the same time");

				scanKey->scanDirection = so->naturalOrder;
				for(j=0; j<scanKey->nentries; j++)
				{
					RumScanEntry	scanEntry = scanKey->scanEntry[j];

					scanEntry->scanDirection = so->naturalOrder;
				}
			}
		}
	}
}

void
rumNewScanKey(IndexScanDesc scan)
{
	RumScanOpaque so = (RumScanOpaque) scan->opaque;
	int			i;
	bool		hasNullQuery = false;
	bool		checkEmptyEntry = false;
	MemoryContext oldCtx;
	enum {
		haofNone = 0x00,
		haofHasAddOnRestriction = 0x01,
		haofHasAddToRestriction = 0x02
	}		hasAddOnFilter = haofNone;

	so->naturalOrder = NoMovementScanDirection;

	/*
	 * Allocate all the scan key information in the key context. (If
	 * extractQuery leaks anything there, it won't be reset until the end of
	 * scan or rescan, but that's OK.)
	 */
	oldCtx = MemoryContextSwitchTo(so->keyCtx);

	/* if no scan keys provided, allocate extra EVERYTHING RumScanKey */
	so->keys = (RumScanKey*)
		palloc((Max(scan->numberOfKeys, 1) + scan->numberOfOrderBys) *
			   sizeof(*so->keys));
	so->nkeys = 0;

	so->isVoidRes = false;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		initScanKey(so, &scan->keyData[i], &hasNullQuery);
		if (so->isVoidRes)
			break;
	}

	/*
	 * If there are no regular scan keys, generate an EVERYTHING scankey to
	 * drive a full-index scan.
	 */
	if (so->nkeys == 0 && !so->isVoidRes)
	{
		hasNullQuery = true;
		rumFillScanKey(so, FirstOffsetNumber,
					   InvalidStrategy,
					   GIN_SEARCH_MODE_EVERYTHING,
					   (Datum) 0, 0,
					   NULL, NULL, NULL, NULL, false);
		checkEmptyEntry = true;
	}

	for (i = 0; i < scan->numberOfOrderBys; i++)
	{
		initScanKey(so, &scan->orderByData[i], &hasNullQuery);
		if (so->isVoidRes)
			break;
	}

	/*
	 * Fill markAddInfo if possible
	 */
	for (i = 0; i < so->nkeys; i++)
	{
		RumScanKey  key = so->keys[i];

		if (so->rumstate.useAlternativeOrder &&
			key->orderBy && key->useAddToColumn &&
			key->attnum == so->rumstate.attrnAddToColumn)
			fillMarkAddInfo(so, key);

		if (key->orderBy == false)
		{
			if (key->attnumOrig == so->rumstate.attrnAddToColumn)
				hasAddOnFilter |= haofHasAddToRestriction;
			if (key->attnumOrig == so->rumstate.attrnOrderByColumn)
				 hasAddOnFilter |= haofHasAddOnRestriction;
		}
	}

	if ((hasAddOnFilter & haofHasAddToRestriction) &&
		(hasAddOnFilter & haofHasAddOnRestriction))
	{
		RumScanKey *keys = palloc(sizeof(*keys) * so->nkeys);
		int			nkeys = 0,
					j;
		RumScanKey	addToKey = NULL;

		for(i=0; i<so->nkeys; i++)
		{
			RumScanKey  key = so->keys[i];

			if (key->orderBy == false &&
				key->attnumOrig == so->rumstate.attrnOrderByColumn)
			{
				for(j=0; addToKey == NULL && j<so->nkeys; j++)
					if (so->keys[j]->orderBy == false &&
						so->keys[j]->attnumOrig == so->rumstate.attrnAddToColumn)
					{
						addToKey = so->keys[j];

						addToKey->addInfoKeys =
							palloc(sizeof(*addToKey->addInfoKeys) * so->nkeys);
					}

				if (addToKey == NULL)
					keys[nkeys++] = key;
				else
					addToKey->addInfoKeys[ addToKey->addInfoNKeys++ ] = key;
			}
			else
			{
				keys[nkeys++] = key;
			}
		}

		pfree(so->keys);
		so->keys = keys;
		so->nkeys = nkeys;
	}

	adjustScanDirection(so);

	/* initialize expansible array of RumScanEntry pointers */
	so->totalentries = 0;
	so->allocentries = 32;
	so->entries = (RumScanEntry *)
		palloc(so->allocentries * sizeof(RumScanEntry));
	so->sortedEntries = NULL;

	for(i = 0; i<so->nkeys; i++)
	{
		RumScanKey  key = so->keys[i];

		/* Add it to so's array */
		while (so->totalentries + key->nentries >= so->allocentries)
		{
			so->allocentries *= 2;
			so->entries = (RumScanEntry *)
				repalloc(so->entries, so->allocentries * sizeof(RumScanEntry));
		}

		memcpy(so->entries + so->totalentries,
			   key->scanEntry, sizeof(*key->scanEntry) * key->nentries);
		so->totalentries += key->nentries;
	}

	/*
	 * If there are order-by keys, mark empty entry for scan with add info.
	 * If so->nkeys > 1 then there are order-by keys.
	 */
	if (checkEmptyEntry && so->nkeys > 1)
	{
		Assert(so->totalentries > 0);
		so->entries[0]->scanWithAddInfo = true;
	}

	if (scan->numberOfOrderBys > 0)
	{
		scan->xs_orderbyvals = palloc0(sizeof(Datum) * scan->numberOfOrderBys);
		scan->xs_orderbynulls = palloc(sizeof(bool) * scan->numberOfOrderBys);
		memset(scan->xs_orderbynulls, true, sizeof(bool) *
			   scan->numberOfOrderBys);
	}

	MemoryContextSwitchTo(oldCtx);

	pgstat_count_index_scan(scan->indexRelation);
}

void
rumrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		  ScanKey orderbys, int norderbys)
{
	/* remaining arguments are ignored */
	RumScanOpaque so = (RumScanOpaque) scan->opaque;

	so->firstCall = true;

	freeScanKeys(so);

	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	if (orderbys && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys,
				scan->numberOfOrderBys * sizeof(ScanKeyData));

	if (so->sortstate)
	{
		rum_tuplesort_end(so->sortstate);
		so->sortstate = NULL;
	}
}

void
rumendscan(IndexScanDesc scan)
{
	RumScanOpaque so = (RumScanOpaque) scan->opaque;

	freeScanKeys(so);

	if (so->sortstate)
		rum_tuplesort_end(so->sortstate);

	MemoryContextDelete(so->tempCtx);
	MemoryContextDelete(so->keyCtx);

	pfree(so);
}

Datum
rummarkpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "RUM does not support mark/restore");
	PG_RETURN_VOID();
}

Datum
rumrestrpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "RUM does not support mark/restore");
	PG_RETURN_VOID();
}

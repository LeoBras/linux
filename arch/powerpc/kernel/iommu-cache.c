// SPDX-License-Identifier: GPL-2.0-or-later

#include <asm/iommu-cache.h>

struct iommu_pagecache_entry {
	struct llist_node fifo;
	struct llist_node next_map;	/* Next mapping for this cpu page */
	unsigned long dmapage;
	unsigned long cpupage;
	atomic_t count;		/* Usage count */
	enum dma_data_direction direction;

};

struct iommu_pagecache_unmap_entry {
	struct list_head list;
	unsigned long dmabase;
	unsigned long size;
};

#define IOMMU_CACHE_MAX		75	/* percent of the total pages */
#define IOMMU_CACHE_THRES	128	/* pages */
#define IOMMU_CACHE_REMOVING	0x0deadbee

static inline bool iommu_pagecache_use_one(struct iommu_pagecache_entry *d)
{
	int r = atomic_fetch_add_unless(&d->count, 1, -IOMMU_CACHE_REMOVING);

	return (r != -IOMMU_CACHE_REMOVING);
}

static inline bool iommu_pagecache_use_range(struct iommu_pagecache *cache,
					     struct iommu_pagecache_entry *d, unsigned int npages,
					     enum dma_data_direction direction)
{
	int i;
	struct iommu_pagecache_entry *tmp;
	const unsigned long dmapage = d->dmapage;
	const unsigned long cpupage = d->cpupage;

	/* Reserve first page*/
	if (!iommu_pagecache_use_one(d))
		return false;

	/* Start from the last entry, to fail fast if the whole range is not available */
	for (i = npages - 1; i > 0; i--) {
		tmp = xa_load(&cache->dmapages, dmapage + i);
		if (!tmp || tmp->cpupage != (cpupage + i))
			goto out_reverse;

		if (!DMA_DIR_COMPAT(tmp->direction, direction))
			goto out_reverse;

		if (!iommu_pagecache_use_one(d))
			goto out_reverse;
	}

	return true;

out_reverse:
	for (i++; i < npages; i++) {
		tmp = xa_load(&cache->dmapages, dmapage + i);
		if (tmp)
			atomic_dec(&tmp->count);
	}

	return false;
}

/**
 * iommu_pagecache_use() - Looks for a DMA mapping in cache
 * @tbl: Device's iommu_table.
 * @page: Address for which a DMA mapping is desired.
 * @npages: Page count needed from that address
 * @direction: DMA direction needed for the mapping
 *
 * Looks into the iommu pagecache for a page/range that is already mapped with given direction.
 *
 * Return: DMA mapping for range/direction present in cache
 *	   DMA_MAPPING_ERROR if not found.
 */
dma_addr_t iommu_pagecache_use(struct iommu_table *tbl, void *page, unsigned int npages,
			       enum dma_data_direction direction)
{
	struct iommu_pagecache_entry *d;
	const unsigned long p = (unsigned long)page >> tbl->it_page_shift;

	d = xa_load(&tbl->cache.cpupages, p);
	if (!d)
		return DMA_MAPPING_ERROR;

	llist_for_each_entry(d, &d->next_map, next_map) {
		if (p != d->cpupage || !DMA_DIR_COMPAT(d->direction, direction))
			continue;

		if (iommu_pagecache_use_range(&tbl->cache, d, npages, direction))
			return d->dmapage << tbl->it_page_shift;
	}

	return DMA_MAPPING_ERROR;
}

/**
 * iommu_pagecache_cpupage_update() - Update cpupages with a new entry
 * @cache: Device's iommu_pagecache.
 * @e: Entry to be added to cpupages
 * @cpupage: CPU page number (index for cpupages).
 */
static inline void iommu_pagecache_cpupage_update(struct iommu_pagecache *cache,
						  struct iommu_pagecache_entry *e,
						  unsigned long cpupage)
{
	struct llist_node *n;
	struct iommu_pagecache_entry *first;

	for (;;) {
		while (xa_is_err(e = xa_store(&cache->cpupages, cpupage, e, GFP_ATOMIC)))
			pr_err("%s: Failed to store entry %p to cpu pagecache xarray.\n",
			       __func__, e);

		if (likely(!e))
			return;

		/* Something got stored between xa_erase and xa_store (unlikely) */

		first = xa_erase(&cache->cpupages, cpupage);

		/* Find last valid node */
		for (n = &e->next_map; n->next; n = n->next)
			;

		xchg(&n->next, &first->next_map);
	}
}

/**
 * iommu_pagecache_entry_remove() - Remove a dma mapping from cpupage & dmapage XArrays
 * @cache: Device's iommu_pagecache.
 * @d: dma_mapping to be removed
 */
static inline void iommu_pagecache_entry_remove(struct iommu_pagecache *cache,
						struct iommu_pagecache_entry *d)
{
	struct iommu_pagecache_entry *e, *first, *tmp;

	first = xa_erase(&cache->cpupages, d->cpupage);
	if (!first || xa_is_err(first)) {
		pr_err("%s: Entry for page %lx not found.\n", __func__, d->cpupage);
		goto free_dma;
	}

	if (d == first) {
		if (!d->next_map.next)
			goto free_dma;

		first = llist_entry(d->next_map.next, struct iommu_pagecache_entry, next_map);
		goto upd_cpu;
	}

	/* Find the previous entry in this cpupage */
//	tmp = NULL; //TODO change this behavior
	llist_for_each_entry(e, &first->next_map, next_map) {
		if (e == d) {
			tmp->next_map.next = e->next_map.next;
			goto upd_cpu;
		}
		tmp = e;
	}

	goto free_dma;

upd_cpu:
	iommu_pagecache_cpupage_update(cache, first, d->cpupage);

free_dma:
	xa_erase(&cache->dmapages, d->dmapage);
}

static inline bool iommu_pagecache_unmap_add(struct list_head **l, unsigned long dmapage)
{
	struct iommu_pagecache_unmap_entry *u;

	if (!*l)
		goto new_entry;

	list_for_each_entry(u, *l, list) {
		if (dmapage == u->dmabase + u->size) {
			u->size++;
			return true;
		}
	}

new_entry:
	u = kmalloc(sizeof(*u), GFP_ATOMIC);
	if (!u)
		return false;

	u->dmabase = dmapage;
	u->size = 1;

	if (*l)
		list_add(&u->list, *l);
	else
		INIT_LIST_HEAD(*l = &u->list);

	return true;
}

static inline void iommu_pagecache_unmap(struct iommu_table *tbl, struct list_head *l)
{
	struct iommu_pagecache_unmap_entry *u, *tmp;
	unsigned long freed = 0;

	list_for_each_entry_safe(u, tmp, l, list) {
		__iommu_free(tbl, u->dmabase << tbl->it_page_shift, u->size);
		freed += u->size;
		kfree(u);
	}

	atomic64_sub(freed, &tbl->cache.cachesize);
}

/**
 * iommu_pagecache_clean() - Clean count mappings from iommu_pagecache fifo
 * @tbl: Device's iommu_table.
 * @count: number of entries to be removed.
 */
static void iommu_pagecache_clean(struct iommu_table *tbl, const long count)
{
	struct iommu_pagecache_entry *d, *tmp;
	struct llist_node *n;
	struct iommu_pagecache *cache = &tbl->cache;
	unsigned long removed = 0;
	struct list_head *l = NULL;
	int r;

	n = llist_del_all(&cache->fifo_del);
	if (!n)
		return;

	llist_for_each_entry_safe(d, tmp, n, fifo) {
		r = atomic_sub_return_relaxed(IOMMU_CACHE_REMOVING, &d->count);
		if (r != -IOMMU_CACHE_REMOVING) {
			/* In use. Re-add it to list. */
			n = xchg(&cache->fifo_add.first, &d->fifo);
			if (n)
				n->next = &d->fifo;

			atomic_add(IOMMU_CACHE_REMOVING, &d->count);
		}

		/* Count was 0, fully remove entry */
		iommu_pagecache_entry_remove(cache, d);
		iommu_pagecache_unmap_add(&l, d->dmapage);
		kfree(d);

		if (++removed >= count)
			break;
	}

	/* No entry removed */
	if (!l)
		return;

	xchg(&cache->fifo_del.first, &tmp->fifo);
	iommu_pagecache_unmap(tbl, l);
}

/**
 * iommu_pagecache_free() - Decrement a mapping usage from iommu_pagecache and clean when full
 * @tbl: Device's iommu_table.
 * @dma_handle: DMA address from the mapping.
 * @npages: Page count from that address
 *
 * Decrements an atomic counter for a mapping in this dma_handle + npages, and remove
 * some unused dma mappings from iommu_pagecache fifo.
 */
void iommu_pagecache_free(struct iommu_table *tbl, dma_addr_t dma_handle, unsigned int npages)
{
	struct iommu_pagecache_entry *d;
	long exceeding;
	unsigned long dmapage = dma_handle >> tbl->it_page_shift;
	unsigned long dmapage_end = dmapage + npages;
	struct list_head *l = NULL;

	for (; dmapage < dmapage_end; dmapage++) {
		d = xa_load(&tbl->cache.dmapages, dmapage);
		if (d) {
			atomic_dec(&d->count);
			continue;
		}
		iommu_pagecache_unmap_add(&l, dmapage);
	}

	if (l)
		iommu_pagecache_unmap(tbl, l);

	exceeding = atomic64_read(&tbl->cache.cachesize) - tbl->cache.max_cachesize;
	if (exceeding > 0)
		iommu_pagecache_clean(tbl, exceeding + IOMMU_CACHE_THRES);
}

/**
 * iommu_pagecache_add() - Create and add a new dma_mapping into cache.
 * @tbl: Device's iommu_table.
 * @page: Address for which a DMA mapping was created.
 * @npages: Page count mapped from that address
 * @addr: DMA address created for that mapping
 * @direction: DMA direction for the mapping created
 *
 * Create a dma_mapping and add it to dmapages and cpupages XArray, then add it to fifo.
 * On both cpupages and dmapages, an entry will be created for each page in the mapping.
 */
void iommu_pagecache_add(struct iommu_table *tbl, void *page, unsigned int npages, dma_addr_t addr,
			 enum dma_data_direction direction)
{
	struct iommu_pagecache_entry *d, *tmp;
	struct llist_node *n;
	unsigned long cpupage, dmapage;
	unsigned int i;

	/* Increment cachesize even if adding fails: avoid too many fails causing starvation */
	atomic64_add(npages, &tbl->cache.cachesize);

	cpupage = (unsigned long)page >> tbl->it_page_shift;
	dmapage = (unsigned long)addr >> tbl->it_page_shift;

	for (i = 0; i < npages ; i++) {
		d = kmalloc(sizeof(*d), GFP_ATOMIC);
		if (!d)
			return;

		d->cpupage = cpupage + i;
		d->dmapage = dmapage + i;
		d->direction = direction;
		d->fifo.next = NULL;
		d->next_map.next = NULL;
		atomic_set(&d->count, 1);

		tmp = xa_store(&tbl->cache.dmapages, dmapage + i, d, GFP_ATOMIC);
		if (xa_is_err(tmp)) {
			kfree(d);
			break;
		}

		tmp = xa_store(&tbl->cache.cpupages, cpupage + i, d, GFP_ATOMIC);
		if (xa_is_err(tmp)) {
			xa_erase(&tbl->cache.dmapages, dmapage + i);
			kfree(d);
			break;
		}

		if (tmp)	/* Entry was present */
			xchg(&d->next_map.next, &tmp->next_map);

		n = xchg(&tbl->cache.fifo_add.first, &d->fifo);
		if (n)
			n->next = &d->fifo;
	}
}

/**
 * iommu_pagecache_destroy() - Free iommu_cache resources
 * @tbl: Device's iommu_table.
 *
 * Destroy a previously initialized iommu_cache and free resources
 */
void iommu_pagecache_destroy(struct iommu_table *tbl)
{
	iommu_pagecache_clean(tbl, atomic64_read(&tbl->cache.cachesize));

	xa_destroy(&tbl->cache.cpupages);
	xa_destroy(&tbl->cache.dmapages);
}

/**
 * iommu_pagecache_init() - Setup a iommu_cache for given iommu_table.
 * @tbl: Device's iommu_table.
 */
void iommu_pagecache_init(struct iommu_table *tbl)
{
	struct iommu_pagecache *cache = &tbl->cache;
	struct iommu_pagecache_entry *d;

	init_llist_head(&cache->fifo_add);
	init_llist_head(&cache->fifo_del);

	/* First entry for linking both llist_heads */
	d = kmalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		panic("%s: Can't allocate %ld bytes\n", __func__, sizeof(*d));

	llist_add(&d->fifo, &cache->fifo_add);
	llist_add(&d->fifo, &cache->fifo_del);

	d->cpupage = -1UL;
	d->dmapage = -1UL;
	d->direction = DMA_NONE;
	atomic_set(&d->count, 1); //Needs to be bigger than 0, so it can't be freed

	xa_init(&cache->cpupages);
	xa_init(&cache->dmapages);

	atomic64_set(&cache->cachesize, 0);
	cache->max_cachesize = (IOMMU_CACHE_MAX * tbl->it_size) / 100;
}

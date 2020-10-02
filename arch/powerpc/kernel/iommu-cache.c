// SPDX-License-Identifier: GPL-2.0-or-later

#include <asm/iommu-cache.h>

struct dma_mapping {
	struct llist_node fifo;
	unsigned long dmapage;
	unsigned long cpupage;
	unsigned long size;
	atomic_t count;
	enum dma_data_direction direction;

};

struct cpupage_entry {
	struct llist_node node;
	struct dma_mapping *data;
};

#define IOMMU_CACHE_MAX		75	/* percent of the total pages */
#define IOMMU_CACHE_THRES	128	/* pages */
#define IOMMU_CACHE_REMOVING	0x0deadbee

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
	struct cpupage_entry *e;
	struct dma_mapping *d;
	const unsigned long start = (unsigned long)page >> tbl->it_page_shift;
	const unsigned long end = start + npages;
	int r;

	e = xa_load(&tbl->cache.cpupages, start);
	if (!e)
		return DMA_MAPPING_ERROR;

	llist_for_each_entry(e, &e->node, node) {
		d = e->data;
		if (start < d->cpupage || end > (d->cpupage + d->size) ||
		    !DMA_DIR_COMPAT(d->direction, direction))
			continue;

		r = atomic_fetch_add_unless(&d->count, 1, -IOMMU_CACHE_REMOVING);
		if (r == -IOMMU_CACHE_REMOVING)
			continue;

		return (d->dmapage + start - d->cpupage) << tbl->it_page_shift;
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
						  struct cpupage_entry *e, unsigned long cpupage)
{
	struct llist_node *n;
	struct cpupage_entry *first;

	for (;;) {
		while (xa_is_err(e = xa_store(&cache->cpupages, cpupage, e, GFP_ATOMIC)))
			pr_err("%s: Failed to store entry %p to cpu pagecache xarray.\n",
			       __func__, e);

		if (likely(!e))
			break;

		/* Something got stored between xa_erase and xa_store (unlikely) */

		first = xa_erase(&cache->cpupages, cpupage);

		/* Find last valid node */
		for (n = &e->node; n->next; n = n->next)
			;

		xchg(&n->next, &first->node);
	}
}

/**
 * iommu_pagecache_entry_remove() - Remove a dma mapping from cpupage & dmapage XArrays
 * @cache: Device's iommu_pagecache.
 * @d: dma_mapping to be removed
 */
static inline void iommu_pagecache_entry_remove(struct iommu_pagecache *cache,
						struct dma_mapping *d)
{
	struct cpupage_entry *e, *first, *tmp;
	unsigned long cpupage = d->cpupage;
	unsigned long cpupage_end = cpupage + d->size;

	for (; cpupage < cpupage_end; cpupage++) {
		first = xa_erase(&cache->cpupages, cpupage);
		if (!first || xa_is_err(first)) {
			pr_err("%s: Entry for page %lx not found.\n", __func__, cpupage);
			continue;
		}

		/* Find the entry that contains the dma_mapping */
		tmp = NULL; //TODO change this behavior
		llist_for_each_entry(e, &first->node, node) {
			if (e->data == d)
				break;
			tmp = e;
		}

		if (e->data != d)
			continue;

		if (e != first) {
			tmp->node.next = e->node.next;
			tmp = first;
		} else if (e->node.next) {
			tmp = llist_entry(e->node.next, struct cpupage_entry, node);
		}

		kfree(e);

		if (tmp)
			iommu_pagecache_cpupage_update(cache, tmp, cpupage);
	}

	xa_erase(&cache->dmapages, d->dmapage);
}

/**
 * iommu_pagecache_clean() - Clean count mappings from iommu_pagecache fifo
 * @tbl: Device's iommu_table.
 * @count: number of entries to be removed.
 */
static void iommu_pagecache_clean(struct iommu_table *tbl, const long count)
{
	struct dma_mapping *d, *tmp;
	struct llist_node *n;
	struct iommu_pagecache *cache = &tbl->cache;
	unsigned long removed = 0;
	int r;

	n = llist_del_all(&cache->fifo_del);
	if (!n)
		return;

	llist_for_each_entry_safe(d, tmp, n, fifo) {
		r = atomic_sub_return_relaxed(IOMMU_CACHE_REMOVING, &d->count);
		if (r == -IOMMU_CACHE_REMOVING) {
			/* If count was 0, fully remove entry */
			iommu_pagecache_entry_remove(cache, d);
			__iommu_free(tbl, d->dmapage << tbl->it_page_shift, d->size);
			removed += d->size;
			kfree(d);
		} else {
			/* In use. Re-add it to list. */
			n = xchg(&cache->fifo_add.first, &d->fifo);
			if (n)
				n->next = &d->fifo;

			atomic_add(IOMMU_CACHE_REMOVING, &d->count);
		}

		if (removed >= count)
			break;
	}

	atomic64_sub(removed, &cache->cachesize);

	xchg(&cache->fifo_del.first, &tmp->fifo);
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
	struct dma_mapping *d;
	long exceeding;
	unsigned long dmapage = dma_handle >> tbl->it_page_shift;

	d = xa_load(&tbl->cache.dmapages, dmapage);
	if (!d) {
		/* Not in list, just free */
		__iommu_free(tbl, dma_handle, npages);
		return;
	}

	atomic_dec(&d->count);

	exceeding = atomic64_read(&tbl->cache.cachesize) - tbl->cache.max_cachesize;

	if (exceeding > 0)
		iommu_pagecache_clean(tbl, exceeding + IOMMU_CACHE_THRES);
}

/**
 * iommu_pagecache_entry_add() - Add a new dma_mapping into cache.
 * @cache: Device's iommu_pagecache.
 * @d: dma_mapping to be added
 * @cpupage: CPU page number (index for cpupages).
 *
 * Add dma_mapping to cpupages XArray.
 * An entry will be created for each page in the mapping. As there may exist many mappings for a
 * single cpupage, each entry has a llist that starts at the last mapped entry.
 *
 * Return: true if the mapping was correctly added to cpupages
 *	   false otherwisee
 */
static inline bool iommu_pagecache_entry_add(struct iommu_pagecache *cache, struct dma_mapping *d,
					     unsigned long cpupage)
{
	struct cpupage_entry *e, *old;

	/* Multiple mappings may exist for a page, get them in a list*/
	e = kmalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return false;

	e->data = d;
	e->node.next = NULL;

	old = xa_store(&cache->cpupages, cpupage, e, GFP_ATOMIC);
	if (!xa_is_err(old)) {
		if (old)
			xchg(&e->node.next, &old->node);

		return true;
	}

	kfree(e);
	return false;
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
	struct dma_mapping *d, *tmp;
	struct llist_node *n;
	unsigned long cpupage, dmapage;
	unsigned int i;

	/* Increment cachesize even if adding fails: avoid too many fails causing starvation */
	atomic64_add(npages, &tbl->cache.cachesize);

	d = kmalloc(sizeof(*d), GFP_ATOMIC);
	if (!d)
		return;

	cpupage = (unsigned long)page >> tbl->it_page_shift;
	dmapage = (unsigned long)addr >> tbl->it_page_shift;

	d->cpupage = cpupage;
	d->dmapage = dmapage;
	d->direction = direction;
	d->fifo.next = NULL;
	d->size = npages;
	atomic_set(&d->count, 1);

	/* Only one mapping may exist for a DMA address*/
	if (npages - 1)
		tmp = xa_store_range(&tbl->cache.dmapages, dmapage, dmapage + npages - 1, d,
				     GFP_ATOMIC);
	else
		tmp = xa_store(&tbl->cache.dmapages, dmapage, d, GFP_ATOMIC);

	if (xa_is_err(tmp))
		goto out_free;

	for (i = 0; i < npages ; i++) {
		if (!iommu_pagecache_entry_add(&tbl->cache, d, cpupage + i))
			break;
	}

	/* Failed adding the first page */
	if (i == 0)
		goto out_free;

	n = xchg(&tbl->cache.fifo_add.first, &d->fifo);
	if (n)
		n->next = &d->fifo;

	return;

out_free:
	kfree(d);
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
	struct dma_mapping *d;

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
	d->size = 0;
	atomic_set(&d->count, 1); //Needs to be bigger than 0, so it can't be freed

	xa_init(&cache->cpupages);
	xa_init(&cache->dmapages);

	atomic64_set(&cache->cachesize, 0);
	cache->max_cachesize = (IOMMU_CACHE_MAX * tbl->it_size) / 100;
}

// SPDX-License-Identifier: GPL-2.0-or-later

#include <asm/iommu-cache.h>

struct dma_mapping {
	struct llist_node fifo;
	unsigned long dmapage;
	unsigned long cpupage;
	unsigned long size;
	refcount_t count;
	enum dma_data_direction direction;

};

struct cpupage_entry {
	struct llist_node node;
	struct dma_mapping *data;
};

#define IOMMU_MAP_LIST_MAX	75	/* percent of the total pages */
#define IOMMU_MAP_LIST_THRES	128	/* pages */

/**
 * iommu_pagecache_use() - Looks for a DMA mapping in cache
 * @tbl: Device's iommu_table.
 * @page: Address for which a DMA mapping is desired.
 * @npages: Page count needed from that address
 * @direction: DMA direction needed for the mapping
 *
 * Looks into the DMA cache for a page/range that is already mapped with given direction.
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

	e = xa_load(&tbl->cache.cpupages, start);
	if (!e)
		return DMA_MAPPING_ERROR;

	llist_for_each_entry(e, &e->node, node) {
		d = e->data;
		if (start < d->cpupage || end > (d->cpupage + d->size) ||
		    !DMA_DIR_COMPAT(d->direction, direction))
			continue;

		refcount_inc(&d->count);
		return (d->dmapage + start - d->cpupage) << tbl->it_page_shift;
	}

	return DMA_MAPPING_ERROR;
}

/**
 * iommu_pagecache_entry_remove() - Remove a dma mapping from cpupage & dmapage XArrays
 * @cache: Device's dmacache.
 * @d: dma_mapping to be removed
 */
static void iommu_pagecache_entry_remove(struct dmacache *cache, struct dma_mapping *d)
{
	struct cpupage_entry *e, *tmp;
	dma_addr_t dp = d->dmapage;
	dma_addr_t end = dp + d->size;
	unsigned long cp = d->cpupage;

	for (; dp < end; dp++, cp++) {
		e = xa_erase(&cache->cpupages, cp);
		if (e && e->node.next) {
			tmp = llist_entry(e->node.next, struct cpupage_entry, node);
			xa_store(&cache->cpupages, cp, tmp, GFP_KERNEL);
		}

		kfree(e);
		xa_erase(&cache->dmapages, dp);
	}
}

/**
 * iommu_pagecache_clean() - Clean count mappings from dmacache fifo
 * @tbl: Device's iommu_table.
 * @count: number of entries to be removed.
 */
static void iommu_pagecache_clean(struct iommu_table *tbl, const long count)
{
	struct dma_mapping *d, *tmp;
	struct llist_node *n;
	struct dmacache *cache = &tbl->cache;
	unsigned long removed = 0;

	n = llist_del_all(&cache->fifo_del);
	if (!n)
		return;

	llist_for_each_entry_safe(d, tmp, n, fifo) {
		switch (refcount_read(&d->count)) {
		case 0:
			/* Fully remove entry */
			iommu_pagecache_entry_remove(cache, d);
			__iommu_free(tbl, d->dmapage << tbl->it_page_shift, d->size);
			removed += d->size;
			kfree(d);
			break;
		case 1:
			/* Remove entry but don't undo mapping */
			iommu_pagecache_entry_remove(cache, d);
			removed += d->size;
			kfree(d);
			break;
		default:
			/* In use. Re-add it to list. */
			n = xchg(&cache->fifo_add.first, &d->fifo);
			if (n)
				n->next = &d->fifo;

			break;
		}

		if (removed >= count)
			break;
	}

	atomic64_sub(removed, &cache->cachesize);

	xchg(&cache->fifo_del.first, &tmp->fifo);
}

/**
 * iommu_pagecache_free() - Decrement a mapping usage from dmacache and clean when full
 * @tbl: Device's iommu_table.
 * @dma_handle: DMA address from the mapping.
 * @npages: Page count from that address
 *
 * Decrements a refcount for a mapping in this dma_handle + npages, and remove
 * some unused dma mappings from dmacache fifo.
 */
void iommu_pagecache_free(struct iommu_table *tbl, dma_addr_t dma_handle, unsigned int npages)
{
	struct dma_mapping *d;
	long exceeding;

	d = xa_load(&tbl->cache.dmapages, dma_handle >> tbl->it_page_shift);
	if (!d) {
		/* Not in list, just free */
		__iommu_free(tbl, dma_handle, npages);
		return;
	}

	refcount_dec(&d->count);

	exceeding = atomic64_read(&tbl->cache.cachesize) - tbl->cache.max_cachesize;

	if (exceeding > 0)
		iommu_pagecache_clean(tbl, exceeding + IOMMU_MAP_LIST_THRES);
}

/**
 * iommu_pagecache_add() - Create and add a new dma mapping into cache.
 * @tbl: Device's iommu_table.
 * @page: Address for which a DMA mapping was created.
 * @npages: Page count mapped from that address
 * @addr: DMA address created for that mapping
 * @direction: DMA direction for the mapping created
 *
 * Create a dma_mapping and add it to dmapages and cpupages XArray, then add it to fifo.
 * On both cpupages and dmapages, an entry will be created for each page in the mapping.
 * On cpupages, as there may exist many mappings for a single cpupage, each entry has a llist
 * that starts at the last mapped entry.
 *
 */
void iommu_pagecache_add(struct iommu_table *tbl, void *page, unsigned int npages, dma_addr_t addr,
			 enum dma_data_direction direction)
{
	struct dma_mapping *d, *tmp;
	struct cpupage_entry *e, *old;
	struct llist_node *n;
	unsigned long p = (unsigned long)page;
	unsigned int i;

	d = kmalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return;

	d->cpupage = (unsigned long)p >> tbl->it_page_shift;
	d->dmapage = (unsigned long)addr >> tbl->it_page_shift;
	d->size = npages;
	d->direction = direction;
	d->fifo.next = NULL;
	refcount_set(&d->count, 1);

	p = d->cpupage;
	addr = d->dmapage;

	for (i = 0; i < npages ; i++) {
		/* Only one mapping may exist for a DMA address*/
		tmp = xa_store(&tbl->cache.dmapages, addr++, d, GFP_KERNEL);
		if (xa_is_err(tmp))
			break;

		/* Multiple mappings may exist for a page, get them in a list*/
		e = kmalloc(sizeof(*e), GFP_KERNEL);
		if (!d)
			break;

		e->data = d;
		old = xa_store(&tbl->cache.cpupages, p++, e, GFP_KERNEL);
		e->node.next = &old->node;

		if (xa_is_err(old)) {
			kfree(e);
			break;
		}
	}

	n = xchg(&tbl->cache.fifo_add.first, &d->fifo);
	if (n)
		n->next = &d->fifo;

	atomic64_add(npages, &tbl->cache.cachesize);
}

void iommu_pagecache_destroy(struct iommu_table *tbl)
{
	iommu_pagecache_clean(tbl, atomic64_read(&tbl->cache.cachesize));

	xa_destroy(&tbl->cache.cpupages);
	xa_destroy(&tbl->cache.dmapages);
}

void iommu_pagecache_init(struct iommu_table *tbl)
{
	struct dma_mapping *d;

	init_llist_head(&tbl->cache.fifo_add);
	init_llist_head(&tbl->cache.fifo_del);

	/* First entry for linking both llist_heads */
	d = kmalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		panic("%s: Can't allocate %ld bytes\n", __func__, sizeof(*d));

	llist_add(&d->fifo, &tbl->cache.fifo_add);
	llist_add(&d->fifo, &tbl->cache.fifo_del);

	d->cpupage = -1UL;
	d->dmapage = -1UL;
	d->direction = DMA_NONE;
	d->size = 0;
	refcount_set(&d->count, 0);

	xa_init(&tbl->cache.cpupages);
	xa_init(&tbl->cache.dmapages);

	atomic64_set(&tbl->cache.cachesize, 0);
	tbl->cache.max_cachesize = (IOMMU_MAP_LIST_MAX * tbl->it_size) / 100;
}

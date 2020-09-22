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
#define IOMMU_CACHE_REMOVE	0x0deadbee

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
	int r;

	e = xa_load(&tbl->cache.cpupages, start);
	if (!e)
		return DMA_MAPPING_ERROR;

	llist_for_each_entry(e, &e->node, node) {
		d = e->data;
		if (start < d->cpupage || end > (d->cpupage + d->size) ||
		    !DMA_DIR_COMPAT(d->direction, direction))
			continue;

		r = atomic_fetch_add_unless(&d->count, 1, -IOMMU_CACHE_REMOVE);
		if (r == -IOMMU_CACHE_REMOVE)
			continue;

		return (d->dmapage + start - d->cpupage) << tbl->it_page_shift;
	}

	return DMA_MAPPING_ERROR;
}

static inline void iommu_pagecache_entry_replace(struct dmacache *cache, struct cpupage_entry *e,
						 unsigned long cp)
{
	struct llist_node *n;
	struct cpupage_entry *first;

	for (;;) {
		while (xa_is_err(e = xa_store(&cache->cpupages, cp, e, GFP_ATOMIC)))
			pr_err("%s: Failed to store entry %p to cpu pagecache xarray.\n",
			       __func__, e);

		if (likely(!e))
			break;

		/* Something got stored between xa_erase and xa_store (unlikely) */

		first = xa_erase(&cache->cpupages, cp);

		/* Find last valid node */
		for (n = &e->node; n->next; n = n->next)
			;

		xchg(&n->next, &first->node);
	}
}

/**
 * iommu_pagecache_entry_remove() - Remove a dma mapping from cpupage & dmapage XArrays
 * @cache: Device's dmacache.
 * @d: dma_mapping to be removed
 */
static inline void iommu_pagecache_entry_remove(struct dmacache *cache, struct dma_mapping *d)
{
	struct cpupage_entry *e, *first, *tmp;
	dma_addr_t dp = d->dmapage;
	dma_addr_t end = dp + d->size;
	unsigned long cp = d->cpupage;

	for (; dp < end; dp++, cp++) {
		first = xa_erase(&cache->cpupages, cp);
		if (!first) {
			pr_err("%s: Entry for page %lx not found.\n", __func__, cp);
			goto next;
		}

		/* Find the entry that contains the dma_mapping */
		tmp = NULL;
		llist_for_each_entry(e, &first->node, node) {
			if (e->data == d)
				break;
			tmp = e;
		}

		if (tmp) {
			/* Entry to be removed is not the first entry */
			tmp->node.next = e->node.next;
			tmp = first;
		} else if (e->node.next) {
			tmp = llist_entry(e->node.next, struct cpupage_entry, node);
		}

		kfree(e);

		if (tmp)
			iommu_pagecache_entry_replace(cache, tmp, cp);
next:
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
	int r;

	n = llist_del_all(&cache->fifo_del);
	if (!n)
		return;

	llist_for_each_entry_safe(d, tmp, n, fifo) {
		r = atomic_sub_return_relaxed(IOMMU_CACHE_REMOVE, &d->count);
		if (r == -IOMMU_CACHE_REMOVE) {
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

			atomic_add(IOMMU_CACHE_REMOVE, &d->count);
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
 * Decrements a refcount (atomic) for a mapping in this dma_handle + npages, and remove
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

	atomic_dec(&d->count);

	exceeding = atomic64_read(&tbl->cache.cachesize) - tbl->cache.max_cachesize;

	if (exceeding > 0)
		iommu_pagecache_clean(tbl, exceeding + IOMMU_CACHE_THRES);
}


static inline bool iommu_pagecache_entry_add(struct dmacache *cache, struct dma_mapping *d,
					     unsigned long p, dma_addr_t addr)
{
	struct dma_mapping *tmp;
	struct cpupage_entry *e, *old;

	/* Only one mapping may exist for a DMA address*/
	tmp = xa_store(&cache->dmapages, addr, d, GFP_ATOMIC);
	if (xa_is_err(tmp))
		return false;

	/* Multiple mappings may exist for a page, get them in a list*/
	e = kmalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		goto out;

	e->data = d;

	old = xa_store(&cache->cpupages, p, e, GFP_ATOMIC);
	if (!xa_is_err(old)) {
		if (old)
			xchg(&e->node.next, &old->node);

		return true;
	}

	kfree(e);
out:
	xa_erase(&cache->dmapages, addr);
	return false;
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
	struct dma_mapping *d;
	struct llist_node *n;
	unsigned long p = (unsigned long)page;
	unsigned int i;

	d = kmalloc(sizeof(*d), GFP_ATOMIC);
	if (!d)
		return;

	d->cpupage = (unsigned long)p >> tbl->it_page_shift;
	d->dmapage = (unsigned long)addr >> tbl->it_page_shift;
	d->direction = direction;
	d->fifo.next = NULL;
	atomic_set(&d->count, 1);

	p = d->cpupage;
	addr = d->dmapage;

	for (i = 0; i < npages ; i++) {
		if (!iommu_pagecache_entry_add(&tbl->cache, d, p++, addr++))
			break;
	}

	if (i == 0) {
		/* Failed on adding the first page */
		kfree(d);
		return;
	}

	d->size = i;

	n = xchg(&tbl->cache.fifo_add.first, &d->fifo);
	if (n)
		n->next = &d->fifo;

	atomic64_add(i, &tbl->cache.cachesize);
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
	atomic_set(&d->count, 1); //Needs to be 1, so it doesn't try to free

	xa_init(&tbl->cache.cpupages);
	xa_init(&tbl->cache.dmapages);

	atomic64_set(&tbl->cache.cachesize, 0);
	tbl->cache.max_cachesize = (IOMMU_CACHE_MAX * tbl->it_size) / 100;
}

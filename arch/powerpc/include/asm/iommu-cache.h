/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _IOMMU_CACHE_H
#define _IOMMU_CACHE_H
#ifdef __KERNEL__
#ifdef CONFIG_IOMMU_PAGECACHE

#include <linux/llist.h>
#include <linux/xarray.h>
#include <linux/atomic.h>

struct iommu_pagecache {
	struct llist_head fifo_add;
	struct llist_head fifo_del;
	struct xarray cpupages;
	struct xarray dmapages;
	atomic64_t cachesize;
	unsigned long max_cachesize;
#ifdef IOMMU_PAGECACHE_DBG
	struct xarray debug;
#endif
};

#include <asm/iommu.h>

void iommu_pagecache_init(struct iommu_table *tbl);
void iommu_pagecache_destroy(struct iommu_table *tbl);
void _iommu_pagecache_add(struct iommu_table *tbl, void *page, unsigned int npages, dma_addr_t addr,
			  enum dma_data_direction direction);
dma_addr_t _iommu_pagecache_use(struct iommu_table *tbl, void *page, unsigned int npages,
				enum dma_data_direction direction);
void _iommu_pagecache_free(struct iommu_table *tbl, dma_addr_t dma_handle, unsigned int npages);

inline void iommu_pagecache_add(struct iommu_table *tbl, void *page, unsigned int npages,
				dma_addr_t addr, enum dma_data_direction direction)
{
	if (tbl->cache.max_cachesize)
		_iommu_pagecache_add(tbl, page, npages, addr, direction);
}

inline dma_addr_t iommu_pagecache_use(struct iommu_table *tbl, void *page, unsigned int npages,
				      enum dma_data_direction direction)
{
	if (tbl->cache.max_cachesize)
		_iommu_pagecache_use(tbl, page, npages, direction);
	else
		return DMA_MAPPING_ERROR;
}

inline void iommu_pagecache_free(struct iommu_table *tbl, dma_addr_t dma_handle,
				 unsigned int npages)
{
	if (tbl->cache.max_cachesize)
		_iommu_pagecache_free(tbl, dma_handle, npages);
	else
		__iommu_free(tbl, dma_handle, npages);
}

#else

struct iommu_pagecache {};

inline void iommu_pagecache_init(struct iommu_table *tbl) {}
inline void iommu_pagecache_destroy(struct iommu_table *tbl) {}
inline void iommu_pagecache_add(struct iommu_table *tbl, void *page,
				unsigned int npages, dma_addr_t addr,
				enum dma_data_direction direction) {}
inline dma_addr_t iommu_pagecache_use(struct iommu_table *tbl, void *page, unsigned int npages,
				      enum dma_data_direction direction)
{
	return DMA_MAPPING_ERROR;
}

inline void iommu_pagecache_free(struct iommu_table *tbl, dma_addr_t dma_handle,
				 unsigned int npages)
{
	__iommu_free(tbl, dma_handle, npages);
}

#endif /* CONFIG_IOMMU_PAGECACHE */
#endif /* __KERNEL__ */
#endif /* _IOMMU_CACHE_H */

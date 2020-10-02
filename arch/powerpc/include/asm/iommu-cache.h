/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _IOMMU_CACHE_H
#define _IOMMU_CACHE_H
#ifdef __KERNEL__

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
};

#include <asm/iommu.h>

void iommu_pagecache_init(struct iommu_table *tbl);
void iommu_pagecache_destroy(struct iommu_table *tbl);
void iommu_pagecache_add(struct iommu_table *tbl, void *page, unsigned int npages, dma_addr_t addr,
			 enum dma_data_direction direction);
dma_addr_t iommu_pagecache_use(struct iommu_table *tbl, void *page, unsigned int npages,
			       enum dma_data_direction direction);
void iommu_pagecache_free(struct iommu_table *tbl, dma_addr_t dma_handle, unsigned int npages);

#endif /* __KERNEL__ */
#endif /* _IOMMU_CACHE_H */

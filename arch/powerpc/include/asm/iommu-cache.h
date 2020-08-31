/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _IOMMU_CACHE_H
#define _IOMMU_CACHE_H
#ifdef __KERNEL__

#include <linux/llist.h>
#include <linux/xarray.h>
#include <linux/atomic.h>

struct dmacache {
	struct llist_head fifo_add;
	struct llist_head fifo_del;
	struct xarray cpupages;
	struct xarray dmapages;
	atomic64_t cachesize;
};

#include <asm/iommu.h>

void iommu_cache_init(struct iommu_table *tbl);
void iommu_dmacache_add(struct iommu_table *tbl, void *page, unsigned int npages, dma_addr_t addr,
			enum dma_data_direction direction);
dma_addr_t iommu_dmacache_use(struct iommu_table *tbl, void *page, unsigned int npages,
			      enum dma_data_direction direction);
void iommu_dmacache_free(struct iommu_table *tbl, dma_addr_t dma_handle, unsigned int npages);

#define IOMMU_MAP_LIST_MAX	8192
#define IOMMU_MAP_LIST_THRES	128

#endif /* __KERNEL__ */
#endif /* _IOMMU_CACHE_H */

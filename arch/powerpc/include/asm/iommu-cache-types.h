/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _IOMMU_CACHE_TYPES_H
#define _IOMMU_CACHE_TYPES_H
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

#else

struct iommu_pagecache {};

#endif /* CONFIG_IOMMU_PAGECACHE */
#endif /* __KERNEL__ */
#endif /* _IOMMU_CACHE_TYPES_H */

// SPDX-License-Identifier: GPL-2.0
/*
 * Lightweight gen_pool based allocator for memory that is placed at a specific
 * location in the TPU address space (such as a carveout memory)
 *
 * Copyright (C) 2020 Google, Inc.
 */

#include <linux/printk.h>

#ifdef CONFIG_X86
#include <asm/set_memory.h>
#endif

#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "edgetpu-iremap-pool.h"
#include "edgetpu-mmu.h"

struct edgetpu_mempool {
	struct gen_pool *gen_pool;
	void *base_vaddr;
	dma_addr_t base_dma_addr;
	tpu_addr_t base_tpu_addr;
	phys_addr_t base_phys_addr;
	size_t granule;
	struct mutex lock;
};

int edgetpu_iremap_pool_create(struct edgetpu_dev *etdev, void *base_vaddr,
			       dma_addr_t base_dma_addr,
			       tpu_addr_t base_tpu_addr,
			       phys_addr_t base_phys_addr, size_t size,
			       size_t granule)
{
	struct edgetpu_mempool *pool;

	if (etdev->iremap_pool) {
		etdev_err(etdev, "Refusing to replace existing iremap pool\n");
		return -EEXIST;
	}

	pool = kmalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return -ENOMEM;

	mutex_init(&pool->lock);

	pool->gen_pool = gen_pool_create(ilog2(granule), -1);
	if (!pool->gen_pool) {
		kfree(pool);
		etdev_err(etdev, "Failed to create iremap pool\n");
		return -ENOMEM;
	}
	pool->base_vaddr = base_vaddr;
	pool->base_dma_addr = base_dma_addr;
	pool->base_tpu_addr = base_tpu_addr;
	pool->base_phys_addr = base_phys_addr;
	pool->granule = granule;
	if (gen_pool_add(pool->gen_pool, (unsigned long)base_vaddr, size, -1)) {
		gen_pool_destroy(pool->gen_pool);
		kfree(pool);
		etdev_err(etdev, "Failed to add memory to iremap pool\n");
		return -ENOMEM;
	}
	etdev->iremap_pool = pool;
	return 0;
}

void edgetpu_iremap_pool_destroy(struct edgetpu_dev *etdev)
{
	struct edgetpu_mempool *etmempool = etdev->iremap_pool;

	if (!etmempool)
		return;
	gen_pool_destroy(etmempool->gen_pool);
	kfree(etmempool);
	etdev->iremap_pool = NULL;
}

static int edgetpu_alloc_coherent(struct edgetpu_dev *etdev, size_t size,
				  struct edgetpu_coherent_mem *mem,
				  enum edgetpu_context_id context_id)
{
	const u32 flags = EDGETPU_MMU_DIE | EDGETPU_MMU_32 | EDGETPU_MMU_HOST;

	mem->vaddr = dma_alloc_coherent(etdev->dev, size, &mem->dma_addr,
					GFP_KERNEL);
	if (!mem->vaddr)
		return -ENOMEM;
#ifdef CONFIG_X86
	set_memory_uc((unsigned long)mem->vaddr, size >> PAGE_SHIFT);
#endif
	mem->tpu_addr =
		edgetpu_mmu_tpu_map(etdev, mem->dma_addr, size,
				    DMA_BIDIRECTIONAL, context_id, flags);
	if (!mem->tpu_addr) {
#ifdef CONFIG_X86
		set_memory_wb((unsigned long)mem->vaddr, size >> PAGE_SHIFT);
#endif
		dma_free_coherent(etdev->dev, size, mem->vaddr, mem->dma_addr);
		mem->vaddr = NULL;
		return -EINVAL;
	}
	mem->size = size;
	return 0;
}

int edgetpu_iremap_alloc(struct edgetpu_dev *etdev, size_t size,
			 struct edgetpu_coherent_mem *mem,
			 enum edgetpu_context_id context_id)
{
	struct edgetpu_mempool *etmempool = etdev->iremap_pool;
	unsigned long addr;
	size_t offset;

	if (!etmempool)
		return edgetpu_alloc_coherent(etdev, size, mem, context_id);
	mutex_lock(&etmempool->lock);
	size = __ALIGN_KERNEL(size, etmempool->granule);
	addr = gen_pool_alloc(etmempool->gen_pool, size);
	if (!addr) {
		mutex_unlock(&etmempool->lock);
		return -ENOMEM;
	}
	mem->vaddr = (void *)addr;
	offset = mem->vaddr - etmempool->base_vaddr;
	mem->dma_addr = etmempool->base_dma_addr + offset;
	mem->tpu_addr = etmempool->base_tpu_addr + offset;
	mem->size = size;
	etdev_dbg(etdev, "iremap_alloc @ %llx IOVA = %llx size = %zu",
		   (u64)mem->vaddr, mem->dma_addr, size);
	mutex_unlock(&etmempool->lock);
	return 0;
}

static void edgetpu_free_coherent(struct edgetpu_dev *etdev,
				  struct edgetpu_coherent_mem *mem,
				  enum edgetpu_context_id context_id)
{
	edgetpu_mmu_tpu_unmap(etdev, mem->tpu_addr, mem->size, context_id);
#ifdef CONFIG_X86
	set_memory_wb((unsigned long)mem->vaddr, mem->size >> PAGE_SHIFT);
#endif
	dma_free_coherent(etdev->dev, mem->size, mem->vaddr, mem->dma_addr);
	mem->vaddr = NULL;
}

void edgetpu_iremap_free(struct edgetpu_dev *etdev,
			 struct edgetpu_coherent_mem *mem,
			 enum edgetpu_context_id context_id)
{
	struct edgetpu_mempool *etmempool = etdev->iremap_pool;

	if (!etmempool) {
		edgetpu_free_coherent(etdev, mem, context_id);
		return;
	}
	mutex_lock(&etmempool->lock);
	etdev_dbg(etdev, "iremap_free @ %llx IOVA = %llx size = %zu",
		  (u64)mem->vaddr, mem->dma_addr, mem->size);
	gen_pool_free(etmempool->gen_pool, (unsigned long)mem->vaddr,
		      mem->size);
	mem->vaddr = NULL;
	mutex_unlock(&etmempool->lock);
}

int edgetpu_iremap_mmap(struct edgetpu_dev *etdev, struct vm_area_struct *vma,
			struct edgetpu_coherent_mem *mem)
{
	struct edgetpu_mempool *etmempool = etdev->iremap_pool;
	size_t offset;
	phys_addr_t phys;

#ifdef CONFIG_ARM64
	/*
	 * ARM64 will crash on unaligned access to uncached mappings,
	 * which is the attribute set in edgetpu_mmap before this function is
	 * called.
	 * Mark the VMA's pages as writecombine to avoid this.
	 */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
#endif

	vma->vm_pgoff = 0;
	if (!etmempool)
		return dma_mmap_coherent(etdev->dev, vma, mem->vaddr,
					 mem->dma_addr, mem->size);
	offset = mem->vaddr - etmempool->base_vaddr;
	phys = etmempool->base_phys_addr + offset;
	etdev_dbg(etdev, "iremap_mmap: virt = %llx phys = %llx\n",
		  (u64)mem->vaddr, phys);
	return remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start, vma->vm_page_prot);
}
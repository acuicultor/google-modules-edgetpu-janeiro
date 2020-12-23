// SPDX-License-Identifier: GPL-2.0
/*
 * Edge TPU IOMMU interface.
 *
 * Copyright (C) 2019 Google, Inc.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/version.h>

#include "abrolhos-platform.h"
#include "edgetpu-internal.h"
#include "edgetpu-mapping.h"
#include "edgetpu-mmu.h"

struct edgetpu_iommu {
	struct iommu_group *iommu_group;
	/*
	 * IOMMU domains currently attached.
	 * NULL for a slot that doesn't have an attached domain.
	 */
	struct iommu_domain *domains[EDGETPU_NCONTEXTS];
	bool context_0_default;		/* is context 0 domain the default? */
	bool aux_enabled;
};

struct edgetpu_iommu_map_params {
	int prot;
	size_t size;
	struct iommu_domain *domain;
};

static struct iommu_domain *get_domain_by_pasid(struct edgetpu_dev *etdev,
						uint pasid)
{
	struct iommu_domain *domain = NULL;
	struct device *dev = etdev->dev;
	struct edgetpu_iommu *etiommu = etdev->mmu_cookie;

	if (pasid < EDGETPU_NCONTEXTS)
		domain = etiommu->domains[pasid];

	/* Fall back to default domain. */
	if (!domain)
		domain = iommu_get_domain_for_dev(dev);
	return domain;
}

/*
 * Kernel 5.3 introduced iommu_register_device_fault_handler
 */

#if KERNEL_VERSION(5, 3, 0) <=  LINUX_VERSION_CODE

static int edgetpu_iommu_dev_fault_handler(struct iommu_fault *fault,
					   void *token)
{
	struct edgetpu_dev *etdev = (struct edgetpu_dev *)token;

	if (fault->type == IOMMU_FAULT_DMA_UNRECOV) {
		etdev_err(etdev, "Unrecoverable IOMMU fault!\n");
		etdev_err(etdev, "Reason = %08X\n", fault->event.reason);
		etdev_err(etdev, "flags = %08X\n", fault->event.flags);
		etdev_err(etdev, "pasid = %08X\n", fault->event.pasid);
		etdev_err(etdev, "perms = %08X\n", fault->event.perm);
		etdev_err(etdev, "addr = %llX\n", fault->event.addr);
		etdev_err(etdev, "fetch_addr = %llX\n",
			  fault->event.fetch_addr);
	} else if (fault->type == IOMMU_FAULT_PAGE_REQ) {
		etdev_err(etdev, "IOMMU page request fault!\n");
		etdev_err(etdev, "flags = %08X\n", fault->prm.flags);
		etdev_err(etdev, "pasid = %08X\n", fault->prm.pasid);
		etdev_err(etdev, "grpid = %08X\n", fault->prm.grpid);
		etdev_err(etdev, "perms = %08X\n", fault->prm.perm);
		etdev_err(etdev, "addr = %llX\n", fault->prm.addr);
	}
	// Tell the IOMMU driver to carry on
	return -EAGAIN;
}

static int
edgetpu_register_iommu_device_fault_handler(struct edgetpu_dev *etdev)
{
	etdev_dbg(etdev, "Registering IOMMU device fault handler\n");
	return iommu_register_device_fault_handler(
		etdev->dev, edgetpu_iommu_dev_fault_handler, etdev);
}

static int
edgetpu_unregister_iommu_device_fault_handler(struct edgetpu_dev *etdev)
{
	etdev_dbg(etdev, "Unregistering IOMMU device fault handler\n");
	return iommu_unregister_device_fault_handler(etdev->dev);
}

#else /* kernel version before 5.3 */

static int
edgetpu_register_iommu_device_fault_handler(struct edgetpu_dev *etdev)
{
	return 0;
}

static int
edgetpu_unregister_iommu_device_fault_handler(struct edgetpu_dev *etdev)
{
	return 0;
}

#endif /* KERNEL_VERSION(5, 3, 0) <=  LINUX_VERSION_CODE */

static int edgetpu_iommu_fault_handler(struct iommu_domain *domain,
				       struct device *dev, unsigned long iova,
				       int flags, void *token)
{
	struct edgetpu_iommu_domain *etdomain =
		(struct edgetpu_iommu_domain *)token;

	dev_err(dev, "IOMMU fault on address %08lX. PASID = %u flags = %08X",
		iova, etdomain->pasid, flags);
	// Tell the IOMMU driver we are OK with this fault
	return 0;
}

static void edgetpu_init_etdomain(struct edgetpu_iommu_domain *etdomain,
				  struct iommu_domain *domain,
				  unsigned int pasid)
{
	etdomain->iommu_domain = domain;
	etdomain->pasid = pasid;
	iommu_set_fault_handler(domain, edgetpu_iommu_fault_handler, etdomain);
}

/*
 * Expect a default domain was already allocated for the group. If not try to
 * use the domain AUX feature to allocate one.
 */
static int check_default_domain(struct edgetpu_dev *etdev,
				struct edgetpu_iommu *etiommu)
{
	struct iommu_domain *domain;
	int ret;
	uint pasid;

	domain = iommu_get_domain_for_dev(etdev->dev);
	/* if default domain exists then we are done */
	if (domain) {
		etiommu->context_0_default = true;
		goto out;
	}
	etdev_warn(etdev, "device group has no default iommu domain\n");
	/* no default domain and no AUX - we can't have any domain */
	if (!etiommu->aux_enabled)
		return -EINVAL;

	domain = iommu_domain_alloc(etdev->dev->bus);
	if (!domain) {
		etdev_warn(etdev, "iommu domain alloc failed");
		return -EINVAL;
	}
	ret = iommu_aux_attach_device(domain, etdev->dev);
	if (ret) {
		etdev_warn(etdev, "Attach IOMMU aux failed: %d", ret);
		iommu_domain_free(domain);
		return ret;
	}
	pasid = iommu_aux_get_pasid(domain, etdev->dev);
	/* the default domain must have pasid = 0 */
	if (pasid != 0) {
		etdev_warn(etdev, "Invalid PASID %d returned from iommu\n",
			   pasid);
		iommu_aux_detach_device(domain, etdev->dev);
		iommu_domain_free(domain);
		return -EINVAL;
	}
out:
	etiommu->domains[0] = domain;
	return 0;
}

/* mmu_info is unused and NULL for IOMMU version, let IOMMU API supply info */
int edgetpu_mmu_attach(struct edgetpu_dev *etdev, void *mmu_info)
{
	struct edgetpu_platform_dev *edgetpu_pdev = to_abrolhos_dev(etdev);
	struct edgetpu_iommu *etiommu;
	int ret;

	etiommu = kzalloc(sizeof(*etiommu), GFP_KERNEL);
	if (!etiommu)
		return -ENOMEM;

	etdev->mmu_cookie = etiommu;
	etiommu->iommu_group = iommu_group_get(etdev->dev);
	if (etiommu->iommu_group) {
		iommu_group_set_name(etiommu->iommu_group, "edgetpu");
		dev_dbg(etdev->dev, "iommu group id %d setup\n",
			iommu_group_id(etiommu->iommu_group));
	} else {
		dev_warn(etdev->dev, "device has no iommu group\n");
	}

	iommu_dev_enable_feature(etdev->dev, IOMMU_DEV_FEAT_AUX);
	if (!iommu_dev_feature_enabled(etdev->dev, IOMMU_DEV_FEAT_AUX))
		etdev_warn(etdev, "AUX domains not supported\n");
	else
		etiommu->aux_enabled = true;
	ret = check_default_domain(etdev, etiommu);
	if (ret)
		return ret;

	ret = edgetpu_register_iommu_device_fault_handler(etdev);
	if (ret)
		etdev_warn(etdev, "Failed to register fault handler! (%d)\n",
			   ret);

	if (!edgetpu_pdev->csr_iova)
		return 0;

	etdev_dbg(etdev, "Mapping device CSRs: %llX -> %llX (%lu bytes)\n",
		  edgetpu_pdev->csr_iova, edgetpu_pdev->csr_paddr,
		  edgetpu_pdev->csr_size);

	/* Add an IOMMU translation for the CSR region */
	ret = edgetpu_mmu_add_translation(etdev, edgetpu_pdev->csr_iova,
					  edgetpu_pdev->csr_paddr,
					  edgetpu_pdev->csr_size,
					  IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV,
					  EDGETPU_CONTEXT_KCI);
	if (ret) {
		etdev_err(etdev, "Unable to map device CSRs into IOMMU\n");
		edgetpu_unregister_iommu_device_fault_handler(etdev);
		return ret;
	}

	return 0;
}

void edgetpu_mmu_reset(struct edgetpu_dev *etdev)
{
	/* If need to reset IOMMU driver can issue here. */
}

void edgetpu_mmu_detach(struct edgetpu_dev *etdev)
{
	struct edgetpu_platform_dev *edgetpu_pdev = to_abrolhos_dev(etdev);
	struct edgetpu_iommu *etiommu = etdev->mmu_cookie;
	int i, ret;

	if (!etiommu)
		return;

	if (edgetpu_pdev->csr_iova) {
		edgetpu_mmu_remove_translation(&edgetpu_pdev->edgetpu_dev,
					       edgetpu_pdev->csr_iova,
					       edgetpu_pdev->csr_size,
					       EDGETPU_CONTEXT_KCI);
	}
	edgetpu_pdev->csr_iova = 0;

	ret = edgetpu_unregister_iommu_device_fault_handler(etdev);
	if (ret)
		etdev_warn(etdev,
			   "Failed to unregister device fault handler (%d)\n",
			   ret);
	edgetpu_mmu_reset(etdev);

	for (i = etiommu->context_0_default ? 1 : 0; i < EDGETPU_NCONTEXTS;
	     i++) {
		if (etiommu->domains[i]) {
			iommu_aux_detach_device(etiommu->domains[i],
						etdev->dev);
			iommu_domain_free(etiommu->domains[i]);
		}
	}

	if (etiommu->iommu_group)
		iommu_group_put(etiommu->iommu_group);

	kfree(etiommu);
	etdev->mmu_cookie = NULL;
}

int edgetpu_mmu_reattach(struct edgetpu_dev *etdev)
{
	return 0;
}

/* Return context ID enumeration value as a Process Address Space ID. */
static uint context_id_to_pasid(enum edgetpu_context_id context_id)
{
	return (uint)context_id;
}

static int get_iommu_map_params(struct edgetpu_dev *etdev,
				struct edgetpu_mapping *map,
				enum edgetpu_context_id context_id,
				struct edgetpu_iommu_map_params *params)
{
	struct edgetpu_iommu *etiommu = etdev->mmu_cookie;
	size_t size = 0;
	uint pasid = context_id_to_pasid(context_id);
	int prot = __dma_dir_to_iommu_prot(map->dir);
	struct iommu_domain *domain;
	int i;
	struct scatterlist *sg;

	if (pasid >= EDGETPU_NCONTEXTS) {
		etdev_err(etdev, "Invalid context_id %d\n", context_id);
		return -EINVAL;
	}
	if (!etiommu)
		return -EINVAL;

	domain = get_domain_by_pasid(etdev, pasid);
	if (!domain) {
		etdev_err(etdev, "Unable to find an iommu domain\n");
		return -ENODEV;
	}

	for_each_sg(map->sgt.sgl, sg, map->sgt.orig_nents, i)
		size += sg->length;

	prot |= IOMMU_PBHA_PROT(EDGEPTU_MAP_PBHA_VALUE(map->flags));
	params->prot = prot;
	params->size = size;
	params->domain = domain;
	return 0;
}

int edgetpu_mmu_map(struct edgetpu_dev *etdev, struct edgetpu_mapping *map,
		    enum edgetpu_context_id context_id, u32 mmu_flags)
{
	int ret;
	unsigned long iova;
	struct edgetpu_iommu_map_params params;
	struct iommu_domain *default_domain =
		iommu_get_domain_for_dev(etdev->dev);

	ret = get_iommu_map_params(etdev, map, context_id, &params);

	if (ret)
		return ret;

	if (mmu_flags & EDGETPU_MMU_64)
		dev_warn_once(etdev->dev,
			      "%s: 64-bit addressing is not supported",
			      __func__);

	ret = dma_map_sg_attrs(etdev->dev, map->sgt.sgl, map->sgt.nents,
			       edgetpu_host_dma_dir(map->dir), map->dma_attrs);
	if (!ret)
		return -EINVAL;
	map->sgt.nents = ret;
	iova = sg_dma_address(map->sgt.sgl);

	/*
	 * All mappings get added to the default domain by the call to
	 * dma_map_sg above.
	 * Per-context mappings are mirrored to their specific domains here
	 */
	if (params.domain != default_domain) {
		if (!iommu_map_sg(params.domain, iova, map->sgt.sgl,
				  map->sgt.orig_nents, params.prot)) {
			/* Undo the mapping in the default domain */
			dma_unmap_sg_attrs(etdev->dev, map->sgt.sgl,
					   map->sgt.orig_nents,
					   edgetpu_host_dma_dir(map->dir),
					   DMA_ATTR_SKIP_CPU_SYNC);
			return -ENOMEM;
		}
	}

	map->device_address = iova;
	return 0;
}

void edgetpu_mmu_unmap(struct edgetpu_dev *etdev, struct edgetpu_mapping *map,
		       enum edgetpu_context_id context_id)
{
	int ret;
	struct edgetpu_iommu_map_params params;
	struct iommu_domain *default_domain =
		iommu_get_domain_for_dev(etdev->dev);

	ret = get_iommu_map_params(etdev, map, context_id, &params);
	if (!ret && params.domain != default_domain) {
		/*
		 * If this is a per-context mapping, it was mirrored in the
		 * per-context domain. Undo that mapping first.
		 */
		iommu_unmap(params.domain, map->device_address, params.size);
	}

	/*
	 * Always do dma_unmap since context_id might be invalid when group has
	 * mailbox detached.
	 */
	/* Undo the mapping in the default domain */
	dma_unmap_sg_attrs(etdev->dev, map->sgt.sgl, map->sgt.orig_nents,
			   edgetpu_host_dma_dir(map->dir), map->dma_attrs);
}

int edgetpu_mmu_map_iova_sgt(struct edgetpu_dev *etdev, tpu_addr_t iova,
			     struct sg_table *sgt, enum dma_data_direction dir,
			     enum edgetpu_context_id context_id)
{
	const int prot = __dma_dir_to_iommu_prot(edgetpu_host_dma_dir(dir));
	const tpu_addr_t orig_iova = iova;
	struct scatterlist *sg;
	int i;
	int ret;

	for_each_sg(sgt->sgl, sg, sgt->orig_nents, i) {
		ret = edgetpu_mmu_add_translation(etdev, iova, sg_phys(sg),
						  sg->length, prot, context_id);
		if (ret)
			goto error;
		iova += sg->length;
	}
	return 0;

error:
	edgetpu_mmu_remove_translation(etdev, orig_iova, iova - orig_iova,
				       context_id);
	return ret;
}

void edgetpu_mmu_unmap_iova_sgt_attrs(struct edgetpu_dev *etdev,
				      tpu_addr_t iova, struct sg_table *sgt,
				      enum dma_data_direction dir,
				      enum edgetpu_context_id context_id,
				      unsigned long attrs)
{
	size_t size = 0;
	struct scatterlist *sg;
	int i;

	for_each_sg(sgt->sgl, sg, sgt->orig_nents, i)
		size += sg->length;
	edgetpu_mmu_remove_translation(etdev, iova, size, context_id);
}

tpu_addr_t edgetpu_mmu_alloc(struct edgetpu_dev *etdev, size_t size,
			     u32 mmu_flags)
{
	return 0;
}

void edgetpu_mmu_reserve(struct edgetpu_dev *etdev, tpu_addr_t tpu_addr,
			 size_t size)
{
}

void edgetpu_mmu_free(struct edgetpu_dev *etdev, tpu_addr_t tpu_addr,
		      size_t size)
{
}

int edgetpu_mmu_add_translation(struct edgetpu_dev *etdev, unsigned long iova,
				phys_addr_t paddr, size_t size, int prot,
				enum edgetpu_context_id context_id)
{
	struct iommu_domain *domain;
	uint pasid = context_id_to_pasid(context_id);

	if (pasid >= EDGETPU_NCONTEXTS)
		return -EINVAL;

	domain = get_domain_by_pasid(etdev, pasid);
	if (!domain)
		return -ENODEV;
	return iommu_map(domain, iova, paddr, size, prot);
}

void edgetpu_mmu_remove_translation(struct edgetpu_dev *etdev,
				    unsigned long iova, size_t size,
				    enum edgetpu_context_id context_id)
{
	struct iommu_domain *domain;
	uint pasid = context_id_to_pasid(context_id);

	domain = get_domain_by_pasid(etdev, pasid);
	if (domain)
		iommu_unmap(domain, iova, size);
}

tpu_addr_t edgetpu_mmu_tpu_map(struct edgetpu_dev *etdev, dma_addr_t down_addr,
			       size_t size, enum dma_data_direction dir,
			       enum edgetpu_context_id context_id,
			       u32 mmu_flags)
{
	struct iommu_domain *domain;
	struct iommu_domain *default_domain =
		iommu_get_domain_for_dev(etdev->dev);
	phys_addr_t paddr;
	int prot = __dma_dir_to_iommu_prot(dir);
	uint pasid = context_id_to_pasid(context_id);

	if (pasid >= EDGETPU_NCONTEXTS)
		return 0;
	domain = get_domain_by_pasid(etdev, pasid);

	/*
	 * Either we don't have per-context domains or this mapping
	 * belongs to the default context, in which case we don't need
	 * to do anything
	 */
	if (!domain || domain == default_domain)
		return down_addr;
	paddr = iommu_iova_to_phys(default_domain, down_addr);
	if (!paddr)
		return 0;
	/* Map the address to the context-specific domain */
	if (iommu_map(domain, down_addr, paddr, size, prot))
		return 0;

	/* Return downstream IOMMU DMA address as TPU address. */
	return down_addr;
}

void edgetpu_mmu_tpu_unmap(struct edgetpu_dev *etdev, tpu_addr_t tpu_addr,
			   size_t size, enum edgetpu_context_id context_id)
{
	struct iommu_domain *domain;
	struct iommu_domain *default_domain =
		iommu_get_domain_for_dev(etdev->dev);
	uint pasid = context_id_to_pasid(context_id);

	domain = get_domain_by_pasid(etdev, pasid);

	/*
	 * Either we don't have per-context domains or this mapping
	 * belongs to the default context, in which case we don't need
	 * to do anything
	 */
	if (!domain || domain == default_domain)
		return;
	/* Unmap the address from the context-specific domain */
	iommu_unmap(domain, tpu_addr, size);
}

void edgetpu_mmu_use_dev_dram(struct edgetpu_dev *etdev, bool use_dev_dram)
{
}

/* to be returned when domain aux is not supported */
static struct edgetpu_iommu_domain invalid_etdomain = {
	.pasid = IOMMU_PASID_INVALID,
};

struct edgetpu_iommu_domain *edgetpu_mmu_alloc_domain(struct edgetpu_dev *etdev)
{
	struct edgetpu_iommu_domain *etdomain =
		kzalloc(sizeof(*etdomain), GFP_KERNEL);
	struct edgetpu_iommu *etiommu = etdev->mmu_cookie;
	struct iommu_domain *domain;

	if (!etdomain)
		return NULL;
	if (!etiommu->aux_enabled)
		return &invalid_etdomain;
	domain = iommu_domain_alloc(etdev->dev->bus);
	if (!domain) {
		etdev_warn(etdev, "iommu domain alloc failed");
		return NULL;
	}

	edgetpu_init_etdomain(etdomain, domain, IOMMU_PASID_INVALID);
	return etdomain;
}

void edgetpu_mmu_free_domain(struct edgetpu_dev *etdev,
			     struct edgetpu_iommu_domain *etdomain)
{
	if (!etdomain || etdomain == &invalid_etdomain)
		return;
	if (etdomain->pasid != IOMMU_PASID_INVALID) {
		etdev_warn(etdev, "Domain should be detached before free");
		edgetpu_mmu_detach_domain(etdev, etdomain);
	}
	iommu_domain_free(etdomain->iommu_domain);
	kfree(etdomain);
}

int edgetpu_mmu_attach_domain(struct edgetpu_dev *etdev,
			      struct edgetpu_iommu_domain *etdomain)
{
	struct edgetpu_iommu *etiommu = etdev->mmu_cookie;
	struct iommu_domain *domain;
	int ret;
	uint pasid;

	/* Changes nothing if domain AUX is not supported. */
	if (!etiommu->aux_enabled)
		return 0;
	if (etdomain->pasid != IOMMU_PASID_INVALID)
		return -EINVAL;
	domain = etdomain->iommu_domain;
	ret = iommu_aux_attach_device(domain, etdev->dev);
	if (ret) {
		etdev_warn(etdev, "Attach IOMMU aux failed: %d", ret);
		return ret;
	}
	pasid = iommu_aux_get_pasid(domain, etdev->dev);
	if (pasid <= 0 || pasid >= EDGETPU_NCONTEXTS) {
		etdev_warn(etdev, "Invalid PASID %d returned from iommu",
			   pasid);
		ret = -EINVAL;
		goto err_detach;
	}
	/* the IOMMU driver returned a duplicate PASID */
	if (etiommu->domains[pasid]) {
		ret = -EBUSY;
		goto err_detach;
	}
	etiommu->domains[pasid] = domain;
	etdomain->pasid = pasid;
	return 0;
err_detach:
	iommu_aux_detach_device(domain, etdev->dev);
	return ret;
}

void edgetpu_mmu_detach_domain(struct edgetpu_dev *etdev,
			       struct edgetpu_iommu_domain *etdomain)
{
	struct edgetpu_iommu *etiommu = etdev->mmu_cookie;
	uint pasid = etdomain->pasid;

	if (!etiommu->aux_enabled)
		return;
	if (pasid <= 0 || pasid >= EDGETPU_NCONTEXTS)
		return;
	etiommu->domains[pasid] = NULL;
	etdomain->pasid = IOMMU_PASID_INVALID;
	iommu_aux_detach_device(etdomain->iommu_domain, etdev->dev);
}

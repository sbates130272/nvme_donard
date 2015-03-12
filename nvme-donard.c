//////////////////////////////////////////////////////////////////////
//                             PMC-Sierra, Inc.
//
//
//
//                             Copyright 2013
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Author:  Logan Gunthorpe
//
//   Description:
//     NVME IOCTL Extensions for the donard project.
//
////////////////////////////////////////////////////////////////////////

#include "nvme.h"
#include "page_handle.h"

#include "nv-p2p.h"

#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>

static struct nvme_iod *map_gpu_pages(struct nvme_dev *dev,
				      struct nvidia_p2p_page_table *pages,
				      __u64 offset, unsigned length)
{
	int i, of;
	struct nvme_iod *iod;
	struct scatterlist *sg;
	uint32_t page_size;

	if (!length || length > INT_MAX - PAGE_SIZE)
		return ERR_PTR(-EINVAL);

	iod = nvme_alloc_iod(pages->entries, length, dev, GFP_KERNEL);
	if (!iod)
		return ERR_PTR(-ENOMEM);

	switch (pages->page_size) {
	case NVIDIA_P2P_PAGE_SIZE_4KB:
		page_size = 4 * 1024;
		break;
	case NVIDIA_P2P_PAGE_SIZE_64KB:
		page_size = 64 * 1024;
		break;
	case NVIDIA_P2P_PAGE_SIZE_128KB:
		page_size = 128 * 1024;
		break;
	default:
		nvme_free_iod(dev, iod);
		return ERR_PTR(-EIO);
	}

	sg = iod->sg;
	sg_init_table(sg, pages->entries);

	of = offset / page_size;
	offset -= of * page_size;

	for (i = 0; i < pages->entries - of; i++) {
		if (!length)
			break;

		sg[i].page_link = 0;
		sg[i].dma_address =
		    pages->pages[of + i]->physical_address + offset;
		sg[i].length = min_t(unsigned, length, page_size - offset);
		sg[i].dma_length = sg[i].length;
		sg[i].offset = 0;

		offset = 0;

		length -= sg[i].length;
	}

	if (length) {
		nvme_free_iod(dev, iod);
		return ERR_PTR(-EINVAL);
	}

	sg_mark_end(&sg[i]);
	iod->nents = i;

	return iod;
}

static int submit_gpu_io(struct nvme_ns *ns, struct nvme_gpu_io __user * uio)
{
	struct nvme_dev *dev = ns->dev;
	struct nvme_gpu_io io;
	struct nvme_command c;
	unsigned length;

	int status;
	struct nvme_iod *iod;
	struct page_handle *p;

	if (copy_from_user(&io, uio, sizeof(io)))
		return -EFAULT;
	length = (io.nblocks + 1) << ns->lba_shift;

	p = io.gpu_mem_handle;

	switch (io.opcode) {
	case nvme_cmd_write:
	case nvme_cmd_read:
	case nvme_cmd_compare:
		iod =
		    map_gpu_pages(dev, p->page_table, io.gpu_mem_offset,
				  length);
		break;
	default:
		return -EINVAL;
	}

	if (IS_ERR(iod))
		return PTR_ERR(iod);

	memset(&c, 0, sizeof(c));
	c.rw.opcode = io.opcode;
	c.rw.flags = io.flags;
	c.rw.nsid = cpu_to_le32(ns->ns_id);
	c.rw.slba = cpu_to_le64(io.slba);
	c.rw.length = cpu_to_le16(io.nblocks);
	c.rw.control = cpu_to_le16(io.control);
	c.rw.dsmgmt = cpu_to_le32(io.dsmgmt);
	c.rw.reftag = cpu_to_le32(io.reftag);
	c.rw.apptag = cpu_to_le16(io.apptag);
	c.rw.appmask = cpu_to_le16(io.appmask);

	length = nvme_setup_prps(dev, iod, length, GFP_KERNEL);
	c.rw.prp1 = cpu_to_le64(sg_dma_address(iod->sg));
	c.rw.prp2 = cpu_to_le64(iod->first_dma);

	if (length != (io.nblocks + 1) << ns->lba_shift)
		status = -ENOMEM;
	else
		status = nvme_submit_io_cmd(dev, ns, &c, NULL);

	nvme_free_iod(dev, iod);

	return status;
}

int nvme_donard_ioctl(struct nvme_ns *ns, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case NVME_IOCTL_SUBMIT_GPU_IO:
		return submit_gpu_io(ns, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

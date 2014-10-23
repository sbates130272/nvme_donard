//////////////////////////////////////////////////////////////////////
//                             PMC-Sierra, Inc.
//
//
//
//                             Copyright 2014
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
//     Manage Nvidia Pin Buf Memory
//
////////////////////////////////////////////////////////////////////////

#include "donard_nv_pinbuf.h"
#include "page_handle.h"

#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

static struct page_handle *mmap_page_handle = NULL;

static void free_callback(void *data)
{
    struct page_handle *p = data;

    nvidia_p2p_free_page_table(p->page_table);
}

static long pin_gpu_memory(struct donard_gpu_mem __user *upin)
{
    struct donard_gpu_mem pin;
    struct page_handle *p;
    int ret;

    if (copy_from_user(&pin, upin, sizeof(pin)))
        return -EFAULT;

    if ((p = kmalloc(sizeof(*p), GFP_KERNEL)) == NULL)
        return -ENOMEM;

    p->id = PAGE_HANDLE_ID;

    ret = nvidia_p2p_get_pages(pin.p2pToken, pin.vaSpaceToken, pin.address,
                               pin.size, &p->page_table, free_callback, p);
    if (ret) {
        printk("donard: failed pinning: %d\n", ret);
        kfree(p);
        return ret;
    }

    put_user(p, &upin->handle);

    return 0;
}

static long unpin_gpu_memory(struct donard_gpu_mem __user *upin)
{
    struct donard_gpu_mem pin;
    struct page_handle *p;
    int ret;

    mmap_page_handle = NULL;

    if (copy_from_user(&pin, upin, sizeof(pin)))
        return -EFAULT;

    if (pin.handle == 0)
        return -EFAULT;

    p = pin.handle;

    ret = nvidia_p2p_put_pages(pin.p2pToken, pin.vaSpaceToken, pin.address,
                               p->page_table);
    if (ret)
        return ret;

    kfree(p);

    return 0;
}

static long select_mmap_memory(struct page_handle __user *handle)
{
    if (handle->id != PAGE_HANDLE_ID) {
        mmap_page_handle = NULL;
        return -EFAULT;
    }

    mmap_page_handle = handle;
    return 0;
}

static long donard_pinbuf_ioctl(struct file *filp, unsigned int cmd,
                                unsigned long arg)
{
    switch (cmd) {
    case DONARD_IOCTL_PIN_GPU_MEMORY:
        return pin_gpu_memory((void __user *) arg);
    case DONARD_IOCTL_UNPIN_GPU_MEMORY:
        return unpin_gpu_memory((void __user *) arg);
    case DONARD_IOCTL_SELECT_MMAP_MEMORY:
        return select_mmap_memory((void __user *) arg);
    default:
        return -ENOTTY;
    }
}

static int donard_pinbuf_mmap(struct file *filp, struct vm_area_struct *vma)
{
    uint32_t page_size;
    unsigned long addr = vma->vm_start;
    struct nvidia_p2p_page_table *pages;
    int i;
    int ret;

    if (mmap_page_handle == NULL)
        return -EINVAL;

    pages = mmap_page_handle->page_table;

    switch(pages->page_size) {
    case NVIDIA_P2P_PAGE_SIZE_4KB:   page_size =   4*1024; break;
    case NVIDIA_P2P_PAGE_SIZE_64KB:  page_size =  64*1024; break;
    case NVIDIA_P2P_PAGE_SIZE_128KB: page_size = 128*1024; break;
    default:
        return -EIO;
    }


    for (i = 0; i < pages->entries; i++) {
        if (addr+page_size > vma->vm_end) break;

        if ((ret = remap_pfn_range(vma, addr,
                                   pages->pages[i]->physical_address >> PAGE_SHIFT,
                                   page_size, vma->vm_page_prot))) {

            printk("donard: remap %d failed: %d\n", i, ret);
            return -EAGAIN;
        }
        addr += page_size;
    }

    return 0;
}

static const struct file_operations donard_pinbuf_fops = {
    .owner                = THIS_MODULE,
    .unlocked_ioctl       = donard_pinbuf_ioctl,
    .mmap                 = donard_pinbuf_mmap,
};

static struct miscdevice donard_pinbuf_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "donard_pinbuf",
    .fops = &donard_pinbuf_fops,
};

static int __init donard_nv_pinbuf_init(void)
{
    int ret;

    if ((ret = misc_register(&donard_pinbuf_dev)))
        printk(KERN_ERR "Unable to register \"donard_pinbuf\" misc device\n");

    return ret;
}

static void __exit donard_nv_pinbuf_exit(void)
{
    misc_deregister(&donard_pinbuf_dev);
}

MODULE_AUTHOR("Logan Gunthorpe <logang@deltatee.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
module_init(donard_nv_pinbuf_init);
module_exit(donard_nv_pinbuf_exit);

// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual block swap device based on vnswap
 *
 * Copyright (C) 2020 Park Ju Hyung
 * Copyright (C) 2013 SungHwan Yun
 */

#define pr_fmt(fmt) "vbswap: " fmt

// #define DEBUG

#include <linux/module.h>
#include <linux/blkdev.h>

#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1 << SECTOR_SHIFT)
#define SECTORS_PER_PAGE_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE	(1 << SECTORS_PER_PAGE_SHIFT)
#define VBSWAP_LOGICAL_BLOCK_SHIFT 12
#define VBSWAP_LOGICAL_BLOCK_SIZE	(1 << VBSWAP_LOGICAL_BLOCK_SHIFT)
#define VBSWAP_SECTOR_PER_LOGICAL_BLOCK	(1 << \
	(VBSWAP_LOGICAL_BLOCK_SHIFT - SECTOR_SHIFT))

// vbswap is intentionally designed to expose 1 disk only

/* Globals */
static int vbswap_major;
static struct gendisk *vbswap_disk;
static const u64 vbswap_disksize = PAGE_ALIGN((u64)SZ_1G * 6);
static struct page *swap_header_page;

/*
 * Check if request is within bounds and aligned on vbswap logical blocks.
 */
static inline int vbswap_valid_io_request(struct bio *bio)
{
	if (unlikely(
		(bio->bi_iter.bi_sector >= (vbswap_disksize >> SECTOR_SHIFT)) ||
		(bio->bi_iter.bi_sector & (VBSWAP_SECTOR_PER_LOGICAL_BLOCK - 1)) ||
		(bio->bi_iter.bi_size & (VBSWAP_LOGICAL_BLOCK_SIZE - 1)))) {

		return 0;
	}

	/* I/O request is valid */
	return 1;
}

static int vbswap_bvec_read(struct bio_vec *bvec,
			    u32 index, struct bio *bio)
{
	struct page *page;
	unsigned char *user_mem, *swap_header_page_mem;

	if (unlikely(index != 0)) {
		pr_err("tried to read outside of swap header\n");
		// Return empty pages on valid requests to workaround toybox binary search
	}

	page = bvec->bv_page;

	user_mem = kmap_atomic(page);
	if (index == 0 && swap_header_page) {
		swap_header_page_mem = kmap_atomic(swap_header_page);
		memcpy(user_mem + bvec->bv_offset, swap_header_page_mem, bvec->bv_len);
		kunmap_atomic(swap_header_page_mem);

		// It'll be read one-time only
		__free_page(swap_header_page);
		swap_header_page = NULL;
	} else {
		// Do not allow memory dumps
		memset(user_mem + bvec->bv_offset, 0, bvec->bv_len);
	}
	kunmap_atomic(user_mem);
	flush_dcache_page(page);

	return 0;
}

static int vbswap_bvec_write(struct bio_vec *bvec,
			     u32 index, struct bio *bio)
{
	struct page *page;
	unsigned char *user_mem, *swap_header_page_mem;

	if (unlikely(index != 0)) {
		pr_err("tried to write outside of swap header\n");
		return -EIO;
	}

	page = bvec->bv_page;

	user_mem = kmap_atomic(page);
	if (swap_header_page == NULL)
		swap_header_page = alloc_page(GFP_KERNEL | GFP_NOIO);
	swap_header_page_mem = kmap_atomic(swap_header_page);
	memcpy(swap_header_page_mem, user_mem, PAGE_SIZE);
	kunmap_atomic(swap_header_page_mem);
	kunmap_atomic(user_mem);

	return 0;
}

static int vbswap_bvec_rw(struct bio_vec *bvec,
			  u32 index, struct bio *bio, int rw)
{
	if (rw == READ)
		return vbswap_bvec_read(bvec, index, bio);
	else
		return vbswap_bvec_write(bvec, index, bio);
}

static noinline void __vbswap_make_request(struct bio *bio, int rw)
{
	int offset, ret;
	u32 index;
	struct bio_vec bvec;
	struct bvec_iter iter;

	if (!vbswap_valid_io_request(bio)) {
		pr_err("%s %d: invalid io request. "
		       "(bio->bi_iter.bi_sector, bio->bi_iter.bi_size,"
		       "vbswap_disksize) = "
		       "(%llu, %d, %llu)\n",
		       __func__, __LINE__,
		       (unsigned long long)bio->bi_iter.bi_sector,
		       bio->bi_iter.bi_size, vbswap_disksize);

		bio_io_error(bio);
		return;
	}

	index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	offset = (bio->bi_iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
	    SECTOR_SHIFT;

	pr_debug("%s %d: (rw, index, offset, bi_size) = "
		 "(%d, %d, %d, %d)\n",
		 __func__, __LINE__, rw, index, offset, bio->bi_iter.bi_size);

	if (offset) {
		pr_err("%s %d: invalid offset. "
		       "(bio->bi_iter.bi_sector, index, offset) = (%llu, %d, %d)\n",
		       __func__, __LINE__,
		       (unsigned long long)bio->bi_iter.bi_sector,
		       index, offset);
		goto out_error;
	}

	if (bio->bi_iter.bi_size > PAGE_SIZE) {
		goto out_error;
	}

	if (bio->bi_vcnt > 1) {
		goto out_error;
	}

	bio_for_each_segment(bvec, bio, iter) {
		if (bvec.bv_len != PAGE_SIZE || bvec.bv_offset != 0) {
			pr_err("%s %d: bvec is misaligned. "
			       "(bv_len, bv_offset) = (%d, %d)\n",
			       __func__, __LINE__, bvec.bv_len, bvec.bv_offset);
			goto out_error;
		}

		pr_debug("%s %d: (rw, index, bvec.bv_len) = "
			 "(%d, %d, %d)\n",
			 __func__, __LINE__, rw, index, bvec.bv_len);

		ret = vbswap_bvec_rw(&bvec, index, bio, rw);
		if (ret < 0) {
			if (ret != -ENOSPC)
				pr_err("%s %d: vbswap_bvec_rw failed."
				       "(ret) = (%d)\n",
				       __func__, __LINE__, ret);
			else
				pr_debug("%s %d: vbswap_bvec_rw failed. "
					 "(ret) = (%d)\n",
					 __func__, __LINE__, ret);
			goto out_error;
		}

		index++;
	}

	bio->bi_status = BLK_STS_OK;
	bio_endio(bio);

	return;

out_error:
	bio_io_error(bio);
}

/*
 * Handler function for all vbswap I/O requests.
 */
static blk_qc_t vbswap_make_request(struct request_queue *queue,
				    struct bio *bio)
{
	// Deliberately error out on kernel swap
	if (likely(current->flags & PF_KTHREAD))
		bio_io_error(bio);
	else
		__vbswap_make_request(bio, bio_data_dir(bio));

	return BLK_QC_T_NONE;
}

static const struct block_device_operations vbswap_fops = {
	.owner = THIS_MODULE
};

static ssize_t disksize_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%llu\n", vbswap_disksize);
}

static ssize_t disksize_store(struct device *dev,
			      struct device_attribute *attr, const char *buf,
			      size_t len)
{
	return len;
}

static ssize_t max_comp_streams_show(struct device *dev,
                                     struct device_attribute *attr, char *buf)
{
        return scnprintf(buf, PAGE_SIZE, "%d\n", num_online_cpus());
}

static ssize_t max_comp_streams_store(struct device *dev,
                                      struct device_attribute *attr, const char *buf,
                                      size_t len)
{
        return len;
}

static DEVICE_ATTR_RW(disksize);
static DEVICE_ATTR_RW(max_comp_streams);

static struct attribute *vbswap_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_max_comp_streams.attr,
	NULL,
};

ATTRIBUTE_GROUPS(vbswap_disk);

static int create_device(void)
{
	/* gendisk structure */
	vbswap_disk = alloc_disk(1);
	if (!vbswap_disk) {
		pr_err("%s %d: Error allocating disk structure for device\n",
		       __func__, __LINE__);
		return -ENOMEM;
	}

	vbswap_disk->queue = blk_alloc_queue(GFP_KERNEL);
	if (!vbswap_disk->queue) {
		pr_err("%s %d: Error allocating disk queue for device\n",
		       __func__, __LINE__);
		put_disk(vbswap_disk);
		return -ENOMEM;
	}

	blk_queue_make_request(vbswap_disk->queue, vbswap_make_request);

	vbswap_disk->major = vbswap_major;
	vbswap_disk->first_minor = 0;
	vbswap_disk->fops = &vbswap_fops;
	vbswap_disk->private_data = NULL;
	snprintf(vbswap_disk->disk_name, 16, "zram%d", 0);

	set_capacity(vbswap_disk, vbswap_disksize >> SECTOR_SHIFT);
	pr_info("created device with size %llu\n", vbswap_disksize);

	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(vbswap_disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(vbswap_disk->queue,
				     VBSWAP_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(vbswap_disk->queue, PAGE_SIZE);
	blk_queue_io_opt(vbswap_disk->queue, PAGE_SIZE);
	blk_queue_max_hw_sectors(vbswap_disk->queue, PAGE_SIZE / SECTOR_SIZE);

	disk_to_dev(vbswap_disk)->groups = vbswap_disk_groups;
	add_disk(vbswap_disk);

	/* vbswap devices sort of resembles non-rotational disks */
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, vbswap_disk->queue);
	queue_flag_clear_unlocked(QUEUE_FLAG_ADD_RANDOM, vbswap_disk->queue);

	return 0;
}

static void destroy_device(void)
{
	if (vbswap_disk) {
		del_gendisk(vbswap_disk);
		put_disk(vbswap_disk);
	}

	if (vbswap_disk->queue)
		blk_cleanup_queue(vbswap_disk->queue);
}

static int __init vbswap_init(void)
{
	int ret;

	vbswap_major = register_blkdev(0, "zram");
	if (vbswap_major <= 0) {
		pr_err("%s %d: Unable to get major number\n",
		       __func__, __LINE__);
		ret = -EBUSY;
		goto out;
	}

	ret = create_device();
	if (ret) {
		pr_err("%s %d: Unable to create vbswap_device\n",
		       __func__, __LINE__);
		goto free_devices;
	}

	return 0;

free_devices:
	unregister_blkdev(vbswap_major, "zram");
out:
	return ret;
}

static void __exit vbswap_exit(void)
{
	destroy_device();

	unregister_blkdev(vbswap_major, "vbswap");

	if (swap_header_page)
		__free_page(swap_header_page);
}

module_init(vbswap_init);
module_exit(vbswap_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Park Ju Hyung <qkrwngud825@gmail.com>");
MODULE_DESCRIPTION("Virtual block swap device based on vnswap");

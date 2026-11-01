/*
 * gx: LKL's tools/lkl/lib/virtio_blk.c, with requests done on threads of
 * their own (see virtio-defer.c, appended to virtio.c): the kernel's driver
 * hands a request over and goes on, and the disk's thread raises the
 * interrupt when the data is there, as a disk does. build-nx.sh puts this file
 * in place of LKL's.
 */
#include <lkl_host.h>
#include "virtio.h"
#include "endian.h"

struct virtio_req *virtio_req_defer(struct virtio_req *req);
void virtio_req_complete_deferred(struct virtio_req *req, uint32_t len);

struct virtio_blk_dev {
	struct virtio_dev dev;
	struct lkl_virtio_blk_config config;
	struct lkl_dev_blk_ops *ops;
	struct lkl_disk disk;
};

struct virtio_blk_req_trailer {
	uint8_t status;
};

static int blk_check_features(struct virtio_dev *dev)
{
	if (dev->driver_features == dev->device_features)
		return 0;

	return -LKL_EINVAL;
}

/* Does the request; its status goes in its last buffer. */
static void blk_do(struct virtio_dev *dev, struct virtio_req *req)
{
	struct virtio_blk_dev *blk_dev;
	struct lkl_virtio_blk_outhdr *h;
	struct virtio_blk_req_trailer *t;
	struct lkl_blk_req lkl_req;

	if (req->buf_count < 3) {
		lkl_printf("virtio_blk: no status buf\n");
		return;
	}

	h = req->buf[0].iov_base;
	t = req->buf[req->buf_count - 1].iov_base;
	blk_dev = container_of(dev, struct virtio_blk_dev, dev);

	t->status = LKL_DEV_BLK_STATUS_IOERR;

	if (req->buf[0].iov_len != sizeof(*h)) {
		lkl_printf("virtio_blk: bad header buf\n");
		return;
	}

	if (req->buf[req->buf_count - 1].iov_len != sizeof(*t)) {
		lkl_printf("virtio_blk: bad status buf\n");
		return;
	}

	lkl_req.type = le32toh(h->type);
	lkl_req.prio = le32toh(h->ioprio);
	lkl_req.sector = le64toh(h->sector);
	lkl_req.buf = &req->buf[1];
	lkl_req.count = req->buf_count - 2;

	t->status = blk_dev->ops->request(blk_dev->disk, &lkl_req);
}

/* The requests waiting for a thread, first in first out, and the threads. */
#define BLK_THREADS 2

struct blk_job {
	struct virtio_dev *dev;
	struct virtio_req *req;
	struct blk_job *next;
};

static struct lkl_mutex *jobs_lock;
static struct lkl_sem *jobs_waiting;
static struct blk_job *jobs, **jobs_tail = &jobs;
static int blk_async;   /* the threads are there */
/* Set by the host before the first disk: done in enqueue(), as LKL does. */
int lkl_blk_sync;

static void blk_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct blk_job *job;

		lkl_host_ops.sem_down(jobs_waiting);
		lkl_host_ops.mutex_lock(jobs_lock);
		job = jobs;
		jobs = job->next;
		if (!jobs)
			jobs_tail = &jobs;
		lkl_host_ops.mutex_unlock(jobs_lock);

		blk_do(job->dev, job->req);
		virtio_req_complete_deferred(job->req, 0);
		lkl_host_ops.mem_free(job);
	}
}

/* Once, before the kernel uses a disk (lkl_disk_add()). Without the threads,
 * requests are done in enqueue() as LKL does. */
static int blk_threads_up(void)
{
	static int up;

	if (up)
		return up > 0;
	up = -1;
	if (lkl_blk_sync)
		return 0;
	jobs_lock = lkl_host_ops.mutex_alloc(0);
	jobs_waiting = lkl_host_ops.sem_alloc(0);
	if (!jobs_lock || !jobs_waiting)
		return 0;
	for (int i = 0; i < BLK_THREADS; i++)
		if (lkl_host_ops.thread_create(blk_thread, NULL))
			up = 1;
	blk_async = up > 0;
	return blk_async;
}

static int blk_enqueue(struct virtio_dev *dev, int q, struct virtio_req *req)
{
	struct virtio_req *later = blk_async ? virtio_req_defer(req) : NULL;
	struct blk_job *job = later ? lkl_host_ops.mem_alloc(sizeof(*job)) : NULL;

	if (!job) {
		if (later) {
			/* Off the ring already: done here, completed as
			 * deferred. */
			blk_do(dev, later);
			virtio_req_complete_deferred(later, 0);
			return 0;
		}
		blk_do(dev, req);
		virtio_req_complete(req, 0);
		return 0;
	}
	job->dev = dev;
	job->req = later;
	job->next = NULL;
	lkl_host_ops.mutex_lock(jobs_lock);
	*jobs_tail = job;
	jobs_tail = &job->next;
	lkl_host_ops.mutex_unlock(jobs_lock);
	lkl_host_ops.sem_up(jobs_waiting);
	return 0;
}

static struct virtio_dev_ops blk_ops = {
	.check_features = blk_check_features,
	.enqueue = blk_enqueue,
};


int lkl_disk_add(struct lkl_disk *disk)
{
	struct virtio_blk_dev *dev;
	unsigned long long capacity;
	int ret;

	blk_threads_up();

	dev = lkl_host_ops.mem_alloc(sizeof(*dev));
	if (!dev)
		return -LKL_ENOMEM;

	disk->dev = dev;

	dev->dev.device_id = LKL_VIRTIO_ID_BLOCK;
	dev->dev.vendor_id = 0;
	dev->dev.device_features = 0;
	dev->dev.config_gen = 0;
	dev->dev.config_data = &dev->config;
	dev->dev.config_len = sizeof(dev->config);
	dev->dev.ops = &blk_ops;
	if (disk->ops)
		dev->ops = disk->ops;
	else
		dev->ops = &lkl_dev_blk_ops;
	dev->disk = *disk;

	ret = dev->ops->get_capacity(*disk, &capacity);
	if (ret) {
		ret = -LKL_ENOMEM;
		goto out_free;
	}
	dev->config.capacity = htole64(capacity / 512);

	ret = virtio_dev_setup(&dev->dev, 1, 32);
	if (ret)
		goto out_free;

	return dev->dev.virtio_mmio_id;

out_free:
	lkl_host_ops.mem_free(dev);

	return ret;
}

int lkl_disk_remove(struct lkl_disk disk)
{
	struct virtio_blk_dev *dev;
	int ret;

	dev = (struct virtio_blk_dev *)disk.dev;
	if (!dev)
		return -LKL_EINVAL;

	ret = virtio_dev_cleanup(&dev->dev);
	if (ret < 0)
		return ret;

	lkl_host_ops.mem_free(dev);

	return 0;
}

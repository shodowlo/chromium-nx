
/*
 * gx: appended to LKL's tools/lkl/lib/virtio.c by build-nx.sh, for the block
 * device of virtio_blk.c here, which completes its requests on threads of
 * its own.
 *
 * LKL's devices complete a request inside enqueue(), on the thread that ran
 * the kernel's driver, which holds the kernel's one processor all along: a
 * read from the SD card of a couple of megabytes held every other thread's
 * syscalls for tens of milliseconds (an open() took 12 ms on average while a
 * browser started). A deferred request is taken off the ring at once; it is
 * put in the used ring, and the interrupt raised, whenever it is done, in any
 * order, which the block layer is made for.
 */

struct _virtio_deferred {
	struct _virtio_req r;
	uint16_t desc_id;   /* the head of its chain: the avail slot is reused */
};

static struct lkl_mutex *deferred_lock;

/* Takes the request off the ring, in enqueue(): what is returned is to be
 * given to virtio_req_complete_deferred() later, from any thread. One chain
 * of descriptors per request (not the merged buffers of networking). */
struct virtio_req *virtio_req_defer(struct virtio_req *req)
{
	struct _virtio_req *_req = container_of(req, struct _virtio_req, req);
	struct virtio_queue *q = _req->q;
	struct _virtio_deferred *d;

	if (q->max_merge_len)
		return NULL;
	if (!deferred_lock) {
		/* Under the kernel's processor: no other enqueue() runs. */
		deferred_lock = lkl_host_ops.mutex_alloc(0);
		if (!deferred_lock)
			return NULL;
	}
	d = lkl_host_ops.mem_alloc(sizeof(*d));
	if (!d)
		return NULL;
	d->r = *_req;
	d->desc_id = q->avail->ring[_req->idx & (q->num - 1)];
	q->last_avail_idx = _req->idx + 1;
	return &d->r.req;
}

void virtio_req_complete_deferred(struct virtio_req *req, uint32_t len)
{
	struct _virtio_deferred *d = container_of(container_of(req, struct _virtio_req, req),
						  struct _virtio_deferred, r);
	struct virtio_queue *q = d->r.q;
	struct virtio_dev *dev = d->r.dev;
	uint16_t used_idx;

	lkl_host_ops.mutex_lock(deferred_lock);
	used_idx = virtio_get_used_idx(q);
	q->used->ring[used_idx & (q->num - 1)].id = d->desc_id;
	q->used->ring[used_idx & (q->num - 1)].len = htole16(len);
	virtio_sync_used_idx(q, used_idx + 1);
	q->last_used_idx_signaled = used_idx + 1;
	lkl_host_ops.mutex_unlock(deferred_lock);
	/* Always: the driver may be waiting for this one alone. The status
	 * bit atomically: the driver acknowledges on another thread. A driver
	 * that acknowledged between our used index and this bit has already
	 * seen the index, which was written first. */
	__atomic_fetch_or(&dev->int_status, VIRTIO_MMIO_INT_VRING, __ATOMIC_SEQ_CST);
	lkl_trigger_irq(dev->irq);
	lkl_host_ops.mem_free(d);
}

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <linux/farfetch.h>

extern long (*farfetch_ptr)(unsigned int cmd, void __user *addr,
			    pid_t target_pid, unsigned long target_addr,
			    size_t len);
extern long farfetch_default(unsigned int cmd, void __user *addr,
			     pid_t target_pid, unsigned long target_addr,
			     size_t len);

long farfetch(unsigned int cmd, void __user *addr, pid_t target_pid,
	      unsigned long target_addr, size_t len)
{
	struct pid *pid_struct;
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct page **pages;
	void __user *iter;
	int locked;
	long ret;
	int i;
	unsigned long nr_pages;
	size_t page_off = target_addr & ~PAGE_MASK;

	/*
	 * handle overflow cases to be hyper-correct:
	 * - MAX_RW_COUNT check ensures we don't overflow into errno values
	 * - SIZE_MAX check ensures we don't overflow in nr_pages calculation
	 */
	len = min3(len, MAX_RW_COUNT, SIZE_MAX - page_off - PAGE_SIZE + 1);
	nr_pages = (page_off + len + PAGE_SIZE - 1) / PAGE_SIZE;
	WARN_ON(nr_pages * PAGE_SIZE < len);

	if (from_kuid_munged(current_user_ns(), task_euid(current)))
		return -EPERM;

	pid_struct = find_get_pid(target_pid);
	tsk = get_pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	if (!tsk)
		return -ESRCH;

	mm = get_task_mm(tsk);
	put_task_struct(tsk);
	if (!mm)
		return -ESRCH;

	pages = kmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (pages == NULL) {
		mmput(mm);
		return -ENOMEM;
	}

	/*
	 * dealing with 'locked' allows get_user_pages_remote() to retry a fault
	 * if need be, but this isn't strictly necessary for our use cases
	 * (passing NULL would be fine)
	 */
	locked = 1;
	if (mmap_read_lock_killable(mm)) {
		mmput(mm);
		kfree(pages);
		return -EINTR;
	}
	ret = get_user_pages_remote(mm, target_addr, nr_pages,
				    (cmd == FAR_WRITE ? FOLL_WRITE : 0) | FOLL_FORCE,
				    pages, NULL, &locked);
	if (locked)
		mmap_read_unlock(mm);
	mmput(mm);
	if (IS_ERR_VALUE(ret)) {
		kfree(pages);
		return ret;
	}

	if (ret < nr_pages) {
		nr_pages = ret;
		WARN_ON((nr_pages * PAGE_SIZE) - page_off > len);
		len = (nr_pages * PAGE_SIZE) - page_off;
	}

	ret = 0;
	iter = addr;
	for (i = 0; i < nr_pages; ++i) {
		size_t to_copy = min(len - (iter - addr), PAGE_SIZE - page_off);

		switch (cmd) {
		/*
		 * page_address()/page_to_virt() work as well as kmap() for our
		 * real-world use cases (using 64-bit systems), but we use the
		 * latter to be hyper-correct and account for highmem
		 */
		case FAR_READ:
			if (copy_to_user(iter, kmap(pages[i]) + page_off, to_copy))
				ret = -EFAULT;
			kunmap(pages[i]);
			break;
		case FAR_WRITE:
			if (copy_from_user(kmap(pages[i]) + page_off, iter, to_copy))
				ret = -EFAULT;
			else
				set_page_dirty_lock(pages[i]);
			kunmap(pages[i]);
			break;
		default:
			ret = -EINVAL;
		}
		if (ret)
			break;

		page_off = 0;
		iter += to_copy;
	}
	WARN_ON(!ret && iter - addr != len);

	for (i = 0; i < nr_pages; ++i)
		put_page(pages[i]);
	kfree(pages);

	return ret ? ret : len;
}

int farfetch_init(void)
{
	pr_info("Installing farfetch\n");
	farfetch_ptr = farfetch;
	return 0;
}

void farfetch_exit(void)
{
	pr_info("Removing farfetch\n");
	farfetch_ptr = farfetch_default;
}

module_init(farfetch_init);
module_exit(farfetch_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("farfetch: for fetching pages from afar");
MODULE_AUTHOR("Kent Hall");

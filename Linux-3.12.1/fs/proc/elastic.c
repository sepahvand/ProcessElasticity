#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>

#include <linux/rmap.h>
#include "internal.h"
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mm_inline.h>
#include <linux/hugetlb.h>
#include <linux/huge_mm.h>
#include <asm/tlbflush.h>

static int reclaim_pte_range(pmd_t *pmd, unsigned long addr,
				unsigned long end, struct mm_walk *walk)
{		

	struct vm_area_struct *vma = walk->private;
	pte_t *pte, ptent;
	spinlock_t *ptl;
	struct page *page;
	LIST_HEAD(page_list);
	int isolated;
	split_huge_page_pmd(vma, addr, pmd);
	if (pmd_trans_unstable(pmd))
		return 0;
cont:
	isolated = 0;
	pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	for (; addr != end; pte++, addr += PAGE_SIZE) {
		ptent = *pte;
		if (!pte_present(ptent))
			continue;

		page = vm_normal_page(vma, addr, ptent);
		if (!page)
			continue;

		if (isolate_lru_page(page))
			continue;

		list_add(&page->lru, &page_list);
		inc_zone_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
		isolated++;
		if (isolated >= SWAP_CLUSTER_MAX)
			break;
	}
	pte_unmap_unlock(pte - 1, ptl);
	reclaim_pages_from_list(&page_list);
	if (addr != end)
		goto cont;

	cond_resched();
	return 0;
}

static ssize_t reclaim_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct task_struct *task;
	char buffer[PROC_NUMBUF];
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int type;
	int rv;	
	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	rv = kstrtoint(strstrip(buffer), 10, &type);
	if (rv < 0)
		return rv;
	task = get_pid_task(find_vpid(type), PIDTYPE_PID);
	
	if (!task)
		return -ESRCH;
	mm = get_task_mm(task);
	if (mm) {
		struct mm_walk reclaim_walk = {
			.pmd_entry = reclaim_pte_range,
			.mm = mm,
		};
		down_read(&mm->mmap_sem);
		for (vma = mm->mmap; vma; vma = vma->vm_next) {
			reclaim_walk.private = vma;
			if (is_vm_hugetlb_page(vma))
				continue;			
			walk_page_range(vma->vm_start, vma->vm_end,
					&reclaim_walk);
		}
		flush_tlb_mm(mm);
		up_read(&mm->mmap_sem);
		mmput(mm);
	}
	put_task_struct(task);

	return count;
}

const struct file_operations proc_elastic_operations = {
	.write		= reclaim_write,
	.llseek		= noop_llseek,
};

static int __init proc_elastic_init(void)
{
	proc_create("elastic", 0, NULL, &proc_elastic_operations);	
	return 0;
}
module_init(proc_elastic_init);

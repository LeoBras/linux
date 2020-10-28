// SPDX-License-Identifier: GPL-2.0
#include <linux/cpufreq.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <linux/smp.h>
#include <linux/atomic.h>

static int first = 0;
#warning "DEBUGGING smp_call_function v2"
__weak void arch_freq_prepare_all(void)
{
}

static void justsum(void *buf)
{
	atomic_t *a = buf;
	atomic_add(smp_processor_id() + 1, a);
	if (first++ == 0)
		pr_err("leobras: debugging smp_call_function\n");

}

extern const struct seq_operations cpuinfo_op;
static int cpuinfo_open(struct inode *inode, struct file *file)
{
	atomic_t a;
	int b;

	smp_call_function(justsum, &a, true);
	b = atomic_read(&a) + smp_processor_id() + 1;
	if (b != (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8))
		pr_err("leobras: debugging smp_call_funct only ran in %d cpus\n", b);

	arch_freq_prepare_all();
	return seq_open(file, &cpuinfo_op);
}

static const struct proc_ops cpuinfo_proc_ops = {
	.proc_flags	= PROC_ENTRY_PERMANENT,
	.proc_open	= cpuinfo_open,
	.proc_read_iter	= seq_read_iter,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};

static int __init proc_cpuinfo_init(void)
{
	proc_create("cpuinfo", 0, NULL, &cpuinfo_proc_ops);
	return 0;
}
fs_initcall(proc_cpuinfo_init);

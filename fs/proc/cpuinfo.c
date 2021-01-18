// SPDX-License-Identifier: GPL-2.0
#include <linux/cpufreq.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <linux/smp.h>
#include <linux/atomic.h>

static int first = 0;
#warning "DEBUGGING smp_call_function v4"
__weak void arch_freq_prepare_all(void)
{
}

unsigned long long sc[4] = {0};

static void justsum(void *buf)
{
	int *a = buf, cpu;

	cpu = smp_processor_id();
	a[cpu] = cpu;
	sc[cpu]++;

	if (first++ == 0)
		pr_err("leobras: debugging smp_call_function\n");

}

/*
static void smptest(void)
{
	int c;
	int i, a[8] = {-1};

	c = smp_processor_id();
	a[c] = c + 100;

	smp_call_function(justsum, &a, true);

	for (i = 0; i < 4; i++) {
		if (a[i] == i )
			continue;
		if (i == c)
			continue;
		pr_err("leobras: debugging smp_call_funct: %d found in cpu %d (0-7)\n", a[i], i);
	}
}*/

static void smptest2(int cpu)
{
	int a[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	smp_call_function_single(cpu, justsum, &a, true);
	if (a[cpu] != cpu)
		pr_err("leobras: debugging smp_call_funct: %d found in cpu %d (0-7)\n", a[cpu], cpu);

}

extern const struct seq_operations cpuinfo_op;
static int cpuinfo_open(struct inode *inode, struct file *file)
{
//	int i = 0;
	int i = smp_processor_id();
	volatile unsigned long long j;

	if (i == 0)
		goto out;

//	int i = (smp_processor_id() + 1) % 4;
//	for (i = 0; i < 10; i++)
	for (j = 0; j < 200 ; ){
		j++;
		smptest2(0);
	}
out:
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

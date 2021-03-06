#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>

#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/cred.h>
#include <linux/dirent.h>
#include <linux/syscalls.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <asm/uaccess.h>
#include <asm/processor.h>
#include "mmuhack.h"
#include "dumpcode.h"

//Custom defs
/*
#define __NR_open 5
#define __NR_access 33
#define __NR_stat64 195
#define __NR_lstat64 196
#define __NR_fstat64 197
*/

#ifndef __NR_syscalls
//TODO: find way to get rid of this.
#define __NR_syscalls 500
#endif

#ifndef NR_syscalls
#define NR_syscalls __NR_syscalls
#endif

unsigned long **sys_call_table;
unsigned long flags;

int hide_uid[100];
unsigned int hide_uid_count=0;
module_param_array(hide_uid, int, &hide_uid_count, 0644);

char *hide_file[100] = {"bin/su", "bin/busybox", "app/Superuser.apk", "bin/proc", "bin/librank", };
unsigned int hide_file_cnt=1;
module_param_array(hide_file, charp, &hide_file_cnt, 0644);

int check_hide_uid(void)
{
	int i;
	for (i = 0; i < hide_uid_count; i++)
	{
		if (hide_uid[i] == current_uid())
			return 1;
	}
	return 0;
}

int check_hide_file(const char *filename)
{
	int i;
	for (i=0; i < hide_file_cnt; i++)
	{
		if (strstr(filename, hide_file[i]))
		{
			return 1;
		}
	}

	return 0;
}

asmlinkage int my_do_execve(char __user * filename,
			    const char __user * const __user * argv,
			    const char __user * const __user * envp, struct pt_regs *regs);

static struct jprobe my_jprobe = {
	.entry = (kprobe_opcode_t *) my_do_execve
};

static unsigned long **find_sys_call_table(void)
{
	unsigned long int offset = PAGE_OFFSET;
	unsigned long **sct;

	while (offset < ULLONG_MAX)
	{
		sct = (unsigned long **)offset;

		if (sct[__NR_close] == (unsigned long *)sys_close)
			return sct;

		offset += sizeof(void *);
	}

	return NULL;
}

long (*orig_sys_getdents64) (unsigned int fd,
			     struct linux_dirent64 __user * dirent, unsigned int count);

asmlinkage long my_sys_getdents64(unsigned int fd,
				  struct linux_dirent64 __user * dirent, unsigned int count)
{
	int fake = 0;
	int offset, copyret;
	struct linux_dirent64 *td1, *td2, *cur, *prev;
	unsigned long ret, tmp;
	char *ptr;
	char *tmp_path;
	char *pathname;
	struct file *file;
	struct path path;
	char fullpath[512] = {0, };

	ret = orig_sys_getdents64(fd, dirent, count);

	fake = check_hide_uid();

	if (fake == 0)
		return ret;

	if (ret <= 0)
		return ret;

	if ((td2 = kmalloc(ret + 1, GFP_KERNEL)) == NULL)
	{
		printk("KMALLOC FAILED!");
		return ret;
	}

	spin_lock(&current->files->file_lock);
	file = fcheck(fd);

	if (!file) {
		printk("It is weird... How can you reach here?\n");
		spin_unlock(&current->files->file_lock);
		return ret;
	}

	path = file->f_path;
	path_get(&file->f_path);
	spin_unlock(&current->files->file_lock);

	tmp_path = (char *)__get_free_page(GFP_TEMPORARY);

	if (!tmp_path) {
		path_put(&path);
		return -ENOMEM;
	}

	pathname = d_path(&path, tmp_path, PAGE_SIZE);
	path_put(&path);

	if (IS_ERR(pathname)) {
		free_page((unsigned long)tmp_path);
		printk("Errnous path. \n");
		return ret;
	}

	copyret = copy_from_user(td2, dirent, ret);

	td1 = td2;
	ptr = (char *)td2;
	tmp = ret;
	prev = NULL;

	while (ptr < (char *)td1 + tmp)
	{
		cur = (struct linux_dirent64 *)ptr;
		offset = cur->d_reclen;

		fullpath[0] = 0;

		strcat(fullpath, pathname);
		strcat(fullpath, "/");
		strcat(fullpath, cur->d_name);

		if (check_hide_file(fullpath))
		{
			if (!prev)
			{
				ret -= offset;
				td2 = (struct linux_dirent64 *)((char *)td1 + offset);
			}
			else
			{
				prev->d_reclen += offset;
				memset(cur, 0, offset);
			}
		}
		else
			prev = cur;

		ptr += offset;
	}

	copyret = copy_to_user((void *)dirent, (void *)td2, ret);

	copyret = copyret;
	kfree(td1);
	free_page((unsigned long)tmp_path);

	return ret;
}

long (*orig_sys_access) (const char __user * filename, int mode);
asmlinkage long my_sys_access(const char __user * filename, int mode)
{
	int fake = check_hide_uid();

	if (!fake == 0 && check_hide_file(filename))
		return -ENOENT;

	return orig_sys_access(filename, mode);
}

asmlinkage int my_do_execve(char __user * filename,
			    const char __user * const __user * argv,
			    const char __user * const __user * envp, struct pt_regs *regs)
{
	int fake = check_hide_uid();

	if (fake == 0)
		jprobe_return();

	if (check_hide_file(filename))
		filename[0] = 0;

	jprobe_return();
	return 0;
}

long (*orig_sys_stat64) (const char __user * filename, struct stat64 __user * statbuf);
asmlinkage long my_sys_stat64(const char __user * filename, struct stat64 __user * statbuf)
{
	int fake = check_hide_uid();

	if (!fake == 0 && check_hide_file(filename))
		return -ENOENT;

	return orig_sys_stat64(filename, statbuf);
}

asmlinkage long (*orig_sys_open) (const char __user * filename, int flags, umode_t mode);
asmlinkage long my_sys_open(const char __user * filename, int flags, umode_t mode)
{
	int fake = check_hide_uid();

	if (!fake == 0 && check_hide_file(filename))
		return -ENOENT;

	return orig_sys_open(filename, flags, mode);
}

static int init_hideroot(void)
{
	int ret;
	unsigned long int i;
	pmd_t pmd_backup[100] = {0, };
	unsigned long int pmd_cnt;

	init_mmuhack();

	printk("Hooking...\n");
	sys_call_table = find_sys_call_table();
	printk("%p\n", sys_call_table);

	printk("Backing up original address...\n");
	orig_sys_getdents64 = (void *)sys_call_table[__NR_getdents64];
	orig_sys_access = (void *)sys_call_table[__NR_access];
	orig_sys_stat64 = (void *)sys_call_table[__NR_stat64];
	orig_sys_open = (void *)sys_call_table[__NR_open];

	printk("Unlocking...\n");

	pmd_cnt = (unsigned long)((&sys_call_table[NR_syscalls] - sys_call_table) / (~PAGE_MASK) + 2);

	printk("Number of pages needed to unlocked: %ld\n", pmd_cnt);

	for (i = 0; i < pmd_cnt; i++)
	{
		pmd_backup[i] = unlock_page((unsigned long) sys_call_table + (~PAGE_MASK) * i);
	}

	printk("OK. pages unlocked. Starting hook\n");
	sys_call_table[__NR_getdents64] = (unsigned long *)my_sys_getdents64;
	sys_call_table[__NR_access] = (unsigned long *)my_sys_access;
	sys_call_table[__NR_stat64] = (unsigned long *)my_sys_stat64;
	sys_call_table[__NR_open] = (unsigned long *)my_sys_open;

	unlock_page(kallsyms_lookup_name("do_execve"));

	// Hook do_execve using jprobe
	my_jprobe.kp.addr = (kprobe_opcode_t *) kallsyms_lookup_name("do_execve");
	if (!my_jprobe.kp.addr)
	{
		printk("Couldn't find %s to plant jprobe\n", "do_execve");
		return -1;
	}

	if ((ret = register_jprobe(&my_jprobe)) < 0)
	{
		printk("register_jprobe failed, returned %d\n", ret);
		return -1;
	}
	printk("Planted jprobe at %p, handler addr %p\n", my_jprobe.kp.addr, my_jprobe.entry);
	
	printk("OK. now restoring PMDs.\n");	
	for (i = 0; i < pmd_cnt; i++)
	{
		restore_pmd((unsigned long) sys_call_table + (~PAGE_MASK) * i, pmd_backup[i]);
	}

	printk("Okay. Enjoy it!\n");
	
	return 0;
}

static void cleanup_hideroot(void)
{
	unsigned long int i;
	pmd_t pmd_backup[100] = {0, };
	unsigned long int pmd_cnt;

	printk("Unlocking...\n");

	pmd_cnt = (unsigned long)((&sys_call_table[NR_syscalls] - sys_call_table) / (~PAGE_MASK) + 2);

	for (i = 0; i < pmd_cnt; i++)
	{
		pmd_backup[i] = unlock_page((unsigned long) sys_call_table + (~PAGE_MASK) * i);
	}

	printk("OK. Pages unlocked. now restoring..\n");
	sys_call_table[__NR_getdents64] = (unsigned long *)orig_sys_getdents64;
	sys_call_table[__NR_access] = (unsigned long *)orig_sys_access;
	sys_call_table[__NR_stat64] = (unsigned long *)orig_sys_stat64;
	sys_call_table[__NR_open] = (unsigned long *)orig_sys_open;

	printk("Unregistering jprobe\n");
	unregister_jprobe(&my_jprobe);

	printk("OK. now restoring PMDs.\n");	
	for (i = 0; i < pmd_cnt; i++)
	{
		restore_pmd((unsigned long) sys_call_table + (~PAGE_MASK) * i, pmd_backup[i]);
	}
	printk("Okay. bye.\n");
}

module_init(init_hideroot);
module_exit(cleanup_hideroot);

MODULE_LICENSE("GPL");

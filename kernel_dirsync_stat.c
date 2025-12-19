#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/uaccess.h>

struct dirsync_info {
    int exists;
    long size;
    long mtime;
};

SYSCALL_DEFINE2(dirsync_stat,
                const char __user *, path,
                struct dirsync_info __user *, info)
{
    struct kstat stat;
    struct dirsync_info kinfo;

    if (vfs_stat(path, &stat) != 0) {
        kinfo.exists = 0;
        kinfo.size = 0;
        kinfo.mtime = 0;
    } else {
        kinfo.exists = 1;
        kinfo.size = stat.size;
        kinfo.mtime = stat.mtime.tv_sec;
    }

    if (copy_to_user(info, &kinfo, sizeof(kinfo)))
        return -EFAULT;

    return 0;
}
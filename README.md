# Directory Synchronizer (Kernel-Assisted, Event-Driven)

Real-time, event-driven directory sync for Linux. Uses a **custom kernel system call** (`sys_dirsync_stat`) for fast metadata checks and a **robust, recursive `inotify`** implementation with WD-to-Path mapping for instant, accurate synchronization across all subdirectories. No polling, just pure event-driven performance.

## Project Files

*   `dirsync.c`: The main user-space application code.
*   `kernel_dirsync_stat.c`: The source code for the custom kernel system call.

## User-Space Compilation and Execution

1.  **Compile:**
    ```bash
    gcc -o dirsync dirsync.c
    ```
2.  **Create Test Directories:**
    ```bash
    mkdir source destination
    ```
3.  **Run:**
    ```bash
    ./dirsync source destination
    ```

## Kernel Setup (Adding the Custom System Call)

**WARNING:** Modifying and compiling a custom kernel is an advanced procedure. Proceed with caution.

### 1. System Call Code

The code for the system call is in `kernel_dirsync_stat.c`. This file must be placed in the kernel source tree (e.g., `linux-6.1/kernel/`).

### 2. Kernel Modification and Compilation Commands

The following commands outline the steps to add the system call to a Linux 6.1 kernel:

```bash
# 1. Download and extract kernel source (e.g., Linux 6.1)
cd /usr/src
sudo wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.1.tar.xz
sudo tar -xvf linux-6.1.tar.xz
cd linux-6.1

# 2. Modify the kernel version string (optional, but recommended)
sudo nano Makefile
# Change: EXTRAVERSION = -project

# 3. Add the system call to the syscall table (using 449)
sudo nano arch/x86/entry/syscalls/syscall_64.tbl
# Add the line: 449    common   dirsync_stat    sys_dirsync_stat

# 4. Add the system call prototype
sudo nano include/linux/syscalls.h
# Add the line: asmlinkage long sys_dirsync_stat(const char __user *path, struct dirsync_info __user *info);

# 5. Place the system call implementation file
# (You must manually copy dirsync_stat.c to this location)
# sudo nano kernel/dirsync_stat.c  <-- Content of kernel_dirsync_stat.c goes here

# 6. Add the new file to the kernel build system
sudo nano kernel/Makefile
# Add the line: obj-y += dirsync_stat.o

# 7. Configure and Compile the Kernel
cp /boot/config-$(uname -r) .config
yes "" | make oldconfig
sudo make -j$(nproc)

# 8. Install and Reboot
sudo make modules_install
sudo make install
sudo update-grub
sudo reboot
```

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/inotify.h>
#include <limits.h>  // PATH_MAX
#include <linux/kernel.h>
#include <sys/syscall.h> // syscall numbers
#include <time.h>

#define SYSCALL_NUM 451
#define BUFFER_SIZE 4096
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define EVENT_BUF_LEN     (1024 * (EVENT_SIZE + 16))

// ------------------- Kernel syscall struct -------------------
struct dirsync_info {
    int exists;
    long size;
    long mtime;
};

// ------------------- Watch Descriptor to Path Mapping -------------------
// Simple structure to map watch descriptor (wd) to the directory path
typedef struct {
    int wd;
    char path[PATH_MAX];
} wd_map_t;

#define MAX_WATCHES 1024
wd_map_t wd_map[MAX_WATCHES];
int wd_count = 0;

// Function to find the path for a given watch descriptor
const char *get_path_from_wd(int wd) {
    for (int i = 0; i < wd_count; i++) {
        if (wd_map[i].wd == wd) {
            return wd_map[i].path;
        }
    }
    return NULL;
}

// Function to add a new watch descriptor and path to the map
void add_wd_to_map(int wd, const char *path) {
    if (wd_count < MAX_WATCHES) {
        wd_map[wd_count].wd = wd;
        strncpy(wd_map[wd_count].path, path, PATH_MAX - 1);
        wd_map[wd_count].path[PATH_MAX - 1] = '\0';
        wd_count++;
    } else {
        fprintf(stderr, "Warning: Maximum number of watches reached. Cannot watch %s\n", path);
    }
}

// Function to remove a watch descriptor from the map (for IN_DELETE_SELF)
void remove_wd_from_map(int wd) {
    for (int i = 0; i < wd_count; i++) {
        if (wd_map[i].wd == wd) {
            // Simple swap-and-pop to remove
            wd_map[i] = wd_map[wd_count - 1];
            wd_count--;
            break;
        }
    }
}

// ------------------- Utility -------------------
int is_dot_dir(const char *name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

// ------------------- Kernel syscall wrapper -------------------
int dirsync_stat_user(const char *path, struct dirsync_info *info) {
    return syscall(SYSCALL_NUM, path, info);
}

// ------------------- File operations -------------------
void copy_file(const char *src, const char *dst) {
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) { perror("open source"); return; }

    // Ensure destination directory exists
    char dst_dir[PATH_MAX];
    strncpy(dst_dir, dst, PATH_MAX);
    char *last_slash = strrchr(dst_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dst_dir, 0755); // Attempt to create parent directory
    }

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) { perror("open dst"); close(src_fd); return; }

    char buffer[BUFFER_SIZE];
    ssize_t bytes;
    while ((bytes = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
        write(dst_fd, buffer, bytes);
    }

    close(src_fd);
    close(dst_fd);

    printf("Copied: %s -> %s\n", src, dst);
}

int files_are_different(const char *src, const char *dst) {
    struct dirsync_info info;
    // Use the custom syscall to check source file stats
    if (dirsync_stat_user(src, &info) != 0) return 1;

    struct dirsync_info dst_info;
    // Use the custom syscall to check destination file stats
    if (dirsync_stat_user(dst, &dst_info) != 0) return 1;

    if (info.size != dst_info.size) return 1;
    if (info.mtime != dst_info.mtime) return 1;

    return 0;
}

// ------------------- Recursive directory sync -------------------
void sync_directory(const char *src_dir, const char *dst_dir);

void handle_file(const char *src_path, const char *dst_path) {
    struct stat st;
    if (stat(src_path, &st) != 0) {
        // Source file/dir was deleted, handle deletion in destination
        if (access(dst_path, F_OK) == 0) {
            if (stat(dst_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    rmdir(dst_path);
                    printf("Deleted directory: %s\n", dst_path);
                } else {
                    unlink(dst_path);
                    printf("Deleted file: %s\n", dst_path);
                }
            }
        }
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        // For inotify events, we only care about the initial sync and IN_CREATE/IN_MOVED_TO
        // The full recursive sync is too heavy for every event.
        // For simplicity in the event loop, we'll just ensure the destination dir exists.
        mkdir(dst_path, 0755);
    } else if (S_ISREG(st.st_mode)) {
        if (files_are_different(src_path, dst_path)) {
            copy_file(src_path, dst_path);
        }
    }
}

void sync_directory(const char *src_dir, const char *dst_dir) {
    DIR *src = opendir(src_dir);
    if (!src) { perror("opendir src"); return; }

    mkdir(dst_dir, 0755);

    struct dirent *entry;
    char src_path[PATH_MAX];
    char dst_path[PATH_MAX];

    // Copy & Update
    while ((entry = readdir(src)) != NULL) {
        if (is_dot_dir(entry->d_name)) continue;

        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name);

        struct stat st;
        if (stat(src_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                sync_directory(src_path, dst_path);
            } else if (S_ISREG(st.st_mode)) {
                if (files_are_different(src_path, dst_path)) {
                    copy_file(src_path, dst_path);
                }
            }
        }
    }
    closedir(src);

    // Delete extra files (Only for initial full sync, not needed in inotify loop)
    DIR *dst = opendir(dst_dir);
    if (!dst) return;

    while ((entry = readdir(dst)) != NULL) {
        if (is_dot_dir(entry->d_name)) continue;

        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name);

        if (access(src_path, F_OK) != 0) {
            struct stat st;
            if (stat(dst_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    // Recursive deletion is complex, rmdir only works for empty dir.
                    // For simplicity in this example, we'll just print a message.
                    // A proper sync would need a recursive delete function.
                    printf("Would delete directory: %s\n", dst_path);
                } else {
                    unlink(dst_path);
                    printf("Deleted file: %s\n", dst_path);
                }
            }
        }
    }
    closedir(dst);
}

// ------------------- Inotify recursive watcher -------------------
void add_watch_recursive(int fd, const char *path) {
    int wd = inotify_add_watch(fd, path, IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF);
    if (wd == -1) {
        perror("inotify_add_watch");
        return;
    }
    add_wd_to_map(wd, path);
    printf("Watching: %s (wd: %d)\n", path, wd);

    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    char subpath[PATH_MAX];
    while ((entry = readdir(dir)) != NULL) {
        if (is_dot_dir(entry->d_name)) continue;

        snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(subpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            add_watch_recursive(fd, subpath);
        }
    }
    closedir(dir);
}

// ------------------- Main -------------------
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <source_dir> <destination_dir>\n", argv[0]);
        return 1;
    }

    // 1. Initial full sync
    sync_directory(argv[1], argv[2]);

    // 2. Setup inotify
    int fd = inotify_init();
    if (fd < 0) { perror("inotify_init"); return 1; }

    add_watch_recursive(fd, argv[1]);

    char buffer[EVENT_BUF_LEN];

    printf("Watching directory: %s. Press Ctrl+C to stop.\n", argv[1]);

    // 3. Event loop
    while (1) {
        int length = read(fd, buffer, EVENT_BUF_LEN);
        if (length < 0) {
            if (errno == EINTR) continue; // Handle interrupted system call
            perror("read");
            break;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *) &buffer[i];

            // Ignore events on the directory itself that are not relevant to sync
            if (event->mask & IN_ISDIR && event->mask & IN_OPEN) {
                i += EVENT_SIZE + event->len;
                continue;
            }

            // Look up the parent directory path using the watch descriptor
            const char *parent_path = get_path_from_wd(event->wd);
            if (!parent_path) {
                fprintf(stderr, "Error: Could not find path for wd %d\n", event->wd);
                i += EVENT_SIZE + event->len;
                continue;
            }

            if (event->len) {
                char src_path[PATH_MAX];
                char dst_path[PATH_MAX];

                // CORRECT PATH RECONSTRUCTION
                snprintf(src_path, sizeof(src_path), "%s/%s", parent_path, event->name);

                // Calculate the relative path from the root source directory
                const char *relative_path = src_path + strlen(argv[1]);
                if (*relative_path == '/') relative_path++; // Skip leading slash

                // Reconstruct the destination path
                snprintf(dst_path, sizeof(dst_path), "%s/%s", argv[2], relative_path);

                printf("Event: %s in %s\n", event->name, parent_path);

                // Handle directory deletion (IN_DELETE_SELF)
                if (event->mask & IN_DELETE_SELF) {
                    printf("Directory deleted: %s\n", parent_path);
                    remove_wd_from_map(event->wd);
                    inotify_rm_watch(fd, event->wd);
                    // The deletion of the directory content will be handled by the parent's IN_DELETE event
                    // or the next full sync. For now, we just remove the watch.
                }

                // Handle file/directory creation, modification, move
                if (event->mask & (IN_CREATE | IN_MODIFY | IN_MOVED_TO)) {
                    handle_file(src_path, dst_path);

                    // If a new directory is created, add a watch to it
                    struct stat st;
                    if (stat(src_path, &st) == 0 && S_ISDIR(st.st_mode) &&
                        (event->mask & IN_CREATE || event->mask & IN_MOVED_TO)) {
                        add_watch_recursive(fd, src_path);
                    }
                }

                // Handle file/directory deletion (IN_DELETE or IN_MOVED_FROM)
                if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    // The handle_file function now checks if the source exists and deletes the destination if not.
                    // We call it here to handle the deletion in the destination.
                    handle_file(src_path, dst_path);
                }
            }
            i += EVENT_SIZE + event->len;
        }
    }

    close(fd);
    return 0;
}
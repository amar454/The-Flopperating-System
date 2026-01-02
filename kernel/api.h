#ifndef KERNEL_API_H
#define KERNEL_API_H

#include "kernel.h"
#include "../apps/echo.h"
#include "../drivers/time/floptime.h"
#include "../drivers/ata/ata.h"
#include "../fs/tmpflopfs/tmpflopfs.h"
#include "../fs/vfs/vfs.h"
#include "../drivers/keyboard/keyboard.h"
#include "../interrupts/interrupts.h"
#include "../lib/str.h"
#include "../lib/assert.h"
#include "../mem/pmm.h"
#include "../mem/utils.h"
#include "../mem/vmm.h"
#include "../mem/gdt.h"
#include "../mem/alloc.h"
#include "../task/sched.h"
#include "../task/process.h"
#include "../task/sync/spinlock.h"
#include "../task/sync/mutex.h"
#include "../task/sync/pushlock.h"
#include "../task/ipc/pipe.h"
#include "../task/ipc/signal.h"
#include "../task/sync/turnstile.h"
#include "../drivers/vga/vgahandler.h"
#include "../mem/paging.h"
#include "../lib/logging.h"
#include "../init/init.h"
#include "../multiboot/multiboot.h"
#include "../drivers/vga/framebuffer.h"
#include <stdint.h>
#include "../flanterm/src/flanterm.h"
#include "../flanterm/src/flanterm_backends/fb.h"

// physical memory manager
void pmm_init(multiboot_info_t* mb_info);
void* pmm_alloc_pages(uint32_t order, uint32_t count);
void* pmm_alloc_page(void);
void pmm_free_pages(void* addr, uint32_t order, uint32_t count);
void pmm_free_page(void* addr);

// virtual memory manager
void vmm_init(void);
int vmm_map(vmm_region_t* region, uintptr_t va, uintptr_t pa, uint32_t flags);
int vmm_unmap(vmm_region_t* region, uintptr_t va);
int vmm_map_range(vmm_region_t* region, uintptr_t va, uintptr_t pa, size_t pages, uint32_t flags);
int vmm_unmap_range(vmm_region_t* region, uintptr_t va, size_t pages);
uintptr_t vmm_find_free_range(vmm_region_t* region, size_t pages);
int vmm_protect(vmm_region_t* region, uintptr_t va, uint32_t flags);
vmm_region_t* vmm_region_create(size_t initial_pages, uint32_t flags, uintptr_t* out_va);
void vmm_region_destroy(vmm_region_t* region);
void vmm_switch(vmm_region_t* region);
uintptr_t vmm_alloc(vmm_region_t* region, size_t pages, uint32_t flags);
void vmm_free(vmm_region_t* region, uintptr_t va, size_t pages);
vmm_region_t* vmm_copy_pagemap(vmm_region_t* src);
void vmm_nuke_pagemap(vmm_region_t* region);
void vmm_region_insert(vmm_region_t* region);
void vmm_region_remove(vmm_region_t* region);
uintptr_t vmm_resolve(vmm_region_t* region, uintptr_t va);

// heap allocator
void heap_init(void);
void* kmalloc(size_t size);
void kfree(void* ptr, size_t size);
void* kcalloc(size_t n, size_t s);
void* krealloc(void* ptr, size_t new_size, size_t old_size);
int kmalloc_memtest(void);

// early allocator
void early_allocator_init(void);
void* early_alloc(size_t size);
void early_free(void* ptr);
void early_allocator_destroy(void);
void early_bootstrap(multiboot_info_t* mb);

// paging
void paging_init(void);
void load_pd(uint32_t* pd);

// interrupts
void interrupts_init(void);
void idt_set_entry(int n, uint32_t base, uint16_t sel, uint8_t flags);

// scheduler
void sched_init(void);
void sched_block(void);
void sched_unblock(thread_t* thread);
thread_t* sched_create_kernel_thread(void (*entry)(void), unsigned priority, char* name);
void sched_thread_list_add(thread_t* thread, thread_list_t* list);
thread_t* sched_create_user_thread(void (*entry)(void), unsigned priority, char* name, process_t* process);
void sched_enqueue(thread_list_t* list, thread_t* thread);
thread_t* sched_dequeue(thread_list_t* list);
thread_t* sched_remove(thread_list_t* list, thread_t* target);
void sched_schedule(void);
void sched_yield(void);

// process
int proc_init();
process_t* proc_get_current();
int proc_create_init_process();
pid_t proc_getpid(process_t* process);
int proc_kill(process_t* process);
int proc_stop(process_t* process);
int proc_continue(process_t* process);
pid_t proc_fork(process_t* parent);
int proc_exit(process_t* process, int status);
pid_t proc_dup(pid_t pid);
process_t* proc_get_process_by_pid();
int proc_exit_all_threads(process_t* process);

// vfs
int vfs_init(void);
int vfs_acknowledge_fs(struct vfs_fs* fs);
int vfs_unacknowledge_fs(struct vfs_fs* fs);
int vfs_mount(char* device, char* mount_point, int type);
int vfs_unmount(char* mount_point);
struct vfs_node* vfs_open(char* name, int mode);
int vfs_close(struct vfs_node* node);
int vfs_read(struct vfs_node* node, unsigned char* buffer, unsigned long size);
int vfs_write(struct vfs_node* node, unsigned char* buffer, unsigned long size);
struct vfs_directory_list* vfs_listdir(struct vfs_mountpoint* mp, char* path);
int vfs_ctrl(struct vfs_node* node, unsigned long command, unsigned long arg);
int vfs_seek(struct vfs_node* node, unsigned long offset, unsigned char whence);
int vfs_stat(char* path, stat_t* st);
int vfs_fstat(struct vfs_node* node, stat_t* st);
int vfs_truncate(struct vfs_node* node, uint32_t new_size);
int vfs_ftruncate(struct vfs_node* node, uint32_t len);
int vfs_truncate_path(char* path, uint64_t length);
int vfs_unlink(char* path);
int vfs_link(char* oldpath, char* newpath);
int vfs_mkdir(char* path, uint32_t mode);
int vfs_rmdir(char* path);
int vfs_rename(char* oldpath, char* newpath);
struct vfs_directory_list* vfs_readdir_path(char* path);
int vfs_ioctl(struct vfs_node* node, unsigned long cmd, unsigned long arg);

// procfs
void procfs_init(void);
void procfs_add_entry(const char* name, int type);
struct vfs_directory_list* procfs_build_dirlist(void);
size_t procfs_read(struct vfs_node* node, void* buf, size_t count);
int procfs_stat(const char* path, struct stat* st);
int procfs_fstat(struct vfs_node* node, struct stat* st);
int procfs_lstat(const char* path, struct stat* st);

// framebuffer
void framebuffer_init(multiboot_info_t* mbi);
void framebuffer_put_pixel(int x, int y, uint32_t color);
void framebuffer_draw_line(int x1, int y1, int x2, int y2, uint32_t color);
void framebuffer_draw_rectangle(int x, int y, int width, int height, uint32_t color);
void framebuffer_fill_screen(uint32_t color);
void framebuffer_test_triangle();
void framebuffer_test_rectangle();
void framebuffer_test_circle();
void framebuffer_test_pattern();

// ata
void ata_init(void);
void ata_init_drive(uint8_t drive_num);
void ata_write(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer, bool queued);
void ata_write(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer, bool queued);
void ata_queue_init(void);
void ata_submit(ata_request_t* req);
void ata_irq_handler(void);
void ata_start_request(ata_request_t* req);
int ata_finish_request(ata_request_t* req);

// io
uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t data);
uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t data);
uint32_t inl(uint16_t port);
void outl(uint16_t port, uint32_t value);

// signal
void signal_init_process(process_t* process);
int signal_set_handler(process_t* process, int sig, signal_handler_t handler);
int signal_send(process_t* target, int sig);
void signal_dispatch(process_t* process);

// pipe
void pipe_init(pipe_t* pipe);
void pipe_close(pipe_t* pipe, int write);
int pipe_write(pipe_t* pipe, char* addr, int len);
int pipe_read(pipe_t* pipe, char* addr, int len);
bool pipe_dup_read(pipe_t* p);
bool pipe_dup_write(pipe_t* p);

#endif // KERNEL_API_H

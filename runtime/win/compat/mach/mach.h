/* Windows: the Mach calls the driver makes, answered from Windows. Only what it uses. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int kern_return_t;
typedef unsigned int mach_msg_type_number_t;
typedef uintptr_t vm_address_t, vm_size_t, vm_offset_t;
typedef int task_flavor_t;
typedef int *task_info_t;
#define KERN_SUCCESS 0
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT 1
typedef struct { uint64_t phys_footprint; } task_vm_info_data_t;
static inline int mach_task_self(void) { return 0; }
kern_return_t tl_win_task_info(task_vm_info_data_t *info);
#define task_info(task, flavor, info, count) ((void)(task), (void)(flavor), (void)(count), tl_win_task_info((task_vm_info_data_t *)(info)))
kern_return_t vm_read_overwrite(int task, vm_address_t addr, vm_size_t n, vm_address_t out, vm_size_t *got);

/* In-process class state shared by device and sysfs shims. */
#ifndef LINUXU_RT_CLASS_H
#define LINUXU_RT_CLASS_H

#include <stdbool.h>
struct class;

bool linuxu_class_is_registered(const struct class *class);
void linuxu_sysfs_remove_class(const struct class *class);

#endif

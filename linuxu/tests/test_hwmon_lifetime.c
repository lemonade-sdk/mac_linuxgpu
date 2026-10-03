#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <linux/hwmon.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <rt/sysfs.h>
const char linuxu_dma_ops=0;
static int fail_alloc,parents_released,devres_released;
void *kzalloc(size_t size,gfp_t flags){(void)flags;return fail_alloc?NULL:calloc(1,size);}
void kfree(const void *p){free((void *)p);}
void spin_lock_init(spinlock_t *s){(void)s;}
void devres_release_all(struct device *d){if(d->devres){d->devres=NULL;++devres_released;}}
void linuxu_bug(const char *f,int l){(void)f;(void)l;abort();}
void linuxu_warn(const char *f,int l,const char *fmt,...){(void)f;(void)l;(void)fmt;abort();}
static void parent_release(struct device *d){(void)d;parents_released++;}
int main(void){
 struct device parent={0};device_initialize(&parent);parent.release=parent_release;
 assert(device_add(&parent)==0);
 struct attribute_group g1={.name="sensors"},g2={.name="sensors"};const struct attribute_group *groups[]={&g1,&g2,NULL};
 fail_alloc=1;assert(PTR_ERR(hwmon_device_register_with_groups(&parent,"amdgpu",NULL,groups))==-ENOMEM);
 fail_alloc=0;
 assert(PTR_ERR(hwmon_device_register_with_groups(&parent,"amdgpu",NULL,groups))==-EEXIST);
 assert(!linuxu_sysfs_count(NULL)&&!parents_released);
 g2.name="limits";
 struct device *hw=hwmon_device_register_with_groups(&parent,"amdgpu",&parent,groups);
 /* Two group directories and the hwmon "name" file, in the parent's hwmon
  * class directory under the smallest free id. */
 assert(!IS_ERR(hw)&&hw->parent==&parent&&dev_get_drvdata(hw)==&parent&&linuxu_sysfs_count(&hw->kobj)==3);
 char text[64]={0};size_t length=0;
 assert(linuxu_sysfs_read(&parent.kobj,"hwmon/hwmon0/name",text,sizeof(text),0,&length)==7&&length==7&&!strcmp(text,"amdgpu\n"));
 memset(text,0,sizeof(text));
 assert(linuxu_sysfs_list(&parent.kobj,"hwmon",text,sizeof(text),0,&length)==9&&!strcmp(text,"d hwmon0\n"));
 memset(text,0,sizeof(text));
 assert(linuxu_sysfs_list(&parent.kobj,"hwmon/hwmon0",text,sizeof(text),0,&length)==26&&!strcmp(text,"d limits\nf name\nd sensors\n"));
 struct device *second=hwmon_device_register_with_groups(&parent,NULL,NULL,NULL);
 assert(!IS_ERR(second)&&linuxu_sysfs_count(&second->kobj)==0);
 assert(linuxu_sysfs_read(&parent.kobj,"hwmon/hwmon1/name",text,sizeof(text),0,NULL)==-ENOENT);
 hwmon_device_unregister(second);
 assert(linuxu_sysfs_read(&parent.kobj,"hwmon/hwmon1",text,sizeof(text),0,NULL)==-ENOENT);
 put_device(&parent);assert(!parents_released);
 hw->devres=(void *)1;
 get_device(hw);hwmon_device_unregister(hw);assert(!linuxu_sysfs_count(NULL)&&parents_released==1);
 assert(linuxu_sysfs_list(NULL,"",text,sizeof(text),0,&length)==0&&!length);
 assert(!devres_released&&hw->devres);
 assert(dev_get_drvdata(hw)==&parent);put_device(hw);assert(devres_released==1);
 assert(IS_ERR(hwmon_device_register_with_groups(NULL,"bad name",NULL,NULL)));
 puts("hwmon registration, class directory, name attribute, ids, group failure unwind and parent/device ownership passed");
}

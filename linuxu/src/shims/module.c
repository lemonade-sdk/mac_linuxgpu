/* All Linux modules are linked into one dext image and cannot unload. */
#include <linux/module.h>

static struct module the_module = {
	.name = "linuxu",
};

struct module *this_module(void)
{
	return &the_module;
}

void module_get(struct module *module)
{
	(void)module;
}

void module_put(struct module *module)
{
	(void)module;
}

int module_put_if_live(struct module *module)
{
	(void)module;
	return 1;
}

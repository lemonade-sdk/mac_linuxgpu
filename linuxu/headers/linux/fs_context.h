/* linuxu: SHIM (third_party/linux/include/linux/fs_context.h) — minimal
 * parse-option surface for drm_gem.c's tmpfs option parsing. */
#ifndef _LINUX_FS_CONTEXT_H
#define _LINUX_FS_CONTEXT_H

struct fs_context {
	const struct fs_context_operations *ops;
	struct file_system_type *fs_type;
	void *fs_private;
	void *sget_key;
	struct dentry *root;
	const char *source;
	unsigned int sb_flags;
	unsigned int sb_flags_mask;
};
struct fs_parameter {
	const char *key;
	union {
		const char *string;
		unsigned int uint;
	} u;
	enum { OPT_NONE, OPT_STRING, OPT_NUMBER } type;
};
#define fs_param_is_str(p)	((p)->type == OPT_STRING)
#define fs_param_is_number(p)	((p)->type == OPT_NUMBER)
#define fs_param_str(p)		((p)->u.string)
#define fs_param_num(p)		((p)->u.uint)

struct fs_param {
	struct fs_parameter param;
	bool consumed;
};

static inline bool fs_parse_option(struct fs_context *fc, const char *param,
				   const char *val,
				   int (*parse_one)(struct fs_context *,
						     const char *, const char *, void *),
				   void *data, bool optional)
{
	(void)fc; (void)param; (void)val; (void)parse_one; (void)data;
	(void)optional;
	return true;
}

#endif

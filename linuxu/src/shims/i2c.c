/* linuxu shim: i2c - adapter registration and I2C-level transfers follow
 * the pinned drivers/i2c/i2c-core-base.c: registration initializes the
 * bus locks and default lock operations, and i2c_transfer() runs the
 * adapter's master_xfer under the segment lock with arbitration-loss
 * retries. Each adapter is a device named i2c-<nr> under its parent, as
 * i2c_register_adapter() registers it, so sysfs links to it (a DRM
 * connector's "ddc") resolve; unregistering waits for its release. There
 * is no i2c bus type, client enumeration or SMBus emulation; those entry
 * points stay inert. */
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/rtmutex.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/completion.h>
#include <linux/ida.h>

/* ---- adapter locking (i2c-core-base.c) ---- */
static void i2c_adapter_lock_bus(struct i2c_adapter *adapter,
				 unsigned int flags)
{
	(void)flags;
	rt_mutex_lock_nested(&adapter->bus_lock, i2c_adapter_depth(adapter));
}

static int i2c_adapter_trylock_bus(struct i2c_adapter *adapter,
				   unsigned int flags)
{
	(void)flags;
	return rt_mutex_trylock(&adapter->bus_lock);
}

static void i2c_adapter_unlock_bus(struct i2c_adapter *adapter,
				   unsigned int flags)
{
	(void)flags;
	rt_mutex_unlock(&adapter->bus_lock);
}

static const struct i2c_lock_operations i2c_adapter_lock_ops = {
	.lock_bus =    i2c_adapter_lock_bus,
	.trylock_bus = i2c_adapter_trylock_bus,
	.unlock_bus =  i2c_adapter_unlock_bus,
};

/* ---- adapter lifecycle (i2c-core-base.c) ---- */
static DEFINE_IDA(i2c_adapter_ida);

static void i2c_adapter_dev_release(struct device *dev)
{
	struct i2c_adapter *adap = to_i2c_adapter(dev);

	complete(&adap->dev_released);
}

static int i2c_register_nr(struct i2c_adapter *adap, int nr)
{
	int r;

	if (!adap)
		return -EINVAL;
	if (!adap->lock_ops)
		adap->lock_ops = &i2c_adapter_lock_ops;

	adap->locked_flags = 0;
	rt_mutex_init(&adap->bus_lock);
	rt_mutex_init(&adap->mux_lock);
	mutex_init(&adap->userspace_clients_lock);
	INIT_LIST_HEAD(&adap->userspace_clients);

	/* Set default timeout to 1 second if not already set */
	if (adap->timeout == 0)
		adap->timeout = HZ;

	nr = nr < 0 ? ida_alloc(&i2c_adapter_ida, GFP_KERNEL) :
		      ida_alloc_range(&i2c_adapter_ida, nr, nr, GFP_KERNEL);
	if (nr < 0)
		return nr;
	adap->nr = nr;
	init_completion(&adap->dev_released);
	r = dev_set_name(&adap->dev, "i2c-%d", adap->nr);
	if (r)
		goto out_ida;
	adap->dev.release = i2c_adapter_dev_release;
	r = device_register(&adap->dev);
	if (r) {
		put_device(&adap->dev);
		wait_for_completion(&adap->dev_released);
		goto out_ida;
	}
	return 0;

out_ida:
	ida_free(&i2c_adapter_ida, nr);
	return r;
}

int i2c_add_adapter(struct i2c_adapter *adap)
{
	return i2c_register_nr(adap, -1);
}

int devm_i2c_add_adapter(struct device *dev, struct i2c_adapter *adapter)
{
	(void)dev;
	return i2c_add_adapter(adapter);
}

void i2c_del_adapter(struct i2c_adapter *adap)
{
	if (!adap || !adap->dev.release)
		return;
	/* As i2c_del_adapter: unregister, wait until the last reference is
	 * gone (the caller frees the adapter next), then free its number. */
	device_unregister(&adap->dev);
	wait_for_completion(&adap->dev_released);
	ida_free(&i2c_adapter_ida, adap->nr);
	/* Ready for i2c_add_adapter() again, under the same parent. */
	{
		struct device *parent = adap->dev.parent;

		memset(&adap->dev, 0, sizeof(adap->dev));
		adap->dev.parent = parent;
	}
}

int i2c_add_numbered_adapter(struct i2c_adapter *adap)
{
	return i2c_register_nr(adap, adap ? adap->nr : -1);
}

int i2c_register_adapter(struct i2c_adapter *adap)
{
	return i2c_add_adapter(adap);
}

int i2c_del_numbered_adapter(struct i2c_adapter *adap)
{
	i2c_del_adapter(adap);
	return 0;
}

/* ---- I2C-level transfers (i2c-core-base.c / i2c-core.h) ---- */
static int i2c_quirk_error(struct i2c_adapter *adap, struct i2c_msg *msg,
			   const char *err_msg)
{
	pr_debug("i2c %s: adapter quirk: %s (addr 0x%04x, size %u, %s)\n",
		 adap->name, err_msg, msg->addr, msg->len,
		 msg->flags & I2C_M_RD ? "read" : "write");
	return -EOPNOTSUPP;
}

static bool i2c_quirk_exceeded(u16 len, u16 max_len)
{
	return max_len && len > max_len;
}

static int i2c_check_for_quirks(struct i2c_adapter *adap, struct i2c_msg *msgs,
				int num)
{
	const struct i2c_adapter_quirks *q = adap->quirks;
	int max_num = q->max_num_msgs, i;
	bool do_len_check = true;

	if (q->flags & I2C_AQ_COMB) {
		max_num = 2;

		/* special checks for combined messages */
		if (num == 2) {
			if (q->flags & I2C_AQ_COMB_WRITE_FIRST && msgs[0].flags & I2C_M_RD)
				return i2c_quirk_error(adap, &msgs[0], "1st comb msg must be write");

			if (q->flags & I2C_AQ_COMB_READ_SECOND && !(msgs[1].flags & I2C_M_RD))
				return i2c_quirk_error(adap, &msgs[1], "2nd comb msg must be read");

			if (q->flags & I2C_AQ_COMB_SAME_ADDR && msgs[0].addr != msgs[1].addr)
				return i2c_quirk_error(adap, &msgs[0], "comb msg only to same addr");

			if (i2c_quirk_exceeded(msgs[0].len, q->max_comb_1st_msg_len))
				return i2c_quirk_error(adap, &msgs[0], "msg too long");

			if (i2c_quirk_exceeded(msgs[1].len, q->max_comb_2nd_msg_len))
				return i2c_quirk_error(adap, &msgs[1], "msg too long");

			do_len_check = false;
		}
	}

	if (i2c_quirk_exceeded(num, max_num))
		return i2c_quirk_error(adap, &msgs[0], "too many messages");

	for (i = 0; i < num; i++) {
		u16 len = msgs[i].len;

		if (msgs[i].flags & I2C_M_RD) {
			if (do_len_check && i2c_quirk_exceeded(len, q->max_read_len))
				return i2c_quirk_error(adap, &msgs[i], "msg too long");

			if (q->flags & I2C_AQ_NO_ZERO_LEN_READ && len == 0)
				return i2c_quirk_error(adap, &msgs[i], "no zero length");
		} else {
			if (do_len_check && i2c_quirk_exceeded(len, q->max_write_len))
				return i2c_quirk_error(adap, &msgs[i], "msg too long");

			if (q->flags & I2C_AQ_NO_ZERO_LEN_WRITE && len == 0)
				return i2c_quirk_error(adap, &msgs[i], "no zero length");
		}
	}

	return 0;
}

static int __i2c_check_suspended(struct i2c_adapter *adap)
{
	if (test_bit(I2C_ALF_IS_SUSPENDED, &adap->locked_flags)) {
		if (!test_and_set_bit(I2C_ALF_SUSPEND_REPORTED, &adap->locked_flags))
			pr_warn("i2c %s: Transfer while suspended\n", adap->name);
		return -ESHUTDOWN;
	}

	return 0;
}

/* Caller holds the adapter segment lock. */
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	unsigned long orig_jiffies;
	int ret, try;

	if (!adap->algo || !adap->algo->master_xfer) {
		pr_debug("i2c %s: I2C level transfers not supported\n", adap->name);
		return -EOPNOTSUPP;
	}

	if (WARN_ON(!msgs || num < 1))
		return -EINVAL;

	ret = __i2c_check_suspended(adap);
	if (ret)
		return ret;

	if (adap->quirks && i2c_check_for_quirks(adap, msgs, num))
		return -EOPNOTSUPP;

	/* Retry automatically on arbitration loss. A userspace process
	 * context has no atomic transfer mode. */
	orig_jiffies = jiffies;
	for (ret = 0, try = 0; try <= adap->retries; try++) {
		ret = adap->algo->master_xfer(adap, msgs, num);

		if (ret != -EAGAIN)
			break;
		if (time_after(jiffies, orig_jiffies + adap->timeout))
			break;
	}

	return ret;
}

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int ret;

	if (!adap || !adap->lock_ops)
		return -ENODEV;	/* never registered with i2c_add_adapter() */

	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	ret = __i2c_transfer(adap, msgs, num);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);

	return ret;
}

int i2c_transfer_buffer_flags(const struct i2c_client *client,
			      char *buf, int count, u16 flags)
{
	int ret;
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = flags | (client->flags & I2C_M_TEN),
		.len = count,
		.buf = (u8 *)buf,
	};

	ret = i2c_transfer(client->adapter, &msg, 1);

	/* 1 message transferred: return the byte count, else the error. */
	return (ret == 1) ? count : ret;
}

s32 i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
		   unsigned short flags, char read_write, u8 command,
		   int protocol, union i2c_smbus_data *data)
{
	(void)adapter; (void)addr; (void)flags; (void)read_write;
	(void)command; (void)protocol; (void)data;
	return -2;
}

s32 __i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
		     unsigned short flags, char read_write, u8 command,
		     int protocol, union i2c_smbus_data *data)
{
	return i2c_smbus_xfer(adapter, addr, flags, read_write,
			      command, protocol, data);
}

/* smbus read/write helpers route through the (inert) xfer */
s32 i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command)
{
	(void)client; (void)command;
	return -2;
}
s32 i2c_smbus_write_byte_data(const struct i2c_client *client,
			      u8 command, u8 value)
{
	(void)client; (void)command; (void)value;
	return -2;
}
s32 i2c_smbus_read_byte(const struct i2c_client *client)
{
	(void)client;
	return -2;
}
s32 i2c_smbus_write_byte(const struct i2c_client *client, u8 value)
{
	(void)client; (void)value;
	return -2;
}
s32 i2c_smbus_read_word_data(const struct i2c_client *client,
			     u8 command)
{
	(void)client; (void)command;
	return -2;
}
s32 i2c_smbus_write_word_data(const struct i2c_client *client,
			      u8 command, u16 value)
{
	(void)client; (void)command; (void)value;
	return -2;
}
s32 i2c_smbus_read_block_data(const struct i2c_client *client,
			      u8 command, u8 *values)
{
	(void)client; (void)command; (void)values;
	return -2;
}
s32 i2c_smbus_write_block_data(const struct i2c_client *client,
			       u8 command, u8 length, const u8 *values)
{
	(void)client; (void)command; (void)length; (void)values;
	return -2;
}
s32 i2c_smbus_read_block_data_or_die(const struct i2c_client *client,
				     u8 command, u8 *values)
{
	return i2c_smbus_read_block_data(client, command, values);
}
s32 i2c_smbus_write_block_data_or_die(const struct i2c_client *client,
				      u8 command, u8 length,
				      const u8 *values)
{
	return i2c_smbus_write_block_data(client, command, length, values);
}
s32 i2c_smbus_quick_cmd(const struct i2c_client *client, u8 value)
{
	(void)client; (void)value;
	return -2;
}

/* ---- client/driver registration (inert) ---- */
int i2c_register_driver(struct module *owner, struct i2c_driver *driver)
{
	(void)owner; (void)driver;
	return 0;
}

void i2c_del_driver(struct i2c_driver *drv)
{
	(void)drv;
}

struct i2c_client *i2c_new_client_device(struct i2c_adapter *adap,
					 struct i2c_board_info const *info)
{
	(void)adap; (void)info;
	return NULL;
}

struct i2c_client *i2c_new_scanned_device(struct i2c_adapter *adap,
					  struct i2c_board_info *info,
					  unsigned short const *addr_list,
					  int (*probe)(struct i2c_adapter *adap,
							unsigned short addr))
{
	(void)addr_list; (void)probe;
	return i2c_new_client_device(adap, info);
}

struct i2c_client *i2c_new_dummy_device(struct i2c_adapter *adapter,
					 u16 address)
{
	(void)adapter; (void)address;
	return NULL;
}

struct i2c_client *devm_i2c_new_dummy_device(struct device *dev,
					      struct i2c_adapter *adap,
					      u16 addr)
{
	(void)dev;
	return i2c_new_dummy_device(adap, addr);
}

struct i2c_client *i2c_new_ancillary_device(struct i2c_client *client,
					     const char *name,
					     u16 default_addr)
{
	(void)client; (void)name; (void)default_addr;
	return NULL;
}

void i2c_unregister_device(struct i2c_client *client)
{
	(void)client;
}

struct i2c_adapter *i2c_get_adapter(int nr)
{
	(void)nr;
	return NULL;
}

void i2c_put_adapter(struct i2c_adapter *adap)
{
	(void)adap;
}

unsigned int i2c_adapter_depth(struct i2c_adapter *adapter)
{
	(void)adapter;
	return 0;
}

struct i2c_adapter *i2c_verify_adapter(struct device *dev)
{
	(void)dev;
	return NULL;
}

struct i2c_client *i2c_verify_client(struct device *dev)
{
	(void)dev;
	return NULL;
}

const struct i2c_device_id *i2c_match_id(const struct i2c_device_id *id,
					  const struct i2c_client *client)
{
	(void)id; (void)client;
	return NULL;
}

const void *i2c_get_match_data(const struct i2c_client *client)
{
	(void)client;
	return NULL;
}

int i2c_get_device_id(const struct i2c_client *client,
		      struct i2c_device_identity *id)
{
	(void)client; (void)id;
	return -2; /* -ENOENT */
}

int i2c_slave_register(struct i2c_client *client, i2c_slave_cb_t slave_cb)
{
	(void)client; (void)slave_cb;
	return 0;
}

int i2c_slave_unregister(struct i2c_client *client)
{
	(void)client;
	return 0;
}

int i2c_slave_event(struct i2c_client *client,
		    enum i2c_slave_event event, u8 *val)
{
	(void)client; (void)event; (void)val;
	return 0;
}

const char *i2c_freq_mode_string(u32 bus_freq_hz)
{
	(void)bus_freq_hz;
	return "100 kHz";
}

/* ---- algo/bit helpers used by amdgpu_i2c.c ---- */
int i2c_bit_add_bus(struct i2c_adapter *adap)
{
	return i2c_add_adapter(adap);
}

void i2c_bit_del_bus(struct i2c_adapter *adap)
{
	i2c_del_adapter(adap);
}

/* devm I2C helpers (device.h declares these; full i2c types available here) */
/* devm_i2c_new_device: the linuxu device.h/i2c.h pair cannot both
 * re-declare this (shadow-header quirk, see the comment above).
 * The KMD does not call it; amdgpu_i2c.c only uses the inert
 * add/del/xfer surface.  No definition in the P0 build set. */

void devm_i2c_delete_adapter(struct device *dev, struct i2c_adapter *adap)
{
	(void)dev;
	i2c_del_adapter(adap);
}

void devm_i2c_del_adapter(struct device *dev, struct i2c_adapter *adap)
{
	(void)dev;
	i2c_del_adapter(adap);
}

int devm_i2c_add_numbered_adapter(struct device *dev,
				  struct i2c_adapter *adap, int nr)
{
	(void)dev; (void)nr;
	return i2c_add_adapter(adap);
}

int devm_i2c_register_adapter(struct device *dev, struct i2c_adapter *adap)
{
	(void)dev;
	return i2c_add_adapter(adap);
}

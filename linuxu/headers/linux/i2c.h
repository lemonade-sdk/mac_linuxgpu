/* linuxu: SHIM (third_party/linux/include/linux/i2c.h) - I2C=y but inert on
 * gfx12 (no SMU-over-I2C); API-compatible subset of upstream: struct
 * i2c_adapter (rt_mutex/completion/lock fields kept), i2c_algorithm,
 * i2c_lock_operations, i2c_adapter_quirks, i2c_client/driver/board_info,
 * bit algo data, master + smbus API, add/del adapter. The bit-banged
 * adapter's xfer path (linuxu/src/i2c) drives the amdgpu hardware I2C
 * engines directly; struct i2c_msg/union i2c_smbus_data + I2C_M_* /
 * I2C_FUNC_* come from uapi/linux/i2c.h. */
/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _LINUX_I2C_H
#define _LINUX_I2C_H

#include <linux/device.h>
#include <linux/bits.h>
#include <linux/bitmap.h>
#include <linux/completion.h>
#include <linux/mod_devicetable.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/rtmutex.h>
#include <linux/irqdomain.h>
#include <linux/swab.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/list.h>

#include <uapi/linux/i2c.h>

#ifndef I2C_NAME_SIZE
#define I2C_NAME_SIZE 20
#endif

struct module;
struct device_node;
struct fwnode_handle;
struct software_node;
struct resource;
struct dentry;
struct regulator;

struct i2c_adapter;
struct i2c_adapter_quirks;
struct i2c_client;
struct i2c_driver;
struct i2c_device_identity;
union i2c_smbus_data;
struct i2c_board_info;
enum i2c_slave_event;
typedef int (*i2c_slave_cb_t)(struct i2c_client *client,
			      enum i2c_slave_event event, u8 *val);

/* I2C Frequency Modes */
#define I2C_MAX_STANDARD_MODE_FREQ	100000
#define I2C_MAX_FAST_MODE_FREQ		400000
#define I2C_MAX_FAST_MODE_PLUS_FREQ	1000000
#define I2C_MAX_TURBO_MODE_FREQ		1400000
#define I2C_MAX_HIGH_SPEED_MODE_FREQ	3400000
#define I2C_MAX_ULTRA_FAST_MODE_FREQ	5000000

const char *i2c_freq_mode_string(u32 bus_freq_hz);

int i2c_transfer_buffer_flags(const struct i2c_client *client,
			      char *buf, int count, u16 flags);

static inline int i2c_master_recv(const struct i2c_client *client,
				  char *buf, int count)
{
	return i2c_transfer_buffer_flags(client, buf, count, I2C_M_RD);
}

static inline int i2c_master_send(const struct i2c_client *client,
				  const char *buf, int count)
{
	return i2c_transfer_buffer_flags(client, (char *)buf, count, 0);
}

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);

s32 i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
		   unsigned short flags, char read_write, u8 command,
		   int protocol, union i2c_smbus_data *data);
s32 __i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
		     unsigned short flags, char read_write, u8 command,
		     int protocol, union i2c_smbus_data *data);

u8 i2c_smbus_pec(u8 crc, u8 *p, size_t count);
s32 i2c_smbus_read_byte(const struct i2c_client *client);
s32 i2c_smbus_write_byte(const struct i2c_client *client, u8 value);
s32 i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command);
s32 i2c_smbus_write_byte_data(const struct i2c_client *client,
			      u8 command, u8 value);
s32 i2c_smbus_read_word_data(const struct i2c_client *client, u8 command);
s32 i2c_smbus_write_word_data(const struct i2c_client *client,
			      u8 command, u16 value);

static inline s32
i2c_smbus_read_word_swapped(const struct i2c_client *client, u8 command)
{
	s32 value = i2c_smbus_read_word_data(client, command);

	return (value < 0) ? value : swab16(value);
}

static inline s32
i2c_smbus_write_word_swapped(const struct i2c_client *client,
			     u8 command, u16 value)
{
	return i2c_smbus_write_word_data(client, command, swab16(value));
}

s32 i2c_smbus_read_block_data(const struct i2c_client *client,
			      u8 command, u8 *values);
s32 i2c_smbus_write_block_data(const struct i2c_client *client,
			       u8 command, u8 length, const u8 *values);
s32 i2c_smbus_read_i2c_block_data(const struct i2c_client *client,
				  u8 command, u8 length, u8 *values);
s32 i2c_smbus_write_i2c_block_data(const struct i2c_client *client,
				   u8 command, u8 length, const u8 *values);
s32 i2c_smbus_read_i2c_block_data_or_emulated(const struct i2c_client *client,
					      u8 command, u8 length,
					      u8 *values);
int i2c_get_device_id(const struct i2c_client *client,
		      struct i2c_device_identity *id);
const struct i2c_device_id *i2c_client_get_device_id(const struct i2c_client *client);

struct i2c_device_identity {
	u16 manufacturer_id;
	u16 part_id;
	u8 die_revision;
};

enum i2c_alert_protocol {
	I2C_PROTOCOL_SMBUS_ALERT,
	I2C_PROTOCOL_SMBUS_HOST_NOTIFY,
};

enum i2c_driver_flags {
	I2C_DRV_ACPI_WAIVE_D0_PROBE = BIT(0),
};

struct i2c_driver {
	unsigned int class;

	int (*probe)(struct i2c_client *client);
	void (*remove)(struct i2c_client *client);
	void (*shutdown)(struct i2c_client *client);
	void (*alert)(struct i2c_client *client, enum i2c_alert_protocol protocol,
		      unsigned int data);
	int (*command)(struct i2c_client *client, unsigned int cmd, void *arg);

	struct device_driver driver;
	const struct i2c_device_id *id_table;
	int (*detect)(struct i2c_client *client, struct i2c_board_info *info);
	const unsigned short *address_list;
	struct list_head clients;

	u32 flags;
};
#define to_i2c_driver(d) container_of_const(d, struct i2c_driver, driver)

struct i2c_client {
	unsigned short flags;
#define I2C_CLIENT_PEC		0x04
#define I2C_CLIENT_TEN		0x10
#define I2C_CLIENT_SLAVE	0x20
#define I2C_CLIENT_HOST_NOTIFY	0x40
#define I2C_CLIENT_WAKE		0x80
#define I2C_CLIENT_SCCB		0x9000

	unsigned short addr;
	char name[I2C_NAME_SIZE];
	struct i2c_adapter *adapter;
	struct device dev;
	int init_irq;
	int irq;
	struct list_head detected;
	void *devres_group_id;
	struct dentry *debugfs;
};
#define to_i2c_client(d) container_of(d, struct i2c_client, dev)

struct i2c_adapter *i2c_verify_adapter(struct device *dev);
const struct i2c_device_id *i2c_match_id(const struct i2c_device_id *id,
					 const struct i2c_client *client);
const void *i2c_get_match_data(const struct i2c_client *client);

static inline void *i2c_get_clientdata(const struct i2c_client *client)
{
	return dev_get_drvdata(&client->dev);
}

static inline void i2c_set_clientdata(struct i2c_client *client, void *data)
{
	dev_set_drvdata(&client->dev, data);
}

enum i2c_slave_event {
	I2C_SLAVE_READ_REQUESTED,
	I2C_SLAVE_WRITE_REQUESTED,
	I2C_SLAVE_READ_PROCESSED,
	I2C_SLAVE_WRITE_RECEIVED,
	I2C_SLAVE_STOP,
};

int i2c_slave_register(struct i2c_client *client, i2c_slave_cb_t slave_cb);
int i2c_slave_unregister(struct i2c_client *client);
int i2c_slave_event(struct i2c_client *client,
		    enum i2c_slave_event event, u8 *val);

struct i2c_board_info {
	char		type[I2C_NAME_SIZE];
	unsigned short	flags;
	unsigned short	addr;
	const char	*dev_name;
	void		*platform_data;
	struct fwnode_handle *fwnode;
	const struct software_node *swnode;
	const struct resource *resources;
	unsigned int	num_resources;
	int		irq;
};

#define I2C_BOARD_INFO(dev_type, dev_addr) \
	.type = dev_type, .addr = (dev_addr)

struct i2c_client *
i2c_new_client_device(struct i2c_adapter *adap, struct i2c_board_info const *info);

struct i2c_client *
i2c_new_scanned_device(struct i2c_adapter *adap,
		       struct i2c_board_info *info,
		       unsigned short const *addr_list,
		       int (*probe)(struct i2c_adapter *adap, unsigned short addr));

int i2c_probe_func_quick_read(struct i2c_adapter *adap, unsigned short addr);

struct i2c_client *
i2c_new_dummy_device(struct i2c_adapter *adapter, u16 address);

struct i2c_client *
devm_i2c_new_dummy_device(struct device *dev, struct i2c_adapter *adap, u16 address);

struct i2c_client *
i2c_new_ancillary_device(struct i2c_client *client,
			 const char *name,
			 u16 default_addr);

void i2c_unregister_device(struct i2c_client *client);
struct i2c_client *i2c_verify_client(struct device *dev);

/*
 * struct i2c_algorithm - represent I2C transfer methods
 */
struct i2c_algorithm {
	union {
		int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs,
			    int num);
		int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs,
				   int num);
	};
	union {
		int (*xfer_atomic)(struct i2c_adapter *adap,
				   struct i2c_msg *msgs, int num);
		int (*master_xfer_atomic)(struct i2c_adapter *adap,
					   struct i2c_msg *msgs, int num);
	};
	int (*smbus_xfer)(struct i2c_adapter *adap, u16 addr,
			  unsigned short flags, char read_write,
			  u8 command, int size, union i2c_smbus_data *data);
	int (*smbus_xfer_atomic)(struct i2c_adapter *adap, u16 addr,
				 unsigned short flags, char read_write,
				 u8 command, int size, union i2c_smbus_data *data);
	u32 (*functionality)(struct i2c_adapter *adap);
};

struct i2c_lock_operations {
	void (*lock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	int (*trylock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	void (*unlock_bus)(struct i2c_adapter *adapter, unsigned int flags);
};

struct i2c_timings {
	u32 bus_freq_hz;
	u32 scl_rise_ns;
	u32 scl_fall_ns;
	u32 scl_int_delay_ns;
	u32 sda_fall_ns;
	u32 sda_hold_ns;
	u32 digital_filter_width_ns;
	u32 analog_filter_cutoff_freq_hz;
};

struct i2c_adapter_quirks {
	u64 flags;
	int max_num_msgs;
	u16 max_write_len;
	u16 max_read_len;
	u16 max_comb_1st_msg_len;
	u16 max_comb_2nd_msg_len;
};

/*
 * struct i2c_adapter - represent an I2C bus segment controller
 * (upstream layout; bus_recovery_info/gpiod/pinctrl fields dropped -
 * not used by amdgpu)
 */
struct i2c_adapter {
	struct module *owner;
	unsigned int class;
	const struct i2c_algorithm *algo;
	void *algo_data;

	const struct i2c_lock_operations *lock_ops;
	struct rt_mutex bus_lock;
	struct rt_mutex mux_lock;

	int timeout;
	int retries;
	struct device dev;
	unsigned long locked_flags;
#define I2C_ALF_IS_SUSPENDED		0
#define I2C_ALF_SUSPEND_REPORTED	1

	int nr;
	char name[48];
	struct completion dev_released;

	struct mutex userspace_clients_lock;
	struct list_head userspace_clients;

	const struct i2c_adapter_quirks *quirks;

	struct irq_domain *host_notify_domain;
	struct regulator *bus_regulator;

	struct dentry *debugfs;

	/* 7bit address space */
	DECLARE_BITMAP(addrs_in_instantiation, 1 << 7);
};
#define to_i2c_adapter(d) container_of(d, struct i2c_adapter, dev)

static inline void *i2c_get_adapdata(const struct i2c_adapter *adap)
{
	return dev_get_drvdata(&adap->dev);
}

static inline void i2c_set_adapdata(struct i2c_adapter *adap, void *data)
{
	dev_set_drvdata(&adap->dev, data);
}

int i2c_for_each_dev(void *data, int (*fn)(struct device *dev, void *data));

#define I2C_LOCK_ROOT_ADAPTER BIT(0)
#define I2C_LOCK_SEGMENT      BIT(1)

static inline void
i2c_lock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	adapter->lock_ops->lock_bus(adapter, flags);
}

static inline int
i2c_trylock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	return adapter->lock_ops->trylock_bus(adapter, flags);
}

static inline void
i2c_unlock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	adapter->lock_ops->unlock_bus(adapter, flags);
}

static inline void i2c_mark_adapter_suspended(struct i2c_adapter *adap)
{
	i2c_lock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
	set_bit(I2C_ALF_IS_SUSPENDED, &adap->locked_flags);
	i2c_unlock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
}

static inline void i2c_mark_adapter_resumed(struct i2c_adapter *adap)
{
	i2c_lock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
	clear_bit(I2C_ALF_IS_SUSPENDED, &adap->locked_flags);
	i2c_unlock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
}

#define I2C_CLASS_HWMON		(1<<0)
#define I2C_CLASS_DEPRECATED	(1<<8)

#define I2C_CLIENT_END		0xfffeU

#define I2C_ADDRS(addr, addrs...) \
	((const unsigned short []){ addr, ## addrs, I2C_CLIENT_END })

int i2c_add_adapter(struct i2c_adapter *adap);
int devm_i2c_add_adapter(struct device *dev, struct i2c_adapter *adapter);
void i2c_del_adapter(struct i2c_adapter *adap);
int i2c_add_numbered_adapter(struct i2c_adapter *adap);

int i2c_register_driver(struct module *owner, struct i2c_driver *driver);
void i2c_del_driver(struct i2c_driver *driver);

#define i2c_add_driver(driver) \
	i2c_register_driver(THIS_MODULE, driver)

static inline bool i2c_client_has_driver(struct i2c_client *client)
{
	return !IS_ERR_OR_NULL(client) && client->dev.driver;
}

void i2c_clients_command(struct i2c_adapter *adap,
			 unsigned int cmd, void *arg);

struct i2c_adapter *i2c_get_adapter(int nr);
void i2c_put_adapter(struct i2c_adapter *adap);
unsigned int i2c_adapter_depth(struct i2c_adapter *adapter);

void i2c_parse_fw_timings(struct device *dev, struct i2c_timings *t, bool use_defaults);

static inline u32 i2c_get_functionality(struct i2c_adapter *adap)
{
	return adap->algo->functionality(adap);
}

static inline int i2c_check_functionality(struct i2c_adapter *adap, u32 func)
{
	return (func & i2c_get_functionality(adap)) == func;
}

static inline int i2c_adapter_id(struct i2c_adapter *adap)
{
	return adap->nr;
}

static inline u8 i2c_8bit_addr_from_msg(const struct i2c_msg *msg)
{
	return (msg->addr << 1) | (msg->flags & I2C_M_RD);
}

/*
 * Adapter quirk flags (struct i2c_adapter_quirks.flags) — values from
 * third_party/linux/include/linux/i2c.h (2026 pin).
 */
#define I2C_AQ_COMB			BIT(0)
#define I2C_AQ_COMB_WRITE_FIRST		BIT(1)
#define I2C_AQ_COMB_READ_SECOND		BIT(2)
#define I2C_AQ_COMB_SAME_ADDR		BIT(3)
#define I2C_AQ_COMB_WRITE_THEN_READ	(I2C_AQ_COMB | I2C_AQ_COMB_WRITE_FIRST | \
					 I2C_AQ_COMB_READ_SECOND | I2C_AQ_COMB_SAME_ADDR)
#define I2C_AQ_NO_CLK_STRETCH		BIT(4)
#define I2C_AQ_NO_ZERO_LEN_READ		BIT(5)
#define I2C_AQ_NO_ZERO_LEN_WRITE	BIT(6)
#define I2C_AQ_NO_ZERO_LEN		(I2C_AQ_NO_ZERO_LEN_READ | I2C_AQ_NO_ZERO_LEN_WRITE)
#define I2C_AQ_NO_REP_START		BIT(7)

#endif /* _LINUX_I2C_H */

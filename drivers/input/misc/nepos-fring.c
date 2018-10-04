/*
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/power_supply.h>

#define FRING_INTERRUPT_DEVICE_STATUS 	1
#define FRING_INTERRUPT_BATTERY_STATUS	2

#define FRING_DEVICE_STATUS_HOME_BUTTON	1

#define FRING_REG_READ_ID		0x01
#define FRING_REG_READ_INTERRUPT_STATUS	0x04
#define FRING_REG_READ_DEVICE_STATUS	0x06

struct fring_device_status {
	uint32_t status;
	uint32_t hardwareErrors;
	uint8_t ambientLightValue;
	uint8_t temp0;
	uint8_t temp1;
	uint8_t temp2;
} __attribute__ ((packed));

struct fring_battery_status {
	int8_t chargeCurrent;
	uint8_t level;
	uint8_t temp;
	uint8_t dummy;
	uint16_t status;
	uint16_t remainingCapacity;
	uint16_t averageTimeToEmpty;
	uint16_t averageTimeToFull;
	uint16_t cycleCount;
} __attribute__ ((packed));

struct nepos_fring_data {
	struct input_dev  *input;
	struct i2c_client *client;

#ifdef FRING_BATTERY
	struct power_supply *charger;
	struct power_supply *battery;
	struct power_supply_desc charger_desc;
	struct power_supply_desc battery_desc;

	bool charging;
	int battery_level;
#endif
};

static int nepos_fring_readwrite(struct i2c_client *client,
				 u16 wr_len, u8 *wr_buf,
				 u16 rd_len, u8 *rd_buf)
{
	struct i2c_msg msg[2];
	int ret;

	msg[0].addr  = client->addr;
	msg[0].flags = 0;
	msg[0].len = wr_len;
	msg[0].buf = wr_buf;

	msg[1].addr  = client->addr;
	msg[1].flags = I2C_M_RD | I2C_M_NOSTART;
	msg[1].len = rd_len;
	msg[1].buf = rd_buf;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0)
		return ret;

	if (ret != 2)
		return -EIO;

	return 0;
}

static irqreturn_t nepos_fring_isr(int irq, void *dev_id)
{
	struct nepos_fring_data *fring_data = dev_id;
	u32 irq_status;
	u8 command;
	int home, ret;

	command = FRING_REG_READ_INTERRUPT_STATUS;
	ret = nepos_fring_readwrite(fring_data->client,
				    sizeof(command),    (u8 *) &command,
				    sizeof(irq_status), (u8 *) &irq_status);
	if (WARN_ON(ret < 0))
		return IRQ_HANDLED;

	if (irq_status & FRING_INTERRUPT_DEVICE_STATUS) {
		struct fring_device_status dev_status;

		command = FRING_REG_READ_DEVICE_STATUS;
		ret = nepos_fring_readwrite(fring_data->client,
					    sizeof(command), (u8 *) &command,
					    sizeof(dev_status), (u8 *) &dev_status);
		if (WARN_ON(ret < 0))
			return IRQ_HANDLED;

		home = !!(dev_status.status & FRING_DEVICE_STATUS_HOME_BUTTON);

		dev_dbg(&fring_data->client->dev, "%s() reporting HOME: %d\n", __func__, home);

		input_report_key(fring_data->input, KEY_HOME, home);
		input_report_key(fring_data->input, KEY_WAKEUP, home);
		input_sync(fring_data->input);
	}

#ifdef FRING_BATTERY
	if (irq_status & FRING_INTERRUPT_BATTERY_STATUS) {
		struct fring_battery_status battery_status;

		command = FRING_REG_READ_DEVICE_STATUS;
		ret = nepos_fring_readwrite(fring_data->client,
					    sizeof(command), (u8 *) &command,
					    sizeof(battery_status), (u8 *) &battery_status);
		if (WARN_ON(ret < 0))
			return IRQ_HANDLED;

		fring_data->battery_level = battery_status.level;
		fring_data->charging = battery_status.chargeCurrent > 2;

		power_supply_changed(fring_data->charger);
		power_supply_changed(fring_data->battery);
	}
#endif

	return IRQ_HANDLED;
}

#ifdef FRING_BATTERY
static enum power_supply_property charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property battery_properties[] = {
	POWER_SUPPLY_PROP_CAPACITY,
};

static int fring_psy_get_property(struct power_supply *psy,
		enum power_supply_property psp, union power_supply_propval *val)
{
	struct nepos_fring_data *fring_data = psy->drv_data;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = fring_data->charging;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = fring_data->battery_level;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static char *charger_supplied_to[] = {
	"Battery",
};
#endif

static int nepos_fring_probe(struct i2c_client *client,
			     const struct i2c_device_id *dev_id)
{
	struct nepos_fring_data *fring_data;
#ifdef FRING_BATTERY
	struct power_supply_desc *psy_desc;
	struct power_supply_config psy_cfg = {};
#endif
	struct input_dev *input;
	u8 command, id[5];
	int ret;

	dev_dbg(&client->dev, "Probing Nepos Fring\n");

	command = FRING_REG_READ_ID;
	ret = nepos_fring_readwrite(client,
				    sizeof(command), (u8 *) &command,
				    sizeof(id),      (u8 *) &id);
	if (ret < 0) {
		dev_err(&client->dev, "failed to communicate\n");
		return ret;
	}

	if (id[0] != 'F' ||
	    id[1] != 'r' ||
	    id[2] != 'i' ||
	    id[3] != 'n' ||
	    id[4] != 'g') {
		dev_err(&client->dev, "failed to identify hardware\n");
		return -ENODEV;
	}

	fring_data = devm_kzalloc(&client->dev, sizeof(*fring_data), GFP_KERNEL);
	if (!fring_data) {
		dev_err(&client->dev, "failed to allocate driver data.\n");
		return -ENOMEM;
	}

	input = devm_input_allocate_device(&client->dev);
	if (!input) {
		dev_err(&client->dev, "failed to allocate input device.\n");
		return -ENOMEM;
	}

	input->name = "nepos-fring";
	input->id.bustype = BUS_I2C;
	input->dev.parent = &client->dev;

	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_REP, input->evbit);
	__set_bit(KEY_HOME, input->keybit);
	__set_bit(KEY_WAKEUP, input->keybit);

	fring_data->input = input;
	fring_data->client = client;
	i2c_set_clientdata(client, fring_data);

	ret = devm_request_threaded_irq(&client->dev, client->irq,
					NULL, nepos_fring_isr,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					client->name, fring_data);
	if (ret) {
		dev_err(&client->dev, "Unable to request IRQ.\n");
		return ret;
	}

	ret = input_register_device(input);
	if (ret < 0)
		return ret;

#ifdef FRING_BATTERY
	psy_desc = &fring_data->charger_desc;
	psy_desc->name = "USB-C";
	psy_desc->type = POWER_SUPPLY_TYPE_MAINS;
	psy_desc->properties = charger_properties;
	psy_desc->num_properties = ARRAY_SIZE(charger_properties);
	psy_desc->get_property = fring_psy_get_property;

	psy_cfg.supplied_to = charger_supplied_to;
	psy_cfg.num_supplicants = 1;
	psy_cfg.of_node = client->dev.of_node;
	psy_cfg.drv_data = fring_data;

	fring_data->charger = power_supply_register(&client->dev,
						    psy_desc, &psy_cfg);

	psy_desc = &fring_data->battery_desc;
	psy_desc->name = "Battery";
	psy_desc->type = POWER_SUPPLY_TYPE_BATTERY;
	psy_desc->properties = battery_properties;
	psy_desc->num_properties = ARRAY_SIZE(battery_properties);
	psy_desc->get_property = fring_psy_get_property;

	psy_cfg.num_supplicants = 0;
	psy_cfg.of_node = client->dev.of_node;
	psy_cfg.drv_data = fring_data;

	fring_data->battery = power_supply_register(&client->dev,
						    psy_desc, &psy_cfg);

	if (IS_ERR(fring_data->charger)) {
		ret = PTR_ERR(fring_data->charger);
		dev_err(&client->dev, "Failed to register power supply: %d\n", ret);
		return ret;
	}
#endif

	dev_info(&client->dev, "Fring successfully initialized\n");

	return 0;
}

static int nepos_fring_remove(struct i2c_client *client)
{
	return 0;
}

static int __maybe_unused nepos_fring_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	enable_irq_wake(client->irq);

	return 0;
}

static int __maybe_unused nepos_fring_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	disable_irq_wake(client->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(nepos_fring_pm_ops,
			 nepos_fring_suspend, nepos_fring_resume);

static const struct i2c_device_id nepos_fring_id[] = {
	{ .name = "nepos-fring" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, nepos_fring_id);

#ifdef CONFIG_OF
static const struct of_device_id nepos_fring_of_match[] = {
	{ .compatible = "nepos,fring" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nepos_fring_of_match);
#endif

static struct i2c_driver nepos_fring_driver = {
	.driver = {
		.name = "nepos-fring",
		.of_match_table = of_match_ptr(nepos_fring_of_match),
		.pm = &nepos_fring_pm_ops,
	},
	.id_table = nepos_fring_id,
	.probe    = nepos_fring_probe,
	.remove   = nepos_fring_remove,
};

module_i2c_driver(nepos_fring_driver);

MODULE_AUTHOR("Daniel Mack <daniel@nepos.io>");
MODULE_DESCRIPTION("Nepos Fring wrapper");
MODULE_LICENSE("GPL");

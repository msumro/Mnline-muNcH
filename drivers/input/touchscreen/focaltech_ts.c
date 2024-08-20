// SPDX-License-Identifier: GPL-2.0
/*
 * FocalTech Touchscreen Driver
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>

#define MAX_TOUCH_POINTS 10

struct focaltech_ts {
    struct i2c_client *client;
    struct input_dev *input_dev;
    int irq_gpio;
    int reset_gpio;
    struct regulator *vcc;
    bool suspended;
};

static int focaltech_ts_power_on(struct focaltech_ts *ts)
{
    int ret;

    ret = regulator_enable(ts->vcc);
    if (ret) {
        dev_err(&ts->client->dev, "Failed to enable vcc regulator\n");
        return ret;
    }

    gpio_set_value(ts->reset_gpio, 1);
    msleep(20);
    gpio_set_value(ts->reset_gpio, 0);
    msleep(200);

    return 0;
}

static void focaltech_ts_power_off(struct focaltech_ts *ts)
{
    gpio_set_value(ts->reset_gpio, 1);
    regulator_disable(ts->vcc);
}

static irqreturn_t focaltech_ts_irq_handler(int irq, void *dev_id)
{
    struct focaltech_ts *ts = dev_id;
    int ret;
    u8 buf[32];
    int x, y, touch_points, i;

    ret = i2c_master_recv(ts->client, buf, sizeof(buf));
    if (ret < 0) {
        dev_err(&ts->client->dev, "Failed to read touch data\n");
        return IRQ_HANDLED;
    }

    touch_points = buf[2] & 0x0F;

    input_mt_prepare_frame(ts->input_dev);

    for (i = 0; i < touch_points; i++) {
        x = (buf[3 + 6 * i] & 0x0F) << 8 | buf[4 + 6 * i];
        y = (buf[5 + 6 * i] & 0x0F) << 8 | buf[6 + 6 * i];

        input_mt_slot(ts->input_dev, i);
        input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
        input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
        input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    }

    input_mt_sync_frame(ts->input_dev);
    input_sync(ts->input_dev);

    return IRQ_HANDLED;
}

static int focaltech_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct focaltech_ts *ts;
    int ret;

    ts = devm_kzalloc(&client->dev, sizeof(struct focaltech_ts), GFP_KERNEL);
    if (!ts)
        return -ENOMEM;

    ts->client = client;

    ts->vcc = devm_regulator_get(&client->dev, "vcc");
    if (IS_ERR(ts->vcc)) {
        dev_err(&client->dev, "Failed to get vcc regulator\n");
        return PTR_ERR(ts->vcc);
    }

    ts->reset_gpio = of_get_named_gpio(client->dev.of_node, "reset-gpios", 0);
    if (!gpio_is_valid(ts->reset_gpio)) {
        dev_err(&client->dev, "Invalid reset GPIO\n");
        return -EINVAL;
    }

    ret = devm_gpio_request_one(&client->dev, ts->reset_gpio, GPIO_OUT_INIT_HIGH, "fts_reset");
    if (ret) {
        dev_err(&client->dev, "Failed to request reset GPIO\n");
        return ret;
    }

    ts->irq_gpio = of_get_named_gpio(client->dev.of_node, "interrupts", 0);
    if (!gpio_is_valid(ts->irq_gpio)) {
        dev_err(&client->dev, "Invalid IRQ GPIO\n");
        return -EINVAL;
    }

    ret = devm_gpio_request_one(&client->dev, ts->irq_gpio, GPIO_IN, "fts_irq");
    if (ret) {
        dev_err(&client->dev, "Failed to request IRQ GPIO\n");
        return ret;
    }

    ret = focaltech_ts_power_on(ts);
    if (ret) {
        dev_err(&client->dev, "Failed to power on touchscreen\n");
        return ret;
    }

    ts->input_dev = devm_input_allocate_device(&client->dev);
    if (!ts->input_dev) {
        dev_err(&client->dev, "Failed to allocate input device\n");
        ret = -ENOMEM;
        goto err_power_off;
    }

    ts->input_dev->name = "FocalTech Touchscreen";
    ts->input_dev->id.bustype = BUS_I2C;
    input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, 1080, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, 2400, 0, 0);
    input_mt_init_slots(ts->input_dev, MAX_TOUCH_POINTS, INPUT_MT_DIRECT);

    ret = input_register_device(ts->input_dev);
    if (ret) {
        dev_err(&client->dev, "Failed to register input device\n");
        goto err_power_off;
    }

    client->irq = gpio_to_irq(ts->irq_gpio);
    if (client->irq < 0) {
        dev_err(&client->dev, "Failed to get IRQ number\n");
        ret = client->irq;
        goto err_unregister_input;
    }

    ret = devm_request_threaded_irq(&client->dev, client->irq, NULL, focaltech_ts_irq_handler,
                                    IRQF_ONESHOT | IRQF_TRIGGER_FALLING, "fts_irq", ts);
    if (ret) {
        dev_err(&client->dev, "Failed to request IRQ\n");
        goto err_unregister_input;
    }

    i2c_set_clientdata(client, ts);
    pm_runtime_enable(&client->dev);

    dev_info(&client->dev, "FocalTech touchscreen probed successfully\n");
    return 0;

err_unregister_input:
    input_unregister_device(ts->input_dev);
err_power_off:
    focaltech_ts_power_off(ts);
    return ret;
}

static int focaltech_ts_remove(struct i2c_client *client)
{
    struct focaltech_ts *ts = i2c_get_clientdata(client);

    pm_runtime_disable(&client->dev);
    focaltech_ts_power_off(ts);

    return 0;
}

#ifdef CONFIG_PM_SLEEP
static int focaltech_ts_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct focaltech_ts *ts = i2c_get_clientdata(client);

    focaltech_ts_power_off(ts);

    return 0;
}

static int focaltech_ts_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct focaltech_ts *ts = i2c_get_clientdata(client);

    return focaltech_ts_power_on(ts);
}
#endif

static SIMPLE_DEV_PM_OPS(focaltech_ts_pm_ops, focaltech_ts_suspend, focaltech_ts_resume);

static const struct of_device_id focaltech_of_match[] = {
    { .compatible = "focaltech,fts" },
    { }
};
MODULE_DEVICE_TABLE(of, focaltech_of_match);

static const struct i2c_device_id focaltech_ts_id[] = {
    { "focaltech,fts", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, focaltech_ts_id);

static struct i2c_driver focaltech_ts_driver = {
    .driver = {
        .name = "focaltech_ts",
        .pm = &focaltech_ts_pm_ops,
        .of_match_table = focaltech_of_match,
    },
    .probe = focaltech_ts_probe,
    .remove = focaltech_ts_remove,
    .id_table = focaltech_ts_id,
};
module_i2c_driver(focaltech_ts_driver);

MODULE_DESCRIPTION("FocalTech Touchscreen Driver");
MODULE_AUTHOR(msumro)
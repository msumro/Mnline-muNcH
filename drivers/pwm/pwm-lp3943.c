// SPDX-License-Identifier: GPL-2.0-only
/*
 * TI/National Semiconductor LP3943 PWM driver
 *
 * Copyright 2013 Texas Instruments
 *
 * Author: Milo Kim <milo.kim@ti.com>
 */

#include <linux/err.h>
#include <linux/mfd/lp3943.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>

#define LP3943_MAX_DUTY			255
#define LP3943_MIN_PERIOD		6250
#define LP3943_MAX_PERIOD		1600000

struct lp3943_pwm {
	struct lp3943 *lp3943;
	struct lp3943_platform_data *pdata;
	struct lp3943_pwm_map pwm_map[LP3943_NUM_PWMS];
};

static inline struct lp3943_pwm *to_lp3943_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static struct lp3943_pwm_map *
lp3943_pwm_request_map(struct lp3943_pwm *lp3943_pwm, int hwpwm)
{
	struct lp3943_platform_data *pdata = lp3943_pwm->pdata;
	struct lp3943 *lp3943 = lp3943_pwm->lp3943;
	struct lp3943_pwm_map *pwm_map = &lp3943_pwm->pwm_map[hwpwm];
	int i, offset;

	pwm_map->output = pdata->pwms[hwpwm]->output;
	pwm_map->num_outputs = pdata->pwms[hwpwm]->num_outputs;

	for (i = 0; i < pwm_map->num_outputs; i++) {
		offset = pwm_map->output[i];

		/* Return an error if the pin is already assigned */
		if (test_and_set_bit(offset, &lp3943->pin_used))
			return ERR_PTR(-EBUSY);
	}

	return pwm_map;
}

static int lp3943_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct lp3943_pwm *lp3943_pwm = to_lp3943_pwm(chip);
	struct lp3943_pwm_map *pwm_map;

	pwm_map = lp3943_pwm_request_map(lp3943_pwm, pwm->hwpwm);
	if (IS_ERR(pwm_map))
		return PTR_ERR(pwm_map);

	return 0;
}

static void lp3943_pwm_free_map(struct lp3943_pwm *lp3943_pwm,
				struct lp3943_pwm_map *pwm_map)
{
	struct lp3943 *lp3943 = lp3943_pwm->lp3943;
	int i, offset;

	for (i = 0; i < pwm_map->num_outputs; i++) {
		offset = pwm_map->output[i];
		clear_bit(offset, &lp3943->pin_used);
	}
}

static void lp3943_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct lp3943_pwm *lp3943_pwm = to_lp3943_pwm(chip);
	struct lp3943_pwm_map *pwm_map = &lp3943_pwm->pwm_map[pwm->hwpwm];

	lp3943_pwm_free_map(lp3943_pwm, pwm_map);
}

static int lp3943_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			     u64 duty_ns, u64 period_ns)
{
	struct lp3943_pwm *lp3943_pwm = to_lp3943_pwm(chip);
	struct lp3943 *lp3943 = lp3943_pwm->lp3943;
	u8 val, reg_duty, reg_prescale;
	int err;

	/*
	 * How to configure the LP3943 PWMs
	 *
	 * 1) Period = 6250 ~ 1600000
	 * 2) Prescale = period / 6250 -1
	 * 3) Duty = input duty
	 *
	 * Prescale and duty are register values
	 */

	if (pwm->hwpwm == 0) {
		reg_prescale = LP3943_REG_PRESCALE0;
		reg_duty     = LP3943_REG_PWM0;
	} else {
		reg_prescale = LP3943_REG_PRESCALE1;
		reg_duty     = LP3943_REG_PWM1;
	}

	/*
	 * Note that after this clamping, period_ns fits into an int. This is
	 * helpful because we can resort to integer division below instead of
	 * the (more expensive) 64 bit division.
	 */
	period_ns = clamp(period_ns, (u64)LP3943_MIN_PERIOD, (u64)LP3943_MAX_PERIOD);
	val       = (u8)((int)period_ns / LP3943_MIN_PERIOD - 1);

	err = lp3943_write_byte(lp3943, reg_prescale, val);
	if (err)
		return err;

	duty_ns = min(duty_ns, period_ns);
	val = (u8)((int)duty_ns * LP3943_MAX_DUTY / (int)period_ns);

	return lp3943_write_byte(lp3943, reg_duty, val);
}

static int lp3943_pwm_set_mode(struct lp3943_pwm *lp3943_pwm,
			       struct lp3943_pwm_map *pwm_map,
			       u8 val)
{
	struct lp3943 *lp3943 = lp3943_pwm->lp3943;
	const struct lp3943_reg_cfg *mux = lp3943->mux_cfg;
	int i, index, err;

	for (i = 0; i < pwm_map->num_outputs; i++) {
		index = pwm_map->output[i];
		err = lp3943_update_bits(lp3943, mux[index].reg,
					 mux[index].mask,
					 val << mux[index].shift);
		if (err)
			return err;
	}

	return 0;
}

static int lp3943_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct lp3943_pwm *lp3943_pwm = to_lp3943_pwm(chip);
	struct lp3943_pwm_map *pwm_map = &lp3943_pwm->pwm_map[pwm->hwpwm];
	u8 val;

	if (pwm->hwpwm == 0)
		val = LP3943_DIM_PWM0;
	else
		val = LP3943_DIM_PWM1;

	/*
	 * Each PWM generator is set to control any of outputs of LP3943.
	 * To enable/disable the PWM, these output pins should be configured.
	 */

	return lp3943_pwm_set_mode(lp3943_pwm, pwm_map, val);
}

static void lp3943_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct lp3943_pwm *lp3943_pwm = to_lp3943_pwm(chip);
	struct lp3943_pwm_map *pwm_map = &lp3943_pwm->pwm_map[pwm->hwpwm];

	/*
	 * LP3943 outputs are open-drain, so the pin should be configured
	 * when the PWM is disabled.
	 */

	lp3943_pwm_set_mode(lp3943_pwm, pwm_map, LP3943_GPIO_OUT_HIGH);
}

static int lp3943_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	int err;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled) {
		if (pwm->state.enabled)
			lp3943_pwm_disable(chip, pwm);
		return 0;
	}

	err = lp3943_pwm_config(chip, pwm, state->duty_cycle, state->period);
	if (err)
		return err;

	if (!pwm->state.enabled)
		err = lp3943_pwm_enable(chip, pwm);

	return err;
}

static const struct pwm_ops lp3943_pwm_ops = {
	.request	= lp3943_pwm_request,
	.free		= lp3943_pwm_free,
	.apply		= lp3943_pwm_apply,
};

static int lp3943_pwm_parse_dt(struct device *dev,
			       struct lp3943_pwm *lp3943_pwm)
{
	static const char * const name[] = { "ti,pwm0", "ti,pwm1", };
	struct device_node *node = dev->of_node;
	struct lp3943_platform_data *pdata;
	struct lp3943_pwm_map *pwm_map;
	enum lp3943_pwm_output *output;
	int i, err, count = 0;
	u32 num_outputs;

	if (!node)
		return -EINVAL;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	/*
	 * Read the output map configuration from the device tree.
	 * Each of the two PWM generators can drive zero or more outputs.
	 */

	for (i = 0; i < LP3943_NUM_PWMS; i++) {
		num_outputs = of_property_count_u32_elems(node, name[i]);
		if (num_outputs <= 0)
			continue;

		output = devm_kcalloc(dev, num_outputs, sizeof(*output),
				      GFP_KERNEL);
		if (!output)
			return -ENOMEM;

		err = of_property_read_u32_array(node, name[i], output,
						 num_outputs);
		if (err)
			return err;

		pwm_map = devm_kzalloc(dev, sizeof(*pwm_map), GFP_KERNEL);
		if (!pwm_map)
			return -ENOMEM;

		pwm_map->output = output;
		pwm_map->num_outputs = num_outputs;
		pdata->pwms[i] = pwm_map;

		count++;
	}

	if (count == 0)
		return -ENODATA;

	lp3943_pwm->pdata = pdata;
	return 0;
}

static int lp3943_pwm_probe(struct platform_device *pdev)
{
	struct lp3943 *lp3943 = dev_get_drvdata(pdev->dev.parent);
	struct pwm_chip *chip;
	struct lp3943_pwm *lp3943_pwm;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, LP3943_NUM_PWMS, sizeof(*lp3943_pwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	lp3943_pwm = to_lp3943_pwm(chip);

	lp3943_pwm->pdata = lp3943->pdata;
	if (!lp3943_pwm->pdata) {
		if (IS_ENABLED(CONFIG_OF))
			ret = lp3943_pwm_parse_dt(&pdev->dev, lp3943_pwm);
		else
			ret = -ENODEV;

		if (ret)
			return ret;
	}

	lp3943_pwm->lp3943 = lp3943;
	chip->ops = &lp3943_pwm_ops;

	return devm_pwmchip_add(&pdev->dev, chip);
}

#ifdef CONFIG_OF
static const struct of_device_id lp3943_pwm_of_match[] = {
	{ .compatible = "ti,lp3943-pwm", },
	{ }
};
MODULE_DEVICE_TABLE(of, lp3943_pwm_of_match);
#endif

static struct platform_driver lp3943_pwm_driver = {
	.probe = lp3943_pwm_probe,
	.driver = {
		.name = "lp3943-pwm",
		.of_match_table = of_match_ptr(lp3943_pwm_of_match),
	},
};
module_platform_driver(lp3943_pwm_driver);

MODULE_DESCRIPTION("LP3943 PWM driver");
MODULE_ALIAS("platform:lp3943-pwm");
MODULE_AUTHOR("Milo Kim");
MODULE_LICENSE("GPL");

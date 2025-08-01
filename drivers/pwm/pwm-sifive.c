// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017-2018 SiFive
 * For SiFive's PWM IP block documentation please refer Chapter 14 of
 * Reference Manual : https://static.dev.sifive.com/FU540-C000-v1.0.pdf
 *
 * PWM output inversion: According to the SiFive Reference manual
 * the output of each comparator is high whenever the value of pwms is
 * greater than or equal to the corresponding pwmcmpX[Reference Manual].
 *
 * Figure 29 in the same manual shows that the pwmcmpXcenter bit is
 * hard-tied to 0 (XNOR), which effectively inverts the comparison so that
 * the output goes HIGH when  `pwms < pwmcmpX`.
 *
 * In other words, each pwmcmp register actually defines the **inactive**
 * (low) period of the pulse, not the active time exactly opposite to what
 * the documentation text implies.
 *
 * To compensate, this driver always **inverts** the duty value when reading
 * or writing pwmcmp registers , so that users interact with a conventional
 * **active-high** PWM interface.
 *
 *
 * Limitations:
 * - When changing both duty cycle and period, we cannot prevent in
 *   software that the output might produce a period with mixed
 *   settings (new period length and old duty cycle).
 * - The hardware cannot generate a 0% duty cycle.
 * - The hardware generates only inverted output.
 */
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/bitfield.h>

/* Register offsets */
#define PWM_SIFIVE_PWMCFG		0x0
#define PWM_SIFIVE_PWMCOUNT		0x8
#define PWM_SIFIVE_PWMS			0x10
#define PWM_SIFIVE_PWMCMP(i)		(0x20 + 4 * (i))

/* PWMCFG fields */
#define PWM_SIFIVE_PWMCFG_SCALE		GENMASK(3, 0)
#define PWM_SIFIVE_PWMCFG_STICKY	BIT(8)
#define PWM_SIFIVE_PWMCFG_ZERO_CMP	BIT(9)
#define PWM_SIFIVE_PWMCFG_DEGLITCH	BIT(10)
#define PWM_SIFIVE_PWMCFG_EN_ALWAYS	BIT(12)
#define PWM_SIFIVE_PWMCFG_EN_ONCE	BIT(13)
#define PWM_SIFIVE_PWMCFG_CENTER	BIT(16)
#define PWM_SIFIVE_PWMCFG_GANG		BIT(24)
#define PWM_SIFIVE_PWMCFG_IP		BIT(28)

#define PWM_SIFIVE_CMPWIDTH		16
#define PWM_SIFIVE_DEFAULT_PERIOD	10000000

struct pwm_sifive_ddata {
	struct device *parent;
	struct mutex lock; /* lock to protect user_count and approx_period */
	struct notifier_block notifier;
	struct clk *clk;
	void __iomem *regs;
	unsigned int real_period;
	unsigned int approx_period;
	int user_count;
};

static inline
struct pwm_sifive_ddata *pwm_sifive_chip_to_ddata(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int pwm_sifive_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct pwm_sifive_ddata *ddata = pwm_sifive_chip_to_ddata(chip);

	mutex_lock(&ddata->lock);
	ddata->user_count++;
	mutex_unlock(&ddata->lock);

	return 0;
}

static void pwm_sifive_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct pwm_sifive_ddata *ddata = pwm_sifive_chip_to_ddata(chip);

	mutex_lock(&ddata->lock);
	ddata->user_count--;
	mutex_unlock(&ddata->lock);
}

/* Called holding ddata->lock */
static void pwm_sifive_update_clock(struct pwm_sifive_ddata *ddata,
				    unsigned long rate)
{
	unsigned long long num;
	unsigned long scale_pow;
	int scale;
	u32 val;
	/*
	 * The PWM unit is used with pwmzerocmp=0, so the only way to modify the
	 * period length is using pwmscale which provides the number of bits the
	 * counter is shifted before being feed to the comparators. A period
	 * lasts (1 << (PWM_SIFIVE_CMPWIDTH + pwmscale)) clock ticks.
	 * (1 << (PWM_SIFIVE_CMPWIDTH + scale)) * 10^9/rate = period
	 */
	scale_pow = div64_ul(ddata->approx_period * (u64)rate, NSEC_PER_SEC);
	scale = clamp(ilog2(scale_pow) - PWM_SIFIVE_CMPWIDTH, 0, 0xf);

	val = PWM_SIFIVE_PWMCFG_EN_ALWAYS |
	      FIELD_PREP(PWM_SIFIVE_PWMCFG_SCALE, scale);
	writel(val, ddata->regs + PWM_SIFIVE_PWMCFG);

	/* As scale <= 15 the shift operation cannot overflow. */
	num = (unsigned long long)NSEC_PER_SEC << (PWM_SIFIVE_CMPWIDTH + scale);
	ddata->real_period = DIV_ROUND_UP_ULL(num, rate);
	dev_dbg(ddata->parent,
		"New real_period = %u ns\n", ddata->real_period);
}

static int pwm_sifive_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct pwm_sifive_ddata *ddata = pwm_sifive_chip_to_ddata(chip);
	u32 duty, val, inactive;

	inactive = readl(ddata->regs + PWM_SIFIVE_PWMCMP(pwm->hwpwm));
	/*
	 * PWM hardware uses 'inactive' counts in pwmcmp, so invert to get actual duty.
	 * Here, 'inactive' is the low time and we compute duty as max_count - inactive.
	 */
	duty = (1U << PWM_SIFIVE_CMPWIDTH) - 1 - inactive;

	state->enabled = duty > 0;

	val = readl(ddata->regs + PWM_SIFIVE_PWMCFG);
	if (!(val & PWM_SIFIVE_PWMCFG_EN_ALWAYS))
		state->enabled = false;

	state->period = ddata->real_period;
	state->duty_cycle = DIV_ROUND_UP_ULL((u64)duty * ddata->real_period,
					     (1U << PWM_SIFIVE_CMPWIDTH));
	state->polarity = PWM_POLARITY_NORMAL;

	return 0;
}

static int pwm_sifive_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	struct pwm_sifive_ddata *ddata = pwm_sifive_chip_to_ddata(chip);
	struct pwm_state cur_state;
	unsigned int duty_cycle;
	unsigned long long num;
	bool enabled;
	int ret = 0;
	u64 frac;
	u32 inactive;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	cur_state = pwm->state;
	enabled = cur_state.enabled;

	duty_cycle = state->duty_cycle;
	if (!state->enabled)
		duty_cycle = 0;

	/*
	 * The problem of output producing mixed setting as mentioned at top,
	 * occurs here. To minimize the window for this problem, we are
	 * calculating the register values first and then writing them
	 * consecutively
	 */
	num = (u64)duty_cycle * (1U << PWM_SIFIVE_CMPWIDTH);
	frac = num;
	do_div(frac, state->period);
	/* The hardware cannot generate a 0% duty cycle */
	frac = min(frac, (u64)(1U << PWM_SIFIVE_CMPWIDTH) - 1);
	/* pwmcmp register must be loaded with the inactive(invert the duty) */
	inactive = (1U << PWM_SIFIVE_CMPWIDTH) - 1 - frac;

	mutex_lock(&ddata->lock);
	if (state->period != ddata->approx_period) {
		/*
		 * Don't let a 2nd user change the period underneath the 1st user.
		 * However if ddate->approx_period == 0 this is the first time we set
		 * any period, so let whoever gets here first set the period so other
		 * users who agree on the period won't fail.
		 */
		if (ddata->user_count != 1 && ddata->approx_period) {
			mutex_unlock(&ddata->lock);
			return -EBUSY;
		}
		ddata->approx_period = state->period;
		pwm_sifive_update_clock(ddata, clk_get_rate(ddata->clk));
	}
	mutex_unlock(&ddata->lock);

	/*
	 * If the PWM is enabled the clk is already on. So only enable it
	 * conditionally to have it on exactly once afterwards independent of
	 * the PWM state.
	 */
	if (!enabled) {
		ret = clk_enable(ddata->clk);
		if (ret) {
			dev_err(pwmchip_parent(chip), "Enable clk failed\n");
			return ret;
		}
	}

	writel(inactive, ddata->regs + PWM_SIFIVE_PWMCMP(pwm->hwpwm));

	if (!state->enabled)
		clk_disable(ddata->clk);

	return 0;
}

static const struct pwm_ops pwm_sifive_ops = {
	.request = pwm_sifive_request,
	.free = pwm_sifive_free,
	.get_state = pwm_sifive_get_state,
	.apply = pwm_sifive_apply,
};

static int pwm_sifive_clock_notifier(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct clk_notifier_data *ndata = data;
	struct pwm_sifive_ddata *ddata =
		container_of(nb, struct pwm_sifive_ddata, notifier);

	if (event == POST_RATE_CHANGE) {
		mutex_lock(&ddata->lock);
		pwm_sifive_update_clock(ddata, ndata->new_rate);
		mutex_unlock(&ddata->lock);
	}

	return NOTIFY_OK;
}

static int pwm_sifive_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_sifive_ddata *ddata;
	struct pwm_chip *chip;
	int ret;
	u32 val;
	unsigned int enabled_pwms = 0, enabled_clks = 1;

	chip = devm_pwmchip_alloc(dev, 4, sizeof(*ddata));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	ddata = pwm_sifive_chip_to_ddata(chip);
	ddata->parent = dev;
	mutex_init(&ddata->lock);
	chip->ops = &pwm_sifive_ops;

	ddata->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ddata->regs))
		return PTR_ERR(ddata->regs);

	ddata->clk = devm_clk_get_prepared(dev, NULL);
	if (IS_ERR(ddata->clk))
		return dev_err_probe(dev, PTR_ERR(ddata->clk),
				     "Unable to find controller clock\n");

	ret = clk_enable(ddata->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock for pwm: %d\n", ret);
		return ret;
	}

	val = readl(ddata->regs + PWM_SIFIVE_PWMCFG);
	if (val & PWM_SIFIVE_PWMCFG_EN_ALWAYS) {
		unsigned int i;

		for (i = 0; i < chip->npwm; ++i) {
			val = readl(ddata->regs + PWM_SIFIVE_PWMCMP(i));
			if (val > 0)
				++enabled_pwms;
		}
	}

	/* The clk should be on once for each running PWM. */
	if (enabled_pwms) {
		while (enabled_clks < enabled_pwms) {
			/* This is not expected to fail as the clk is already on */
			ret = clk_enable(ddata->clk);
			if (unlikely(ret)) {
				dev_err_probe(dev, ret, "Failed to enable clk\n");
				goto disable_clk;
			}
			++enabled_clks;
		}
	} else {
		clk_disable(ddata->clk);
		enabled_clks = 0;
	}

	/* Watch for changes to underlying clock frequency */
	ddata->notifier.notifier_call = pwm_sifive_clock_notifier;
	ret = clk_notifier_register(ddata->clk, &ddata->notifier);
	if (ret) {
		dev_err(dev, "failed to register clock notifier: %d\n", ret);
		goto disable_clk;
	}

	ret = pwmchip_add(chip);
	if (ret < 0) {
		dev_err(dev, "cannot register PWM: %d\n", ret);
		goto unregister_clk;
	}

	platform_set_drvdata(pdev, chip);
	dev_dbg(dev, "SiFive PWM chip registered %d PWMs\n", chip->npwm);

	return 0;

unregister_clk:
	clk_notifier_unregister(ddata->clk, &ddata->notifier);
disable_clk:
	while (enabled_clks) {
		clk_disable(ddata->clk);
		--enabled_clks;
	}

	return ret;
}

static void pwm_sifive_remove(struct platform_device *dev)
{
	struct pwm_chip *chip = platform_get_drvdata(dev);
	struct pwm_sifive_ddata *ddata = pwm_sifive_chip_to_ddata(chip);
	struct pwm_device *pwm;
	int ch;

	pwmchip_remove(chip);
	clk_notifier_unregister(ddata->clk, &ddata->notifier);

	for (ch = 0; ch < chip->npwm; ch++) {
		pwm = &chip->pwms[ch];
		if (pwm->state.enabled)
			clk_disable(ddata->clk);
	}
}

static const struct of_device_id pwm_sifive_of_match[] = {
	{ .compatible = "sifive,pwm0" },
	{},
};
MODULE_DEVICE_TABLE(of, pwm_sifive_of_match);

static struct platform_driver pwm_sifive_driver = {
	.probe = pwm_sifive_probe,
	.remove = pwm_sifive_remove,
	.driver = {
		.name = "pwm-sifive",
		.of_match_table = pwm_sifive_of_match,
	},
};
module_platform_driver(pwm_sifive_driver);

MODULE_DESCRIPTION("SiFive PWM driver");
MODULE_LICENSE("GPL v2");

/*
 * Copyright (c) 2020, Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zephyr_native_posix_counter

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>
#include <soc.h>
#include <hw_counter.h>
#include <limits.h>

#define DRIVER_CONFIG_INFO_FLAGS (COUNTER_CONFIG_INFO_COUNT_UP)
#define DRIVER_CONFIG_INFO_CHANNELS 1
#define COUNTER_NATIVE_POSIX_IRQ_FLAGS (0)
#define COUNTER_NATIVE_POSIX_IRQ_PRIORITY (2)

#define COUNTER_PERIOD (USEC_PER_SEC / CONFIG_COUNTER_NATIVE_POSIX_FREQUENCY)
#define TOP_VALUE (UINT_MAX)

static struct counter_alarm_cfg pending_alarm;
static bool is_alarm_pending;
static struct counter_top_cfg top;
static bool is_top_set;
static const struct device *device;
static uint32_t last_top;
static uint32_t next_top;

static void counter_isr(const void *arg)
{
	ARG_UNUSED(arg);
	uint32_t current_value = hw_counter_get_value();

	/* Process `top` first to ensure new target is used in any subsequent
	 * calls to `ctr_set_alarm`.
	 */
	if (is_top_set && (current_value == (last_top + top.ticks))) {
		if (top.callback) {
			top.callback(device, top.user_data);
		}
		last_top = current_value;
		next_top = current_value + top.ticks;

		if (next_top < last_top) {
			hw_counter_set_target(TOP_VALUE);
		} else {
			hw_counter_set_target(next_top);
		}
	}

	if (is_alarm_pending && (current_value == pending_alarm.ticks)) {
		is_alarm_pending = false;
		if (pending_alarm.callback) {
			pending_alarm.callback(device, 0, current_value,
								   pending_alarm.user_data);
		}
	}

	/* Rollover when `TOP_VALUE` is reached, then set the next target */
	if (current_value == TOP_VALUE) {
		hw_counter_reset();

		if (is_alarm_pending && is_top_set) {
			hw_counter_set_target(MIN(pending_alarm.ticks, next_top));
		} else if (is_alarm_pending) {
			hw_counter_set_target(pending_alarm.ticks);
		} else if (is_top_set) {
			hw_counter_set_target(next_top);
		} else {
			hw_counter_set_target(TOP_VALUE);
		}
	}
}

static int ctr_init(const struct device *dev)
{
	device = dev;
	is_alarm_pending = false;
	is_top_set = false;

	IRQ_CONNECT(COUNTER_EVENT_IRQ, COUNTER_NATIVE_POSIX_IRQ_PRIORITY,
		    counter_isr, NULL, COUNTER_NATIVE_POSIX_IRQ_FLAGS);
	hw_counter_set_period(COUNTER_PERIOD);
	hw_counter_set_target(TOP_VALUE);

	return 0;
}

static int ctr_start(const struct device *dev)
{
	ARG_UNUSED(dev);

	hw_counter_start();
	return 0;
}

static int ctr_stop(const struct device *dev)
{
	ARG_UNUSED(dev);

	hw_counter_stop();
	return 0;
}

static int ctr_get_value(const struct device *dev, uint32_t *ticks)
{
	ARG_UNUSED(dev);

	*ticks = hw_counter_get_value();
	return 0;
}

static uint32_t ctr_get_pending_int(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int ctr_set_top_value(const struct device *dev,
			     const struct counter_top_cfg *cfg)
{
	ARG_UNUSED(dev);

	if (is_alarm_pending) {
		posix_print_warning("Can't set top value while alarm is active\n");
		return -EBUSY;
	}

	uint32_t current_value = hw_counter_get_value();

	if (cfg->flags & COUNTER_TOP_CFG_DONT_RESET) {
		if (current_value >= cfg->ticks) {
			if (cfg->flags & COUNTER_TOP_CFG_RESET_WHEN_LATE) {
				hw_counter_reset();
			}
			return -ETIME;
		}
	} else {
		hw_counter_reset();
	}

	top = *cfg;
	last_top = current_value;

	if ((cfg->ticks != TOP_VALUE) && cfg->callback) {
		is_top_set = true;
		hw_counter_set_target(current_value + cfg->ticks);
		irq_enable(COUNTER_EVENT_IRQ);
	} else {
		is_top_set = false;
	}

	return 0;
}

static uint32_t ctr_get_top_value(const struct device *dev)
{
	return top.ticks;
}

static int ctr_set_alarm(const struct device *dev, uint8_t chan_id,
			 const struct counter_alarm_cfg *alarm_cfg)
{
	ARG_UNUSED(dev);

	if (chan_id >= DRIVER_CONFIG_INFO_CHANNELS) {
		posix_print_warning("channel %u is not supported\n", chan_id);
		return -ENOTSUP;
	}

	if (is_alarm_pending)
		return -EBUSY;

	uint32_t ticks = alarm_cfg->ticks;
	uint32_t current_value = hw_counter_get_value();

	if (!(alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE)) {
		ticks += current_value;
	}

	if (is_top_set && ((ticks - current_value) > (top.ticks - current_value))) {
		posix_print_warning("Alarm ticks %u exceed top ticks %u\n", ticks,
				    top.ticks);
		return -EINVAL;
	}

	pending_alarm = *alarm_cfg;
	pending_alarm.ticks = ticks;
	is_alarm_pending = true;

	/* set target to sooner of `ticks` and `TOP_VALUE`, accounting for rollover */
	if ((TOP_VALUE - current_value) < (ticks - current_value)) {
		hw_counter_set_target(TOP_VALUE);
	} else {
		hw_counter_set_target(pending_alarm.ticks);
	}

	irq_enable(COUNTER_EVENT_IRQ);

	return 0;
}

static int ctr_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	ARG_UNUSED(dev);

	if (chan_id >= DRIVER_CONFIG_INFO_CHANNELS) {
		posix_print_warning("channel %u is not supported\n", chan_id);
		return -ENOTSUP;
	}

	if (!hw_counter_is_started()) {
		posix_print_warning("Counter not started\n");
		return -ENOTSUP;
	}

	is_alarm_pending = false;

	return 0;
}

static const struct counter_driver_api ctr_api = {
	.start = ctr_start,
	.stop = ctr_stop,
	.get_value = ctr_get_value,
	.set_alarm = ctr_set_alarm,
	.cancel_alarm = ctr_cancel_alarm,
	.set_top_value = ctr_set_top_value,
	.get_pending_int = ctr_get_pending_int,
	.get_top_value = ctr_get_top_value,
};

static const struct counter_config_info ctr_config = {
	.max_top_value = UINT_MAX,
	.freq = CONFIG_COUNTER_NATIVE_POSIX_FREQUENCY,
	.channels = DRIVER_CONFIG_INFO_CHANNELS,
	.flags = DRIVER_CONFIG_INFO_FLAGS
};

DEVICE_DT_INST_DEFINE(0, ctr_init,
		    NULL, NULL, &ctr_config, PRE_KERNEL_1,
		    CONFIG_COUNTER_INIT_PRIORITY, &ctr_api);

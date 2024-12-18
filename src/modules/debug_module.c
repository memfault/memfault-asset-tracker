/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#if defined(CONFIG_MEMFAULT)
#include <memfault/metrics/metrics.h>
#include <memfault/ports/zephyr/http.h>
#include <memfault/core/data_packetizer.h>
#include <memfault/core/trace_event.h>
#include <memfault/ports/watchdog.h>
#include <memfault/panics/coredump.h>
#include <memfault/metrics/platform/battery.h>
#include <memfault_ncs.h>
#if !defined(CONFIG_ADP536X)
#error "CONFIG_ADP536X must be defined"
#endif
#include <adp536x.h>
#endif

#include <zephyr/shell/shell.h>

#define MODULE debug_module

#if defined(CONFIG_WATCHDOG_APPLICATION)
#include "watchdog_app.h"
#endif /* CONFIG_WATCHDOG_APPLICATION */
#include "modules_common.h"
#include "events/app_module_event.h"
#include "events/cloud_module_event.h"
#include "events/data_module_event.h"
#include "events/sensor_module_event.h"
#include "events/util_module_event.h"
#include "events/modem_module_event.h"
#include "events/ui_module_event.h"
#include "events/debug_module_event.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_DEBUG_MODULE_LOG_LEVEL);

struct debug_msg_data {
	union {
		struct cloud_module_event cloud;
		struct util_module_event util;
		struct ui_module_event ui;
		struct sensor_module_event sensor;
		struct data_module_event data;
		struct app_module_event app;
		struct modem_module_event modem;
	} module;
};

/* Forward declarations. */
static void message_handler(struct debug_msg_data *msg);

#if defined(CONFIG_MEMFAULT)
/* Enumerator used to specify what type of Memfault that is sent. */
static enum memfault_data_type {
	METRICS,
	COREDUMP
} send_type;

static K_SEM_DEFINE(mflt_internal_send_sem, 0, 1);

static void memfault_internal_send(void)
{
entry:
	k_sem_take(&mflt_internal_send_sem, K_FOREVER);

	if (send_type == COREDUMP) {
		if (memfault_coredump_has_valid_coredump(NULL)) {
			LOG_DBG("Sending a coredump to Memfault!");
		} else {
			LOG_DBG("No coredump available.");
			goto entry;
		}
	}

	/* If dispatching Memfault data over an external transport is not enabled,
	 * Memfault SDKs internal HTTP transport is used.
	 */
#if !defined(CONFIG_DEBUG_MODULE_MEMFAULT_USE_EXTERNAL_TRANSPORT)
	int rv = memfault_zephyr_port_post_data();
	if (rv != 0) {
		LOG_ERR("memfault_zephyr_port_post_data, error: %d", rv);
	}
	goto entry;
#else
	uint8_t data[CONFIG_DEBUG_MODULE_MEMFAULT_CHUNK_SIZE_MAX];
	size_t len = sizeof(data);
	uint8_t *message = NULL;

	/* Retrieve, allocate and send parts of the memfault data.
	 * After use it is expected that the data is freed.
	 */
	while (memfault_packetizer_get_chunk(data, &len)) {
		message = k_malloc(len);
		if (message == NULL) {
			LOG_ERR("Failed to allocate memory for Memfault data");
			memfault_metrics_connectivity_record_memfault_sync_failure();
			goto entry;
		}

		memcpy(message, data, len);

		struct debug_module_event *debug_module_event = new_debug_module_event();

		__ASSERT(debug_module_event, "Not enough heap left to allocate event");

		debug_module_event->type = DEBUG_EVT_MEMFAULT_DATA_READY;
		debug_module_event->data.memfault.len = len;
		debug_module_event->data.memfault.buf = message;

		APP_EVENT_SUBMIT(debug_module_event);

		len = sizeof(data);
	}

	// Finished submitting Memfault chunks to the MQTT QoS handler, count this
	// as a Memfault sync success
	memfault_metrics_connectivity_record_memfault_sync_success();
#endif
	goto entry;
}

K_THREAD_DEFINE(mflt_send_thread, CONFIG_DEBUG_MODULE_MEMFAULT_THREAD_STACK_SIZE,
		memfault_internal_send, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

#endif /* if defined(CONFIG_MEMFAULT) */

static struct module_data self = {
	.name = "debug",
	.msg_q = NULL,
	.supports_shutdown = false,
};

/* Handlers */
static bool app_event_handler(const struct app_event_header *aeh)
{
	if (is_modem_module_event(aeh)) {
		struct modem_module_event *event = cast_modem_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.modem = *event
		};

		message_handler(&debug_msg);
	}

	if (is_cloud_module_event(aeh)) {
		struct cloud_module_event *event = cast_cloud_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.cloud = *event
		};

		message_handler(&debug_msg);
	}

	if (is_sensor_module_event(aeh)) {
		struct sensor_module_event *event =
				cast_sensor_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.sensor = *event
		};

		message_handler(&debug_msg);
	}

	if (is_ui_module_event(aeh)) {
		struct ui_module_event *event = cast_ui_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.ui = *event
		};

		message_handler(&debug_msg);
	}

	if (is_app_module_event(aeh)) {
		struct app_module_event *event = cast_app_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.app = *event
		};

		message_handler(&debug_msg);
	}

	if (is_data_module_event(aeh)) {
		struct data_module_event *event = cast_data_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.data = *event
		};

		message_handler(&debug_msg);
	}

	if (is_util_module_event(aeh)) {
		struct util_module_event *event = cast_util_module_event(aeh);
		struct debug_msg_data debug_msg = {
			.module.util = *event
		};

		message_handler(&debug_msg);
	}

	return false;
}

#if defined(CONFIG_MEMFAULT)
#if defined(CONFIG_WATCHDOG_APPLICATION)
static void watchdog_handler(const struct watchdog_evt *evt)
{
	int err;

	switch (evt->type) {
	case WATCHDOG_EVT_START:
		LOG_DBG("WATCHDOG_EVT_START");
		err = memfault_software_watchdog_enable();
		if (err) {
			LOG_ERR("memfault_software_watchdog_enable, error: %d", err);
		}
		break;
	case WATCHDOG_EVT_FEED:
		LOG_DBG("WATCHDOG_EVT_FEED");
		err = memfault_software_watchdog_feed();
		if (err) {
			LOG_ERR("memfault_software_watchdog_feed, error: %d", err);
		}
		break;
	case WATCHDOG_EVT_TIMEOUT_INSTALLED:
		LOG_DBG("WATCHDOG_EVT_TIMEOUT_INSTALLED");
		/* Set the software watchdog timeout slightly shorter than the actual watchdog
		 * timeout. This is to catch a timeout in advance so that Memfault can save
		 * coredump data before a reboot occurs.
		 */
		__ASSERT(evt->timeout > CONFIG_DEBUG_MODULE_MEMFAULT_WATCHDOG_DELTA_MS,
			 "Installed watchdog timeout is too small");

		err = memfault_software_watchdog_update_timeout(
						evt->timeout -
						CONFIG_DEBUG_MODULE_MEMFAULT_WATCHDOG_DELTA_MS);
		if (err) {
			LOG_ERR("memfault_software_watchdog_update_timeout, error: %d", err);
		}
		break;
	default:
		break;
	}
}
#endif /* defined(CONFIG_WATCHDOG_APPLICATION) */

/**
 * @brief Send Memfault data. To transfer Memfault data using an internal transport,
 *	  CONFIG_DEBUG_MODULE_MEMFAULT_USE_EXTERNAL_TRANSPORT must be selected.
 *	  Dispatching of Memfault data is offloaded to a dedicated thread.
 */
static void send_memfault_data(void)
{
	/* Offload sending of Memfault data to a dedicated thread. */
	if (memfault_packetizer_data_available()) {
		k_sem_give(&mflt_internal_send_sem);
	}
}

static void memfault_handle_event(struct debug_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_START)) {
		/* Register callback for watchdog events. Used to attach Memfault software
		 * watchdog.
		 */
#if defined(CONFIG_WATCHDOG_APPLICATION)
		watchdog_register_handler(watchdog_handler);
#endif
	}

	/* Send Memfault data at the same time application data is sent to save overhead
	 * compared to having Memfault SDK trigger regular updates independently. All data
	 * should preferably be sent within the same LTE RRC connected window.
	 */
	if ((IS_EVENT(msg, data, DATA_EVT_DATA_SEND)) ||
	    (IS_EVENT(msg, data, DATA_EVT_DATA_SEND_BATCH)) ||
	    (IS_EVENT(msg, data, DATA_EVT_CLOUD_LOCATION_DATA_SEND)) ||
	    (IS_EVENT(msg, data, DATA_EVT_UI_DATA_SEND))) {
		/* Limit how often non-coredump memfault data (events and metrics) are sent
		 * to memfault. Updates can never occur more often than the interval set by
		 * CONFIG_DEBUG_MODULE_MEMFAULT_UPDATES_MIN_INTERVAL_SEC and the first update is
		 * always sent.
		 */
		static int64_t last_update;

		if (((k_uptime_get() - last_update) <
		     (MSEC_PER_SEC * CONFIG_DEBUG_MODULE_MEMFAULT_UPDATES_MIN_INTERVAL_SEC)) &&
		    (last_update != 0)) {
			LOG_DBG("Not enough time has passed since the last Memfault update, abort");
			return;
		}

		last_update = k_uptime_get();
		send_type = METRICS;
		send_memfault_data();
		return;
	}

	/* If the module is configured to use Memfaults internal HTTP transport, coredumps are
	 * sent on an established connection to LTE.
	 */
	if (IS_EVENT(msg, modem, MODEM_EVT_LTE_CONNECTED) &&
	    !IS_ENABLED(CONFIG_DEBUG_MODULE_MEMFAULT_USE_EXTERNAL_TRANSPORT)) {
		/* Send coredump on LTE CONNECTED. */
		send_type = COREDUMP;
		send_memfault_data();
		return;
	}

	/* If the module is configured to use an external cloud transport, coredumps are
	 * sent on an established connection to the configured cloud service.
	 */
	if (IS_EVENT(msg, cloud, CLOUD_EVT_CONNECTED) &&
	    IS_ENABLED(CONFIG_DEBUG_MODULE_MEMFAULT_USE_EXTERNAL_TRANSPORT)) {
		/* Send coredump on CLOUD CONNECTED. */
		send_type = COREDUMP;
		send_memfault_data();
		return;
	}

}

/* 1 means discharging, 0 means charging, -1 means error */
static int prv_adp536x_is_discharging(void)
{
	uint8_t status;
	int err = adp536x_charger_status_1_read(&status);
	if (err) {
		LOG_ERR("Failed to get charger status: %d", err);
		return -1;
	}
	/*
		bits [2:0] are CHARGER_STATUS states:
		Charger Status Bus. The following values are indications for the charger status:
		000 = off.
		001 = trickle charge.
		010 = fast charge (constant current mode).
		011 = fast charge (constant voltage mode).
		100 = charge complete.
		101 = LDO mode.
		110 = trickle or fast charge timer expired.
		111 = battery detection.

		Only 0b000 means the battery is connected and discharging.
	*/
	return (status & 0x7) == 0 ? 1 : 0;
}

int memfault_platform_get_stateofcharge(sMfltPlatformBatterySoc *soc)
{
	int err;
	uint8_t percentage;

	err = adp536x_fg_soc(&percentage);
	if (err) {
		LOG_ERR("Failed to get battery level: %d", err);
		return -1;
	}

	const int discharging = prv_adp536x_is_discharging();

	// failed to retrieve charging status, return error
	if (discharging < 0) {
		return -1;
	}

	*soc = (sMfltPlatformBatterySoc){
		.soc = percentage,
		.discharging = discharging,
	};
	return 0;
}
#endif /* defined(CONFIG_MEMFAULT) */

static void message_handler(struct debug_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_START)) {
		int err = module_start(&self);

		if (err) {
			LOG_ERR("Failed starting module, error: %d", err);
			SEND_ERROR(debug, DEBUG_EVT_ERROR, err);
		}

		/* Notify the rest of the application that it is connected to network
		 * when building for PC.
		 */
		if (IS_ENABLED(CONFIG_BOARD_NATIVE_SIM)) {
			{ SEND_EVENT(debug, DEBUG_EVT_EMULATOR_INITIALIZED); }
			SEND_EVENT(debug, DEBUG_EVT_EMULATOR_NETWORK_CONNECTED);
		}
	}

#if defined(CONFIG_MEMFAULT)
	memfault_handle_event(msg);
#endif
}

void memfault_metrics_heartbeat_collect_data(void)
{
	int err;

	uint16_t millivolts;
	err = adp536x_fg_volts(&millivolts);
	if (err == 0) {
		memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(battery_voltage_mv),
							millivolts);
	} else {
		LOG_ERR("Failed to get battery voltage: %d", err);
	}

	/* Standard NCS metrics */
	memfault_ncs_metrics_collect_data();
}

static int prv_batt_cmd(const struct shell *shell, size_t argc, char **argv) {
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const int discharging = prv_adp536x_is_discharging();
	uint8_t percentage;
	uint16_t millivolts;

	if (adp536x_fg_soc(&percentage) || adp536x_fg_volts(&millivolts) ) {
		shell_print(shell, "Failed to get battery info");
		return -1;
	}

	shell_print(shell, "Battery: %d%%, %d mV, %s",
		percentage, millivolts, discharging ? "discharging" : "charging");

	return 0;
}

SHELL_CMD_REGISTER(batt, NULL, "Print battery info", prv_batt_cmd);

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, app_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, modem_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, cloud_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, ui_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, sensor_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, data_module_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, util_module_event);

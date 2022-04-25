/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 *  SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <errno.h>
#include <zephyr/zephyr.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/direction.h>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <stdbool.h>

//#include <sys/printk.h>
//#include <sys/byteorder.h>
//#include <sys/util.h>

//#include <bluetooth/bluetooth.h>
//#include <bluetooth/gap.h>
//#include <bluetooth/direction.h>
#include <zephyr/bluetooth/hci.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define NAME_LEN        30
#define PEER_NAME_LEN_MAX 30
/* BT Core 5.3 Vol 6, Part B section 4.4.5.1 Periodic Advertising Trains allows controller to wait
 * 6 periodic advertising events for synchronization establishment, hence timeout must be longer
 * than that.
 */
#define SYNC_CREATE_TIMEOUT_INTERVAL_NUM 7
/* Maximum length of advertising data represented in hexadecimal format */
#define ADV_DATA_HEX_STR_LEN_MAX (BT_GAP_ADV_MAX_EXT_ADV_DATA_LEN * 2 + 1)

static struct bt_le_per_adv_sync_param sync_create_param;
static struct bt_le_per_adv_sync *sync;
static bt_addr_le_t per_addr;
static bool per_adv_found;
static bool scan_enabled;
static bool sync_wait;
static bool sync_terminated;
static uint8_t per_sid;
static uint32_t sync_create_timeout_ms;

static K_SEM_DEFINE(sem_per_adv, 0, 1);
static K_SEM_DEFINE(sem_per_sync, 0, 1);
static K_SEM_DEFINE(sem_per_sync_lost, 0, 1);

#if defined(CONFIG_BT_DF_CTE_RX_AOA)
//const static uint8_t ant_patterns[] = { 0x1, 0x2, 0x3, 0x4, 0x5,
//					0x6, 0x7, 0x8, 0x9, 0xA };
//const static uint8_t ant_patterns[] = { 0x8, 0x8, 0x8, 0x8, 0x8 }; // A0, A0, A0, A0
//const static uint8_t ant_patterns[] = { 0x8, 0x8, 0xd, 0x8, 0xd }; // A6, A7, A6, A7
//const static uint8_t ant_patterns[] = { 0x8, 0x8, 0xd, 0x8, 0x8 }; // A6, A7, A6, A6
//const static uint8_t ant_patterns[] = { 0x8, 0x8, 0x8, 0xd, 0x8 }; // A6, A6, A7, A6
//const static uint8_t ant_patterns[] = { 0x8, 0x8, 0xd, 0x8, 0xa }; // A6, A7, A6, A5
//const static uint8_t ant_patterns[] = { 0x8, 0x8, 0xd, 0x8, 0xe }; // A6, A7, A6, A8
//const static uint8_t ant_patterns[] = { 0x1, 0x2, 0x3, 0x4, 0x5,
//					0x6, 0x7, 0x8, 0x9, 0xA };
//const static uint8_t ant_patterns[] = { 0x6, 0x6, 0x4, 0x6, 0x4 }; // A2, A3, A2, A3
const static uint8_t ant_patterns[] = { 0x6, 0x4, 0x6, 0x6, 0x6, 0x5, 0x6, 0x6, 0x6 }; // A2 A3 A2 A2 A2 A1 A2 A2 A2
#endif /* CONFIG_BT_DF_CTE_RX_AOA */

static bool data_cb(struct bt_data *data, void *user_data);
static void create_sync(void);
static void scan_recv(const struct bt_le_scan_recv_info *info,
		      struct net_buf_simple *buf);

static void sync_cb(struct bt_le_per_adv_sync *sync,
		    struct bt_le_per_adv_sync_synced_info *info);
static void term_cb(struct bt_le_per_adv_sync *sync,
		    const struct bt_le_per_adv_sync_term_info *info);
static void recv_cb(struct bt_le_per_adv_sync *sync,
		    const struct bt_le_per_adv_sync_recv_info *info,
		    struct net_buf_simple *buf);
static void scan_disable(void);
static void cte_recv_cb(struct bt_le_per_adv_sync *sync,
			struct bt_df_per_adv_sync_iq_samples_report const *report);

static struct bt_le_per_adv_sync_cb sync_callbacks = {
	.synced = sync_cb,
	.term = term_cb,
	.recv = recv_cb,
	.cte_report_cb = cte_recv_cb,
};

static struct bt_le_scan_cb scan_callbacks = {
	.recv = scan_recv,
};

static uint32_t sync_create_timeout_get(uint16_t interval)
{
	return BT_GAP_PER_ADV_INTERVAL_TO_MS(interval) * SYNC_CREATE_TIMEOUT_INTERVAL_NUM;
}

static const char *phy2str(uint8_t phy)
{
	switch (phy) {
	case 0: return "No packets";
	case BT_GAP_LE_PHY_1M: return "LE 1M";
	case BT_GAP_LE_PHY_2M: return "LE 2M";
	case BT_GAP_LE_PHY_CODED: return "LE Coded";
	default: return "Unknown";
	}
}

static const char *cte_type2str(uint8_t type)
{
	switch (type) {
	case BT_DF_CTE_TYPE_AOA: return "AOA";
	case BT_DF_CTE_TYPE_AOD_1US: return "AOD 1 [us]";
	case BT_DF_CTE_TYPE_AOD_2US: return "AOD 2 [us]";
	case BT_DF_CTE_TYPE_NONE: return "";
	default: return "Unknown";
	}
}

static const char *pocket_status2str(uint8_t status)
{
	switch (status) {
	case BT_DF_CTE_CRC_OK: return "CRC OK";
	case BT_DF_CTE_CRC_ERR_CTE_BASED_TIME: return "CRC not OK, CTE Info OK";
	case BT_DF_CTE_CRC_ERR_CTE_BASED_OTHER: return "CRC not OK, Sampled other way";
	case BT_DF_CTE_INSUFFICIENT_RESOURCES: return "No resources";
	default: return "Unknown";
	}
}

static bool data_cb(struct bt_data *data, void *user_data)
{
	char *name = user_data;
	uint8_t len;

	switch (data->type) {
	case BT_DATA_NAME_SHORTENED:
	case BT_DATA_NAME_COMPLETE:
		len = MIN(data->data_len, NAME_LEN - 1);
		memcpy(name, data->data, len);
		name[len] = '\0';
		return false;
	default:
		return true;
	}
}

static void sync_cb(struct bt_le_per_adv_sync *sync,
		    struct bt_le_per_adv_sync_synced_info *info)
{
	char le_addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));

	printk("PER_ADV_SYNC[%u]: [DEVICE]: %s synced, "
	       "Interval 0x%04x (%u ms), PHY %s\n",
	       bt_le_per_adv_sync_get_index(sync), le_addr,
	       info->interval, info->interval * 5 / 4, phy2str(info->phy));

	k_sem_give(&sem_per_sync);
}

static void term_cb(struct bt_le_per_adv_sync *sync,
		    const struct bt_le_per_adv_sync_term_info *info)
{
	char le_addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));

	printk("PER_ADV_SYNC[%u]: [DEVICE]: %s sync terminated\n",
	       bt_le_per_adv_sync_get_index(sync), le_addr);

	if (sync_wait) {
		sync_terminated = true;
		k_sem_give(&sem_per_sync);
	} else {
		k_sem_give(&sem_per_sync_lost);
	}
}

static void recv_cb(struct bt_le_per_adv_sync *sync,
		    const struct bt_le_per_adv_sync_recv_info *info,
		    struct net_buf_simple *buf)
{
	static char data_str[ADV_DATA_HEX_STR_LEN_MAX];
	char le_addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));
	bin2hex(buf->data, buf->len, data_str, sizeof(data_str));

	printk("PER_ADV_SYNC[%u]: [DEVICE]: %s, tx_power %i, "
	       "RSSI %i, CTE %s, data length %u, data: %s\n",
	       bt_le_per_adv_sync_get_index(sync), le_addr, info->tx_power,
	       info->rssi, cte_type2str(info->cte_type), buf->len, data_str);
}

#define SCAN_WATCHDOG 1
#if SCAN_WATCHDOG
// from zephyr/samples/drivers/watchdog
/*
 * To use this sample, either the devicetree's /aliases must have a
 * 'watchdog0' property, or one of the following watchdog compatibles
 * must have an enabled node.
 */
#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
#define WDT_NODE DT_ALIAS(watchdog0)
#elif DT_HAS_COMPAT_STATUS_OKAY(st_stm32_window_watchdog)
#define WDT_NODE DT_INST(0, st_stm32_window_watchdog)
#define WDT_MAX_WINDOW  100U
#elif DT_HAS_COMPAT_STATUS_OKAY(st_stm32_watchdog)
#define WDT_NODE DT_INST(0, st_stm32_watchdog)
#elif DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_wdt)
/* Nordic supports a callback, but it has 61.2 us to complete before
 * the reset occurs, which is too short for this sample to do anything
 * useful.  Explicitly disallow use of the callback.
 */
#define WDT_ALLOW_CALLBACK 0
#define WDT_NODE DT_INST(0, nordic_nrf_wdt)
#elif DT_HAS_COMPAT_STATUS_OKAY(espressif_esp32_watchdog)
#define WDT_NODE DT_INST(0, espressif_esp32_watchdog)
#elif DT_HAS_COMPAT_STATUS_OKAY(silabs_gecko_wdog)
#define WDT_NODE DT_INST(0, silabs_gecko_wdog)
#elif DT_HAS_COMPAT_STATUS_OKAY(nxp_kinetis_wdog32)
#define WDT_NODE DT_INST(0, nxp_kinetis_wdog32)
#elif DT_HAS_COMPAT_STATUS_OKAY(microchip_xec_watchdog)
#define WDT_NODE DT_INST(0, microchip_xec_watchdog)
#elif DT_HAS_COMPAT_STATUS_OKAY(ti_cc32xx_watchdog)
#define WDT_NODE DT_INST(0, ti_cc32xx_watchdog)
#elif DT_HAS_COMPAT_STATUS_OKAY(nxp_imx_wdog)
#define WDT_NODE DT_INST(0, nxp_imx_wdog)
#endif

#define WDT_MAX_WINDOW 8000U  // milliseconds
#define WDT_FEED_TRIES 5
#define WDT_ALLOW_CALLBACK 1
/*
 * If the devicetree has a watchdog node, get its label property.
 */
#ifdef WDT_NODE
#define WDT_DEV_NAME DT_LABEL(WDT_NODE)
#else
#define WDT_DEV_NAME ""
#error "Unsupported SoC and no watchdog0 alias in zephyr.dts"
#endif

static int wdt_channel_id;
static const struct device *wdt;

#if WDT_ALLOW_CALLBACK
static void wdt_callback(const struct device *wdt_dev, int channel_id)
{
    static bool handled_event;

    if (handled_event) {
        return;
    }

    wdt_feed(wdt_dev, channel_id);

    printk("Handled things..ready to reset\n");
    handled_event = true;
}
#endif /* WDT_ALLOW_CALLBACK */

void wdt_init(void)
{
    int err;

    printk("Watchdog init...\n");

    wdt = device_get_binding(WDT_DEV_NAME);
    if (!wdt) {
        printk("Cannot get WDT device\n");
        return;
    }

    struct wdt_timeout_cfg wdt_config = {
        /* Reset SoC when watchdog timer expires. */
        .flags = WDT_FLAG_RESET_SOC,

        /* Expire watchdog after max window */
        .window.min = 0U,
        .window.max = WDT_MAX_WINDOW,

        .callback = NULL
    };

#if WDT_ALLOW_CALLBACK
    /* Set up watchdog callback. */
    wdt_config.callback = wdt_callback;

    printk("Attempting to test pre-reset callback\n");
#else /* WDT_ALLOW_CALLBACK */
    printk("Callback in RESET_SOC disabled for this platform\n");
#endif /* WDT_ALLOW_CALLBACK */

    wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
    if (wdt_channel_id == -ENOTSUP) {
        /* IWDG driver for STM32 doesn't support callback */
        printk("Callback support rejected, continuing anyway\n");
        wdt_config.callback = NULL;
        wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
    }
    if (wdt_channel_id < 0) {
        printk("Watchdog install error\n");
        return;
    }

    err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (err < 0) {
        printk("Watchdog setup error\n");
        return;
    }

    /* Feeding watchdog. */
    printk("Feeding watchdog %d times\n", WDT_FEED_TRIES);
    for (int i = 0; i < WDT_FEED_TRIES; ++i) {
        printk("Feeding watchdog...\n");
        wdt_feed(wdt, wdt_channel_id);
        k_sleep(K_MSEC(50));
    }
}
#endif


static int lol = 0;
static void cte_recv_cb(struct bt_le_per_adv_sync* sync,
        struct bt_df_per_adv_sync_iq_samples_report const* report)
{
    if(report->packet_status) {
        return;
    }

#if SCAN_WATCHDOG
    /* Feeding watchdog. */
    if (wdt != NULL)
        wdt_feed(wdt, wdt_channel_id);
#endif 
    lol = 1;
//    printk("CTE[%u]: samples count %d, cte type %s, slot durations: %u [us], "
//           "packet status %s, RSSI %i\n",
//           bt_le_per_adv_sync_get_index(sync), report->sample_count,
//           cte_type2str(report->cte_type), report->slot_durations,
//           pocket_status2str(report->packet_status), report->rssi);
//    printk("CTE[%u]:\n"
//           "chan_idx = %d\n"
//           "rssi = %d\n"
//           "rssi_ant_id = %d\n"
//           "cte_type = %s (%d)\n"
//           "slot_durations = %d [us]\n"
//           "packet_status = %s (%d)\n"
//           "sample_count = %d\n", bt_le_per_adv_sync_get_index(sync),
//           report->chan_idx, report->rssi, report->rssi_ant_id,
//           cte_type2str(report->cte_type), report->cte_type,
//           report->slot_durations, pocket_status2str(report->packet_status), report->packet_status, report->sample_count);
//
//    printk("\nIQ samples : \n");
//    /* Reference period */
//    for (int i = 0; i < 8 && i < report->sample_count; i++) {
//        printk("%d %d \n", report->sample[i].i, report->sample[i].q);
//    }
//    /* Sample period */
//    for (int i = 8; i < report->sample_count; i++) {
//        if (i % 4 == 1) {
//            printk("x x ");
//        }
//        printk("%d %d \n", report->sample[i].i, report->sample[i].q);
////        if (i & 0x1) {
////            printk("\n");
////        }
//    }

//    for (int i = 8; i < report->sample_count; i++) {
//        printk("%d %d ", report->sample[i].i, report->sample[i].q);
//    }

    printk("\nIQ samples : \n");
    printk("%d %d %d %d %d %d %d", report->chan_idx, report->rssi,
           report->rssi_ant_id, report->cte_type,
           report->slot_durations, report->packet_status, report->sample_count);

    for (int i = 0; i < report->sample_count; i++) {
        printk(" %d %d", report->sample[i].i, report->sample[i].q);
    }

    printk("\nIQ samples END\n\n");
}

static void scan_recv(const struct bt_le_scan_recv_info *info,
		      struct net_buf_simple *buf)
{
	char le_addr[BT_ADDR_LE_STR_LEN];
	char name[NAME_LEN];

	(void)memset(name, 0, sizeof(name));

	bt_data_parse(buf, data_cb, name);

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));

//	printk("[DEVICE]: %s, AD evt type %u, Tx Pwr: %i, RSSI %i %s C:%u S:%u "
//	       "D:%u SR:%u E:%u Prim: %s, Secn: %s, Interval: 0x%04x (%u ms), "
//	       "SID: %u\n",
//	       le_addr, info->adv_type, info->tx_power, info->rssi, name,
//	       (info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE) != 0,
//	       (info->adv_props & BT_GAP_ADV_PROP_SCANNABLE) != 0,
//	       (info->adv_props & BT_GAP_ADV_PROP_DIRECTED) != 0,
//	       (info->adv_props & BT_GAP_ADV_PROP_SCAN_RESPONSE) != 0,
//	       (info->adv_props & BT_GAP_ADV_PROP_EXT_ADV) != 0,
//	       phy2str(info->primary_phy), phy2str(info->secondary_phy),
//	       info->interval, info->interval * 5 / 4, info->sid);

	if (!per_adv_found && info->interval != 0) {
		sync_create_timeout_ms = sync_create_timeout_get(info->interval);
		per_adv_found = true;
		per_sid = info->sid;
		bt_addr_le_copy(&per_addr, info->addr);

		k_sem_give(&sem_per_adv);
	}
}

static void create_sync(void)
{
	int err;

	printk("Creating Periodic Advertising Sync...");
	bt_addr_le_copy(&sync_create_param.addr, &per_addr);

	sync_create_param.options = BT_LE_PER_ADV_SYNC_OPT_SYNC_ONLY_CONST_TONE_EXT;//| BT_LE_PER_ADV_SYNC_OPT_DONT_SYNC_AOA | BT_LE_PER_ADV_SYNC_OPT_FILTER_DUPLICATE
	sync_create_param.sid = per_sid;
	sync_create_param.skip = 0;
	sync_create_param.timeout = 0xa;
	err = bt_le_per_adv_sync_create(&sync_create_param, &sync);
	if (err != 0) {
		printk("failed (err %d)\n", err);
		return;
	}
	printk("success.\n");
}

static int delete_sync(void)
{
	int err;

	printk("Deleting Periodic Advertising Sync...");
	err = bt_le_per_adv_sync_delete(sync);
	if (err != 0) {
		printk("failed (err %d)\n", err);
		return err;
	}
	printk("success\n");

	return 0;
}

static void enable_cte_rx(void)
{
	int err;

	const struct bt_df_per_adv_sync_cte_rx_param cte_rx_params = {
		.max_cte_count = 5,
#if defined(CONFIG_BT_DF_CTE_RX_AOA)
		.cte_types = BT_DF_CTE_TYPE_ALL,
		.slot_durations = 0x2,
		.num_ant_ids = ARRAY_SIZE(ant_patterns),
		.ant_ids = ant_patterns,
#else
		.cte_types = BT_DF_CTE_TYPE_AOD_1US | BT_DF_CTE_TYPE_AOD_2US,
#endif /* CONFIG_BT_DF_CTE_RX_AOA */
	};

	printk("Enable receiving of CTE...\n");
	err = bt_df_per_adv_sync_cte_rx_enable(sync, &cte_rx_params);
	if (err != 0) {
		printk("failed (err %d)\n", err);
		return;
	}
	printk("success. CTE receive enabled.\n");
}

static int scan_init(void)
{
	printk("Scan callbacks register...");
	bt_le_scan_cb_register(&scan_callbacks);
	printk("success.\n");

	printk("Periodic Advertising callbacks register...");
	bt_le_per_adv_sync_cb_register(&sync_callbacks);
	printk("success.\n");

	return 0;
}

static int scan_enable(void)
{
	struct bt_le_scan_param param = {
			.type       = BT_LE_SCAN_TYPE_ACTIVE,
			.options    = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
			.interval   = BT_GAP_SCAN_FAST_INTERVAL,
			.window     = BT_GAP_SCAN_FAST_WINDOW,
			.timeout    = 0U, };
	int err;

	if (!scan_enabled) {
		printk("Start scanning...");
		err = bt_le_scan_start(&param, NULL);
		if (err != 0) {
			printk("failed (err %d)\n", err);
			return err;
		}
		printk("success\n");
		scan_enabled = true;
	}

	return 0;
}

static void scan_disable(void)
{
	int err;

	printk("Scan disable...");
	err = bt_le_scan_stop();
	if (err != 0) {
		printk("failed (err %d)\n", err);
		return;
	}
	printk("Success.\n");

	scan_enabled = false;
}

void main(void)
{
	int err;

	printk("Starting Connectionless Locator Demo\n");

	printk("Bluetooth initialization...");
	err = bt_enable(NULL);
	if (err != 0) {
		printk("failed (err %d)\n", err);
	}
	printk("success\n");

	scan_init();

	scan_enabled = false;

#if SCAN_WATCHDOG
    wdt_init();
#endif

	do {
		scan_enable();

		printk("Waiting for periodic advertising...");
		per_adv_found = false;
		err = k_sem_take(&sem_per_adv, K_FOREVER);
		if (err != 0) {
			printk("failed (err %d)\n", err);
			return;
		}
		printk("success. Found periodic advertising.\n");

		sync_wait = true;
		sync_terminated = false;

		create_sync();

		printk("Waiting for periodic sync...\n");
		err = k_sem_take(&sem_per_sync, K_MSEC(sync_create_timeout_ms));
		if (err != 0 || sync_terminated) {
			if (err != 0) {
				printk("failed (err %d)\n", err);
			} else {
				printk("terminated\n");
			}

			sync_wait = false;

			err = delete_sync();
			if (err != 0) {
				return;
			}

			continue;
		}
		printk("success. Periodic sync established.\n");
		sync_wait = false;

		enable_cte_rx();

		/* Disable scan to cleanup output */
		scan_disable();

		printk("Waiting for periodic sync lost...\n");
		err = k_sem_take(&sem_per_sync_lost, K_FOREVER);
		if (err != 0) {
			printk("failed (err %d)\n", err);
			return;
		}
		printk("Periodic sync lost.\n");
	} while (true);
}

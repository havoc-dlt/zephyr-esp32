/*
 * Copyright (c) 2022-2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(CONFIG_BT_CAP_INITIATOR) && defined(CONFIG_BT_BAP_BROADCAST_SOURCE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/byteorder.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>
#include <zephyr/bluetooth/audio/cap.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include "common.h"

#define BROADCAST_STREMT_CNT    CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT
#define BROADCAST_ENQUEUE_COUNT 2U
#define TOTAL_BUF_NEEDED        (BROADCAST_ENQUEUE_COUNT * BROADCAST_STREMT_CNT)

BUILD_ASSERT(CONFIG_BT_ISO_TX_BUF_COUNT >= TOTAL_BUF_NEEDED,
	     "CONFIG_BT_ISO_TX_BUF_COUNT should be at least "
	     "BROADCAST_ENQUEUE_COUNT * BROADCAST_STREMT_CNT");

NET_BUF_POOL_FIXED_DEFINE(tx_pool, TOTAL_BUF_NEEDED, BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

extern enum bst_result_t bst_result;
static struct bt_cap_stream broadcast_source_streams[BROADCAST_STREMT_CNT];
static struct bt_cap_stream *broadcast_streams[ARRAY_SIZE(broadcast_source_streams)];
static struct bt_bap_lc3_preset broadcast_preset_16_2_1 = BT_BAP_LC3_BROADCAST_PRESET_16_2_1(
	BT_AUDIO_LOCATION_FRONT_LEFT, BT_AUDIO_CONTEXT_TYPE_MEDIA);

static K_SEM_DEFINE(sem_broadcast_started, 0U, ARRAY_SIZE(broadcast_streams));
static K_SEM_DEFINE(sem_broadcast_stopped, 0U, ARRAY_SIZE(broadcast_streams));

CREATE_FLAG(flag_broadcast_stopping);

static void broadcast_started_cb(struct bt_bap_stream *stream)
{
	printk("Stream %p started\n", stream);
	k_sem_give(&sem_broadcast_started);
}

static void broadcast_stopped_cb(struct bt_bap_stream *stream, uint8_t reason)
{
	printk("Stream %p stopped with reason 0x%02X\n", stream, reason);
	k_sem_give(&sem_broadcast_stopped);
}

static void broadcast_sent_cb(struct bt_bap_stream *bap_stream)
{
	struct bt_cap_stream *cap_stream =
		CONTAINER_OF(bap_stream, struct bt_cap_stream, bap_stream);
	static uint8_t mock_data[CONFIG_BT_ISO_TX_MTU];
	static bool mock_data_initialized;
	static uint32_t seq_num;
	struct net_buf *buf;
	int ret;

	if (broadcast_preset_16_2_1.qos.sdu > CONFIG_BT_ISO_TX_MTU) {
		FAIL("Invalid SDU %u for the MTU: %d", broadcast_preset_16_2_1.qos.sdu,
		     CONFIG_BT_ISO_TX_MTU);
		return;
	}

	if (TEST_FLAG(flag_broadcast_stopping)) {
		return;
	}

	if (!mock_data_initialized) {
		for (size_t i = 0U; i < ARRAY_SIZE(mock_data); i++) {
			/* Initialize mock data */
			mock_data[i] = (uint8_t)i;
		}
		mock_data_initialized = true;
	}

	buf = net_buf_alloc(&tx_pool, K_FOREVER);
	if (buf == NULL) {
		printk("Could not allocate buffer when sending on %p\n", bap_stream);
		return;
	}

	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
	net_buf_add_mem(buf, mock_data, broadcast_preset_16_2_1.qos.sdu);
	ret = bt_cap_stream_send(cap_stream, buf, seq_num++, BT_ISO_TIMESTAMP_NONE);
	if (ret < 0) {
		/* This will end broadcasting on this stream. */
		printk("Unable to broadcast data on %p: %d\n", bap_stream, ret);
		net_buf_unref(buf);
		return;
	}
}

static struct bt_bap_stream_ops broadcast_stream_ops = {
	.started = broadcast_started_cb,
	.stopped = broadcast_stopped_cb,
	.sent = broadcast_sent_cb,
};

static void init(void)
{
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		FAIL("Bluetooth enable failed (err %d)\n", err);
		return;
	}

	(void)memset(broadcast_source_streams, 0, sizeof(broadcast_source_streams));

	for (size_t i = 0; i < ARRAY_SIZE(broadcast_streams); i++) {
		broadcast_streams[i] = &broadcast_source_streams[i];
		bt_cap_stream_ops_register(broadcast_streams[i], &broadcast_stream_ops);
	}
}

static void setup_extended_adv(struct bt_le_ext_adv **adv)
{
	int err;

	/* Create a non-connectable non-scannable advertising set */
	err = bt_le_ext_adv_create(BT_LE_EXT_ADV_NCONN_NAME, NULL, adv);
	if (err != 0) {
		FAIL("Unable to create extended advertising set: %d\n", err);
		return;
	}

	/* Set periodic advertising parameters */
	err = bt_le_per_adv_set_param(*adv, BT_LE_PER_ADV_DEFAULT);
	if (err) {
		FAIL("Failed to set periodic advertising parameters: %d\n", err);
		return;
	}
}

static void setup_extended_adv_data(struct bt_cap_broadcast_source *source,
				    struct bt_le_ext_adv *adv)
{
	/* Broadcast Audio Streaming Endpoint advertising data */
	NET_BUF_SIMPLE_DEFINE(ad_buf, BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE);
	NET_BUF_SIMPLE_DEFINE(base_buf, 128);
	struct bt_data ext_ad;
	struct bt_data per_ad;
	uint32_t broadcast_id;
	int err;

	err = bt_cap_initiator_broadcast_get_id(source, &broadcast_id);
	if (err != 0) {
		FAIL("Unable to get broadcast ID: %d\n", err);
		return;
	}

	/* Setup extended advertising data */
	net_buf_simple_add_le16(&ad_buf, BT_UUID_BROADCAST_AUDIO_VAL);
	net_buf_simple_add_le24(&ad_buf, broadcast_id);
	ext_ad.type = BT_DATA_SVC_DATA16;
	ext_ad.data_len = ad_buf.len + sizeof(ext_ad.type);
	ext_ad.data = ad_buf.data;
	err = bt_le_ext_adv_set_data(adv, &ext_ad, 1, NULL, 0);
	if (err != 0) {
		FAIL("Failed to set extended advertising data: %d\n", err);
		return;
	}

	/* Setup periodic advertising data */
	err = bt_cap_initiator_broadcast_get_base(source, &base_buf);
	if (err != 0) {
		FAIL("Failed to get encoded BASE: %d\n", err);
		return;
	}

	per_ad.type = BT_DATA_SVC_DATA16;
	per_ad.data_len = base_buf.len;
	per_ad.data = base_buf.data;
	err = bt_le_per_adv_set_data(adv, &per_ad, 1);
	if (err != 0) {
		FAIL("Failed to set periodic advertising data: %d\n", err);
		return;
	}
}

static void start_extended_adv(struct bt_le_ext_adv *adv)
{
	int err;

	/* Start extended advertising */
	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err) {
		FAIL("Failed to start extended advertising: %d\n", err);
		return;
	}

	/* Enable Periodic Advertising */
	err = bt_le_per_adv_start(adv);
	if (err) {
		FAIL("Failed to enable periodic advertising: %d\n", err);
		return;
	}
}

static void stop_and_delete_extended_adv(struct bt_le_ext_adv *adv)
{
	int err;

	/* Stop extended advertising */
	err = bt_le_per_adv_stop(adv);
	if (err) {
		FAIL("Failed to stop periodic advertising: %d\n", err);
		return;
	}

	err = bt_le_ext_adv_stop(adv);
	if (err) {
		FAIL("Failed to stop extended advertising: %d\n", err);
		return;
	}

	err = bt_le_ext_adv_delete(adv);
	if (err) {
		FAIL("Failed to delete extended advertising: %d\n", err);
		return;
	}
}

static void test_broadcast_audio_create_inval(void)
{
	struct bt_audio_codec_data bis_codec_data = BT_AUDIO_CODEC_DATA(
		BT_AUDIO_CODEC_CONFIG_LC3_FREQ, BT_AUDIO_CODEC_CONFIG_LC3_FREQ_16KHZ);
	struct bt_cap_initiator_broadcast_stream_param
		stream_params[ARRAY_SIZE(broadcast_source_streams)];
	struct bt_cap_initiator_broadcast_subgroup_param subgroup_param;
	struct bt_cap_initiator_broadcast_create_param create_param;
	struct bt_cap_broadcast_source *broadcast_source;
	struct bt_audio_codec_cfg invalid_codec = BT_AUDIO_CODEC_LC3_CONFIG_16_2(
		BT_AUDIO_LOCATION_FRONT_LEFT, BT_AUDIO_CONTEXT_TYPE_MEDIA);
	int err;

	for (size_t i = 0U; i < ARRAY_SIZE(broadcast_streams); i++) {
		stream_params[i].stream = &broadcast_source_streams[i];
		stream_params[i].data_count = 1U;
		stream_params[i].data = &bis_codec_data;
	}

	subgroup_param.stream_count = ARRAY_SIZE(broadcast_streams);
	subgroup_param.stream_params = stream_params;
	subgroup_param.codec_cfg = &broadcast_preset_16_2_1.codec_cfg;

	create_param.subgroup_count = 1U;
	create_param.subgroup_params = &subgroup_param;
	create_param.qos = &broadcast_preset_16_2_1.qos;
	create_param.packing = BT_ISO_PACKING_SEQUENTIAL;
	create_param.encryption = false;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_create(NULL, &broadcast_source);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_create with NULL param did not fail\n");
		return;
	}

	err = bt_cap_initiator_broadcast_audio_create(&create_param, NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_create with NULL broadcast source did not "
		     "fail\n");
		return;
	}

	/* Clear metadata so that it does not contain the mandatory stream context */
	memset(&invalid_codec.meta, 0, sizeof(invalid_codec.meta));
	subgroup_param.codec_cfg = &invalid_codec;
	err = bt_cap_initiator_broadcast_audio_create(&create_param, NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_create with invalid metadata did not "
		     "fail\n");
		return;
	}

	/* Since we are just casting the CAP parameters to BAP parameters,
	 * we can rely on the BAP tests to verify the values
	 */
}

static void test_broadcast_audio_create(struct bt_cap_broadcast_source **broadcast_source)
{
	struct bt_audio_codec_data bis_codec_data = BT_AUDIO_CODEC_DATA(
		BT_AUDIO_CODEC_CONFIG_LC3_FREQ, BT_AUDIO_CODEC_CONFIG_LC3_FREQ_16KHZ);
	struct bt_cap_initiator_broadcast_stream_param
		stream_params[ARRAY_SIZE(broadcast_source_streams)];
	struct bt_cap_initiator_broadcast_subgroup_param subgroup_param;
	struct bt_cap_initiator_broadcast_create_param create_param;
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(broadcast_streams); i++) {
		stream_params[i].stream = &broadcast_source_streams[i];
		stream_params[i].data_count = 1U;
		stream_params[i].data = &bis_codec_data;
	}

	subgroup_param.stream_count = ARRAY_SIZE(broadcast_streams);
	subgroup_param.stream_params = stream_params;
	subgroup_param.codec_cfg = &broadcast_preset_16_2_1.codec_cfg;

	create_param.subgroup_count = 1U;
	create_param.subgroup_params = &subgroup_param;
	create_param.qos = &broadcast_preset_16_2_1.qos;
	create_param.packing = BT_ISO_PACKING_SEQUENTIAL;
	create_param.encryption = false;

	printk("Creating broadcast source with %zu broadcast_streams\n",
	       ARRAY_SIZE(broadcast_streams));

	err = bt_cap_initiator_broadcast_audio_create(&create_param, broadcast_source);
	if (err != 0) {
		FAIL("Unable to start broadcast source: %d\n", err);
		return;
	}

	printk("Broadcast source created with %zu broadcast_streams\n",
	       ARRAY_SIZE(broadcast_streams));
}

static void test_broadcast_audio_start_inval(struct bt_cap_broadcast_source *broadcast_source,
					     struct bt_le_ext_adv *adv)
{
	int err;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_start(NULL, adv);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_start with NULL broadcast source did not "
		     "fail\n");
		return;
	}

	err = bt_cap_initiator_broadcast_audio_start(broadcast_source, NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_start with NULL adv did not fail\n");
		return;
	}
}

static void test_broadcast_audio_start(struct bt_cap_broadcast_source *broadcast_source,
				       struct bt_le_ext_adv *adv)
{
	int err;

	err = bt_cap_initiator_broadcast_audio_start(broadcast_source, adv);
	if (err != 0) {
		FAIL("Unable to start broadcast source: %d\n", err);
		return;
	}

	printk("Broadcast source created with %zu broadcast_streams\n",
	       ARRAY_SIZE(broadcast_streams));
}

static void test_broadcast_audio_update_inval(struct bt_cap_broadcast_source *broadcast_source)
{
	const uint16_t mock_ccid = 0x1234;
	const struct bt_audio_codec_data new_metadata[] = {
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_STREAM_CONTEXT,
				    (BT_AUDIO_CONTEXT_TYPE_MEDIA & 0xFFU),
				    ((BT_AUDIO_CONTEXT_TYPE_MEDIA >> 8) & 0xFFU)),
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_CCID_LIST, (mock_ccid & 0xFFU),
				    ((mock_ccid >> 8) & 0xFFU)),
	};
	const struct bt_audio_codec_data invalid_metadata[] = {
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_CCID_LIST, (mock_ccid & 0xFFU),
				    ((mock_ccid >> 8) & 0xFFU)),
	};
	int err;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_update(NULL, new_metadata, ARRAY_SIZE(new_metadata));
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_update with NULL broadcast source did not "
		     "fail\n");
		return;
	}

	err = bt_cap_initiator_broadcast_audio_update(broadcast_source, NULL,
						      ARRAY_SIZE(new_metadata));
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_update with NULL metadata did not fail\n");
		return;
	}

	err = bt_cap_initiator_broadcast_audio_update(broadcast_source, new_metadata, 0);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_update with 0 metadata count did not "
		     "fail\n");
		return;
	}

	/* Test with metadata without streaming context */
	err = bt_cap_initiator_broadcast_audio_update(broadcast_source, invalid_metadata,
						      ARRAY_SIZE(invalid_metadata));
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_update with invalid metadata did not "
		     "fail\n");
		return;
	}

	printk("Broadcast metadata updated\n");
}

static void test_broadcast_audio_update(struct bt_cap_broadcast_source *broadcast_source)
{
	const uint16_t mock_ccid = 0x1234;
	const struct bt_audio_codec_data new_metadata[] = {
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_STREAM_CONTEXT,
				    BT_BYTES_LIST_LE16(BT_AUDIO_CONTEXT_TYPE_MEDIA)),
		BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_CCID_LIST,
				    BT_BYTES_LIST_LE16(mock_ccid)),
	};
	int err;

	printk("Updating broadcast metadata\n");

	err = bt_cap_initiator_broadcast_audio_update(broadcast_source, new_metadata,
						      ARRAY_SIZE(new_metadata));
	if (err != 0) {
		FAIL("Failed to update broadcast source metadata: %d\n", err);
		return;
	}

	printk("Broadcast metadata updated\n");
}

static void test_broadcast_audio_stop_inval(void)
{
	int err;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_stop(NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_stop with NULL broadcast source did not "
		     "fail\n");
		return;
	}
}

static void test_broadcast_audio_tx_sync(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(broadcast_streams); i++) {
		struct bt_cap_stream *cap_stream = broadcast_streams[i];
		struct bt_iso_tx_info info;
		int err;

		err = bt_cap_stream_get_tx_sync(cap_stream, &info);
		if (err != 0) {
			FAIL("Failed to get TX sync for stream[%zu]: %p: %d\n", i, cap_stream, err);
			return;
		}

		if (info.seq_num != 0) {
			printk("stream[%zu]: %p seq_num: %u\n", i, cap_stream, info.seq_num);
		} else {
			FAIL("stream[%zu]: %p seq_num was 0\n", i, cap_stream);
			return;
		}
	}
}

static void test_broadcast_audio_stop(struct bt_cap_broadcast_source *broadcast_source)
{
	int err;

	printk("Stopping broadcast metadata\n");

	err = bt_cap_initiator_broadcast_audio_stop(broadcast_source);
	if (err != 0) {
		FAIL("Failed to stop broadcast source: %d\n", err);
		return;
	}

	/* Wait for all to be stopped */
	printk("Waiting for broadcast_streams to be stopped\n");
	for (size_t i = 0U; i < ARRAY_SIZE(broadcast_streams); i++) {
		k_sem_take(&sem_broadcast_stopped, K_FOREVER);
	}

	printk("Broadcast metadata stopped\n");

	/* Verify that it cannot be stopped twice */
	err = bt_cap_initiator_broadcast_audio_stop(broadcast_source);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_stop with already-stopped broadcast source "
		     "did not fail\n");
		return;
	}
}

static void test_broadcast_audio_delete_inval(void)
{
	int err;

	/* Test NULL parameters */
	err = bt_cap_initiator_broadcast_audio_delete(NULL);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_delete with NULL broadcast source did not "
		     "fail\n");
		return;
	}
}

static void test_broadcast_audio_delete(struct bt_cap_broadcast_source *broadcast_source)
{
	int err;

	printk("Stopping broadcast metadata\n");

	err = bt_cap_initiator_broadcast_audio_delete(broadcast_source);
	if (err != 0) {
		FAIL("Failed to stop broadcast source: %d\n", err);
		return;
	}

	printk("Broadcast metadata stopped\n");

	/* Verify that it cannot be deleted twice */
	err = bt_cap_initiator_broadcast_audio_delete(broadcast_source);
	if (err == 0) {
		FAIL("bt_cap_initiator_broadcast_audio_delete with already-deleted broadcast "
		     "source did not fail\n");
		return;
	}
}

static void test_main_cap_initiator_broadcast(void)
{
	struct bt_cap_broadcast_source *broadcast_source;
	struct bt_le_ext_adv *adv;

	(void)memset(broadcast_source_streams, 0, sizeof(broadcast_source_streams));

	init();

	setup_extended_adv(&adv);

	test_broadcast_audio_create_inval();
	test_broadcast_audio_create(&broadcast_source);

	test_broadcast_audio_start_inval(broadcast_source, adv);
	test_broadcast_audio_start(broadcast_source, adv);

	setup_extended_adv_data(broadcast_source, adv);

	start_extended_adv(adv);

	/* Wait for all to be started */
	printk("Waiting for broadcast_streams to be started\n");
	for (size_t i = 0U; i < ARRAY_SIZE(broadcast_streams); i++) {
		k_sem_take(&sem_broadcast_started, K_FOREVER);
	}

	/* Initialize sending */
	for (size_t i = 0U; i < ARRAY_SIZE(broadcast_streams); i++) {
		for (unsigned int j = 0U; j < BROADCAST_ENQUEUE_COUNT; j++) {
			broadcast_sent_cb(&broadcast_streams[i]->bap_stream);
		}
	}

	/* Keeping running for a little while */
	k_sleep(K_SECONDS(5));

	test_broadcast_audio_update_inval(broadcast_source);
	test_broadcast_audio_update(broadcast_source);

	/* Keeping running for a little while */
	k_sleep(K_SECONDS(5));

	test_broadcast_audio_tx_sync();

	test_broadcast_audio_stop_inval();
	test_broadcast_audio_stop(broadcast_source);

	test_broadcast_audio_delete_inval();
	test_broadcast_audio_delete(broadcast_source);
	broadcast_source = NULL;

	stop_and_delete_extended_adv(adv);
	adv = NULL;

	PASS("CAP initiator broadcast passed\n");
}

static const struct bst_test_instance test_cap_initiator_broadcast[] = {
	{
		.test_id = "cap_initiator_broadcast",
		.test_post_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = test_main_cap_initiator_broadcast,
	},
	BSTEST_END_MARKER,
};

struct bst_test_list *test_cap_initiator_broadcast_install(struct bst_test_list *tests)
{
	return bst_add_tests(tests, test_cap_initiator_broadcast);
}

#else /* !(defined(CONFIG_BT_CAP_INITIATOR) && defined(CONFIG_BT_BAP_BROADCAST_SOURCE)) */

struct bst_test_list *test_cap_initiator_broadcast_install(struct bst_test_list *tests)
{
	return tests;
}

#endif /* defined(CONFIG_BT_CAP_INITIATOR) && defined(CONFIG_BT_BAP_BROADCAST_SOURCE) */

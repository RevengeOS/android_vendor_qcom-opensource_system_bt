/******************************************************************************
 * Copyright (C) 2017, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 ******************************************************************************/
/******************************************************************************
 *
 *  Copyright (C) 2016 The Android Open Source Project
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_a2dp_source"
#define ATRACE_TAG ATRACE_TAG_AUDIO

#include <base/logging.h>
#ifndef OS_GENERIC
#include <cutils/trace.h>
#endif
#include <limits.h>
#include <string.h>
#include <algorithm>

#include "audio_a2dp_hw/include/audio_a2dp_hw.h"
#include "bt_common.h"
#include "bta_av_ci.h"
#include "btif_a2dp.h"
#include "btif_a2dp_control.h"
#include "btif_a2dp_source.h"
#include "btif_av.h"
#include "btif_av_co.h"
#include "btif_util.h"
#include "osi/include/fixed_queue.h"
#include "osi/include/log.h"
#include "osi/include/metrics.h"
#include "osi/include/mutex.h"
#include "osi/include/osi.h"
#include "osi/include/thread.h"
#include "osi/include/time.h"
#include "uipc.h"
#include "btif_a2dp_audio_interface.h"

using system_bt_osi::BluetoothMetricsLogger;
using system_bt_osi::A2dpSessionMetrics;

/**
 * The typical runlevel of the tx queue size is ~1 buffer
 * but due to link flow control or thread preemption in lower
 * layers we might need to temporarily buffer up data.
 */
#define MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ (MAX_PCM_FRAME_NUM_PER_TICK * 2)
#define BTIF_UNBLOCK_AUDIO_START_TOUT 3000
#define BTIF_REMOTE_START_TOUT 3000
enum {
  BTIF_A2DP_SOURCE_STATE_OFF,
  BTIF_A2DP_SOURCE_STATE_STARTING_UP,
  BTIF_A2DP_SOURCE_STATE_RUNNING,
  BTIF_A2DP_SOURCE_STATE_SHUTTING_DOWN
};

/* BTIF Media Source event definition */
enum {
  BTIF_MEDIA_AUDIO_TX_START = 1,
  BTIF_MEDIA_AUDIO_TX_STOP,
  BTIF_MEDIA_AUDIO_TX_FLUSH,
  BTIF_MEDIA_SOURCE_ENCODER_INIT,
  BTIF_MEDIA_SOURCE_ENCODER_USER_CONFIG_UPDATE,
  BTIF_MEDIA_AUDIO_FEEDING_UPDATE,
  BTIF_MEDIA_RESET_VS_STATE
};

#define MAX_MEDIA_WORKQUEUE_SEM_COUNT 1024

/* tBTIF_A2DP_SOURCE_ENCODER_INIT msg structure */
typedef struct {
  BT_HDR hdr;
  tA2DP_ENCODER_INIT_PEER_PARAMS peer_params;
} tBTIF_A2DP_SOURCE_ENCODER_INIT;

/* tBTIF_A2DP_SOURCE_ENCODER_USER_CONFIG_UPDATE msg structure */
typedef struct {
  BT_HDR hdr;
  RawAddress bd_addr;
  btav_a2dp_codec_config_t user_config;
} tBTIF_A2DP_SOURCE_ENCODER_USER_CONFIG_UPDATE;

/* tBTIF_A2DP_AUDIO_FEEDING_UPDATE msg structure */
typedef struct {
  BT_HDR hdr;
  btav_a2dp_codec_config_t feeding_params;
} tBTIF_A2DP_AUDIO_FEEDING_UPDATE;

typedef struct {
  // Counter for total updates
  size_t total_updates;

  // Last update timestamp (in us)
  uint64_t last_update_us;

  // Counter for overdue scheduling
  size_t overdue_scheduling_count;

  // Accumulated overdue scheduling deviations (in us)
  uint64_t total_overdue_scheduling_delta_us;

  // Max. overdue scheduling delta time (in us)
  uint64_t max_overdue_scheduling_delta_us;

  // Counter for premature scheduling
  size_t premature_scheduling_count;

  // Accumulated premature scheduling deviations (in us)
  uint64_t total_premature_scheduling_delta_us;

  // Max. premature scheduling delta time (in us)
  uint64_t max_premature_scheduling_delta_us;

  // Counter for exact scheduling
  size_t exact_scheduling_count;

  // Accumulated and counted scheduling time (in us)
  uint64_t total_scheduling_time_us;
} scheduling_stats_t;

typedef struct {
  uint64_t session_start_us;
  uint64_t session_end_us;

  scheduling_stats_t tx_queue_enqueue_stats;
  scheduling_stats_t tx_queue_dequeue_stats;

  size_t tx_queue_total_frames;
  size_t tx_queue_max_frames_per_packet;

  uint64_t tx_queue_total_queueing_time_us;
  uint64_t tx_queue_max_queueing_time_us;

  size_t tx_queue_total_readbuf_calls;
  uint64_t tx_queue_last_readbuf_us;

  size_t tx_queue_total_flushed_messages;
  uint64_t tx_queue_last_flushed_us;

  size_t tx_queue_total_dropped_messages;
  size_t tx_queue_max_dropped_messages;
  size_t tx_queue_dropouts;
  uint64_t tx_queue_last_dropouts_us;

  size_t media_read_total_underflow_bytes;
  size_t media_read_total_underflow_count;
  uint64_t media_read_last_underflow_us;
} btif_media_stats_t;

typedef struct {
  thread_t* worker_thread;
  fixed_queue_t* cmd_msg_queue;
  fixed_queue_t* tx_audio_queue;
  bool tx_flush; /* Discards any outgoing data when true */
  alarm_t* unblock_audio_start_alarm;
  alarm_t* media_alarm;
  alarm_t *remote_start_alarm;
  const tA2DP_ENCODER_INTERFACE* encoder_interface;
  period_ms_t encoder_interval_ms; /* Local copy of the encoder interval */
  btif_media_stats_t stats;
  btif_media_stats_t accumulated_stats;
  int last_remote_started_index;
  int *last_started_index_pointer;
} tBTIF_A2DP_SOURCE_CB;

static tBTIF_A2DP_SOURCE_CB btif_a2dp_source_cb;
tBTIF_A2DP_SOURCE_VSC btif_a2dp_src_vsc;

static int btif_a2dp_source_state = BTIF_A2DP_SOURCE_STATE_OFF;
extern bool enc_update_in_progress;
extern bool reconfig_a2dp;
extern bool tx_enc_update_initiated;
extern bool is_block_hal_start;
static void btif_a2dp_source_command_ready(fixed_queue_t* queue, void* context);
static void btif_a2dp_source_startup_delayed(void* context);
static void btif_a2dp_source_shutdown_delayed(void* context);
static void btif_a2dp_source_audio_tx_start_event(void);
static void btif_a2dp_source_audio_tx_stop_event(void);
static void btif_a2dp_source_audio_tx_flush_event(BT_HDR* p_msg);
static void btif_a2dp_source_encoder_init_event(BT_HDR* p_msg);
static void btif_a2dp_source_encoder_user_config_update_event(BT_HDR* p_msg);
static void btif_a2dp_source_audio_feeding_update_event(BT_HDR* p_msg);
static void btif_a2dp_source_encoder_init(void);
static void btif_a2dp_source_encoder_init_req(
    tBTIF_A2DP_SOURCE_ENCODER_INIT* p_msg);
static bool btif_a2dp_source_audio_tx_flush_req(void);
static void btif_a2dp_source_alarm_cb(void* context);
static void btif_a2dp_source_audio_handle_timer(void* context);
static uint32_t btif_a2dp_source_read_callback(uint8_t* p_buf, uint32_t len);
static bool btif_a2dp_source_enqueue_callback(BT_HDR* p_buf, size_t frames_n,
                                              uint32_t bytes_read);
static void log_tstamps_us(const char* comment, uint64_t timestamp_us);
static void update_scheduling_stats(scheduling_stats_t* stats, uint64_t now_us,
                                    uint64_t expected_delta);
static void btm_read_rssi_cb(void* data);
static void btm_read_failed_contact_counter_cb(void* data);
static void btm_read_automatic_flush_timeout_cb(void* data);
static void btm_read_tx_power_cb(void* data);
static void btif_a2dp_source_unblock_audio_start_timeout(void* context);
static void btif_a2dp_source_remote_start_timeout(void* context);
static char a2dp_hal_imp[PROPERTY_VALUE_MAX] = "false";
UNUSED_ATTR static const char* dump_media_event(uint16_t event) {
  switch (event) {
    CASE_RETURN_STR(BTIF_MEDIA_AUDIO_TX_START)
    CASE_RETURN_STR(BTIF_MEDIA_AUDIO_TX_STOP)
    CASE_RETURN_STR(BTIF_MEDIA_AUDIO_TX_FLUSH)
    CASE_RETURN_STR(BTIF_MEDIA_SOURCE_ENCODER_INIT)
    CASE_RETURN_STR(BTIF_MEDIA_SOURCE_ENCODER_USER_CONFIG_UPDATE)
    CASE_RETURN_STR(BTIF_MEDIA_AUDIO_FEEDING_UPDATE)
    CASE_RETURN_STR(BTIF_MEDIA_RESET_VS_STATE)
    default:
      break;
  }
  return "UNKNOWN A2DP SOURCE EVENT";
}

void btif_a2dp_source_accumulate_scheduling_stats(scheduling_stats_t* src,
                                                  scheduling_stats_t* dst) {
  dst->total_updates += src->total_updates;
  dst->last_update_us = src->last_update_us;
  dst->overdue_scheduling_count += src->overdue_scheduling_count;
  dst->total_overdue_scheduling_delta_us +=
      src->total_overdue_scheduling_delta_us;
  dst->max_overdue_scheduling_delta_us =
      std::max(dst->max_overdue_scheduling_delta_us,
               src->max_overdue_scheduling_delta_us);
  dst->premature_scheduling_count += src->premature_scheduling_count;
  dst->total_premature_scheduling_delta_us +=
      src->total_premature_scheduling_delta_us;
  dst->max_premature_scheduling_delta_us =
      std::max(dst->max_premature_scheduling_delta_us,
               src->max_premature_scheduling_delta_us);
  dst->exact_scheduling_count += src->exact_scheduling_count;
  dst->total_scheduling_time_us += src->total_scheduling_time_us;
}

void btif_a2dp_source_accumulate_stats(btif_media_stats_t* src,
                                       btif_media_stats_t* dst) {
  dst->tx_queue_total_frames += src->tx_queue_total_frames;
  dst->tx_queue_max_frames_per_packet = std::max(
      dst->tx_queue_max_frames_per_packet, src->tx_queue_max_frames_per_packet);
  dst->tx_queue_total_queueing_time_us += src->tx_queue_total_queueing_time_us;
  dst->tx_queue_max_queueing_time_us = std::max(
      dst->tx_queue_max_queueing_time_us, src->tx_queue_max_queueing_time_us);
  dst->tx_queue_total_readbuf_calls += src->tx_queue_total_readbuf_calls;
  dst->tx_queue_last_readbuf_us = src->tx_queue_last_readbuf_us;
  dst->tx_queue_total_flushed_messages += src->tx_queue_total_flushed_messages;
  dst->tx_queue_last_flushed_us = src->tx_queue_last_flushed_us;
  dst->tx_queue_total_dropped_messages += src->tx_queue_total_dropped_messages;
  dst->tx_queue_max_dropped_messages = std::max(
      dst->tx_queue_max_dropped_messages, src->tx_queue_max_dropped_messages);
  dst->tx_queue_dropouts += src->tx_queue_dropouts;
  dst->tx_queue_last_dropouts_us = src->tx_queue_last_dropouts_us;
  dst->media_read_total_underflow_bytes +=
      src->media_read_total_underflow_bytes;
  dst->media_read_total_underflow_count +=
      src->media_read_total_underflow_count;
  dst->media_read_last_underflow_us = src->media_read_last_underflow_us;
  btif_a2dp_source_accumulate_scheduling_stats(&src->tx_queue_enqueue_stats,
                                               &dst->tx_queue_enqueue_stats);
  btif_a2dp_source_accumulate_scheduling_stats(&src->tx_queue_dequeue_stats,
                                               &dst->tx_queue_dequeue_stats);
  memset(src, 0, sizeof(btif_media_stats_t));
}

bool btif_a2dp_source_startup(void) {
  if (btif_a2dp_source_state != BTIF_A2DP_SOURCE_STATE_OFF) {
    APPL_TRACE_ERROR("%s: A2DP Source media task already running", __func__);
    return false;
  }

  memset(&btif_a2dp_source_cb, 0, sizeof(btif_a2dp_source_cb));
  btif_a2dp_source_state = BTIF_A2DP_SOURCE_STATE_STARTING_UP;
  btif_a2dp_source_cb.last_remote_started_index = -1;
  btif_a2dp_source_cb.last_started_index_pointer = NULL;
  APPL_TRACE_EVENT("## A2DP SOURCE START MEDIA THREAD ##");

  /* Start A2DP Source media task */
  btif_a2dp_source_cb.worker_thread =
      thread_new_sized("media_worker", MAX_MEDIA_WORKQUEUE_SEM_COUNT);
  if (btif_a2dp_source_cb.worker_thread == NULL) {
    APPL_TRACE_ERROR("%s: unable to start up media thread", __func__);
    btif_a2dp_source_state = BTIF_A2DP_SOURCE_STATE_OFF;
    return false;
  }

  btif_a2dp_source_cb.tx_audio_queue = fixed_queue_new(SIZE_MAX);

  btif_a2dp_source_cb.cmd_msg_queue = fixed_queue_new(SIZE_MAX);
  fixed_queue_register_dequeue(
      btif_a2dp_source_cb.cmd_msg_queue,
      thread_get_reactor(btif_a2dp_source_cb.worker_thread),
      btif_a2dp_source_command_ready, NULL);

  APPL_TRACE_EVENT("## A2DP SOURCE MEDIA THREAD STARTED ##");

  /* Schedule the rest of the startup operations */
  thread_post(btif_a2dp_source_cb.worker_thread,
              btif_a2dp_source_startup_delayed, NULL);

  return true;
}

static void btif_a2dp_source_startup_delayed(UNUSED_ATTR void* context) {
  raise_priority_a2dp(TASK_HIGH_MEDIA);
  btif_a2dp_control_init();
  btif_a2dp_source_state = BTIF_A2DP_SOURCE_STATE_RUNNING;
  APPL_TRACE_EVENT("%s: enc_update_in_progress = %d", __func__, enc_update_in_progress);
  enc_update_in_progress = FALSE;
  BluetoothMetricsLogger::GetInstance()->LogBluetoothSessionStart(
      system_bt_osi::CONNECTION_TECHNOLOGY_TYPE_BREDR, 0);
}

void btif_a2dp_source_shutdown(void) {
  if ((btif_a2dp_source_state == BTIF_A2DP_SOURCE_STATE_OFF) ||
      (btif_a2dp_source_state == BTIF_A2DP_SOURCE_STATE_SHUTTING_DOWN)) {
    return;
  }

  /* Make sure no channels are restarted while shutting down */
  btif_a2dp_source_state = BTIF_A2DP_SOURCE_STATE_SHUTTING_DOWN;

  APPL_TRACE_EVENT("## A2DP SOURCE STOP MEDIA THREAD ##");

  if (btif_a2dp_source_cb.unblock_audio_start_alarm != NULL) {
    alarm_free(btif_a2dp_source_cb.unblock_audio_start_alarm);
    btif_a2dp_source_cb.unblock_audio_start_alarm = NULL;
  }

  // Stop the timer
  alarm_free(btif_a2dp_source_cb.media_alarm);
  btif_a2dp_source_cb.media_alarm = NULL;
  if (btif_a2dp_source_cb.remote_start_alarm != NULL) {
    alarm_free(btif_a2dp_source_cb.remote_start_alarm);
    btif_a2dp_source_cb.remote_start_alarm = NULL;
    btif_dispatch_sm_event(BTIF_AV_RESET_REMOTE_STARTED_FLAG_EVT, NULL, 0);
  }

  // Exit the thread
  fixed_queue_free(btif_a2dp_source_cb.cmd_msg_queue, NULL);
  btif_a2dp_source_cb.cmd_msg_queue = NULL;
  thread_post(btif_a2dp_source_cb.worker_thread,
              btif_a2dp_source_shutdown_delayed, NULL);
  thread_free(btif_a2dp_source_cb.worker_thread);
  btif_a2dp_source_cb.worker_thread = NULL;
  APPL_TRACE_EVENT("## A2DP SOURCE MEDIA THREAD STOPPED ##");
}

static void btif_a2dp_source_shutdown_delayed(UNUSED_ATTR void* context) {
  APPL_TRACE_DEBUG("%s", __func__);
  btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
  btif_a2dp_control_cleanup();
  fixed_queue_free(btif_a2dp_source_cb.tx_audio_queue, NULL);
  btif_a2dp_source_cb.tx_audio_queue = NULL;

  btif_a2dp_source_state = BTIF_A2DP_SOURCE_STATE_OFF;
  APPL_TRACE_EVENT("%s: enc_update_in_progress = %d", __func__, enc_update_in_progress);
  enc_update_in_progress = FALSE;
  BluetoothMetricsLogger::GetInstance()->LogBluetoothSessionEnd(
      system_bt_osi::DISCONNECT_REASON_UNKNOWN, 0);
}

bool btif_a2dp_source_media_task_is_running(void) {
  return (btif_a2dp_source_state == BTIF_A2DP_SOURCE_STATE_RUNNING);
}

bool btif_a2dp_source_media_task_is_shutting_down(void) {
  return (btif_a2dp_source_state == BTIF_A2DP_SOURCE_STATE_SHUTTING_DOWN);
}

bool btif_a2dp_source_is_streaming(void) {
  return alarm_is_scheduled(btif_a2dp_source_cb.media_alarm);
}

bool btif_a2dp_source_is_remote_start(void) {
  return alarm_is_scheduled(btif_a2dp_source_cb.remote_start_alarm);
}

int btif_a2dp_source_last_remote_start_index() {
    APPL_TRACE_DEBUG("%s:remote started set for device index %d", __func__, btif_a2dp_source_cb.last_remote_started_index);
    return btif_a2dp_source_cb.last_remote_started_index;
}

void btif_a2dp_source_cancel_remote_start() {
  if (btif_a2dp_source_cb.remote_start_alarm != NULL) {
    alarm_free(btif_a2dp_source_cb.remote_start_alarm);
    btif_a2dp_source_cb.remote_start_alarm = NULL;
    btif_av_clear_remote_start_timer(btif_a2dp_source_cb.last_remote_started_index);
    APPL_TRACE_DEBUG("%s: cancel remote start on AV index %d", __func__, btif_a2dp_source_cb.last_remote_started_index);
    btif_a2dp_source_cb.last_remote_started_index = -1;
    if(btif_a2dp_source_cb.last_started_index_pointer)
      osi_free(btif_a2dp_source_cb.last_started_index_pointer);
    btif_a2dp_source_cb.last_started_index_pointer = NULL;
  }
  return;
}

static void btif_media_remote_start_alarm_cb(UNUSED_ATTR void *context) {
  thread_post(btif_a2dp_source_cb.worker_thread,
              btif_a2dp_source_remote_start_timeout, context);
}

static void btif_a2dp_source_remote_start_timeout(UNUSED_ATTR void* context) {
  int *arg = (int *)context;
  int index = *arg;
  APPL_TRACE_DEBUG("%s: Suspend stream request to Av index %d", __func__, *arg);
  if (btif_a2dp_source_cb.remote_start_alarm != NULL &&
      index == btif_a2dp_source_cb.last_remote_started_index) {
    alarm_free(btif_a2dp_source_cb.remote_start_alarm);
    btif_a2dp_source_cb.remote_start_alarm = NULL;
    btif_a2dp_source_cb.last_remote_started_index = -1;
    btif_a2dp_source_cb.last_started_index_pointer = NULL;
  }
  btif_dispatch_sm_event(BTIF_AV_REMOTE_SUSPEND_STREAM_REQ_EVT, &index, sizeof(index));
  if(arg) {
    osi_free(arg);
  }
  return;
}

void btif_a2dp_source_on_remote_start(struct alarm_t *remote_start_alarm, int index) {
  // initiate remote start timer for index basis
  int *arg = NULL;
  arg = (int *) osi_malloc(sizeof(int));
  remote_start_alarm = alarm_new("btif.remote_start_task");
  if (!remote_start_alarm || !arg) {
    LOG_ERROR(LOG_TAG,"%s:unable to allocate media alarm",__func__);
    btif_av_clear_remote_start_timer(index);
    btif_dispatch_sm_event(BTIF_AV_SUSPEND_STREAM_REQ_EVT, &index, sizeof(index));
    return;
  }
  *arg = index;
  alarm_set(remote_start_alarm, BTIF_REMOTE_START_TOUT,
            btif_media_remote_start_alarm_cb, (void *)arg);
  btif_a2dp_source_cb.remote_start_alarm = remote_start_alarm;
  btif_a2dp_source_cb.last_remote_started_index = index;
  btif_a2dp_source_cb.last_started_index_pointer = arg;
  APPL_TRACE_DEBUG("%s: Remote start timer started index %d arg %d", __func__, index, *arg);
}

static void btif_media_unblock_audio_start_alarm_cb(UNUSED_ATTR void *context) {
  thread_post(btif_a2dp_source_cb.worker_thread,
              btif_a2dp_source_unblock_audio_start_timeout, NULL);
}

void btif_trigger_unblock_audio_start_recovery_timer() {
  // Clear any existing timer before allocate
  btif_a2dp_source_cancel_unblock_audio_start();

  // Allocate new timer
  btif_a2dp_source_cb.unblock_audio_start_alarm = alarm_new("btif.unblock_audio_start_task");
  if (!btif_a2dp_source_cb.unblock_audio_start_alarm) {
    LOG_ERROR(LOG_TAG,"%s:unable to allocate unblock start alarm",__func__);
    return;
  }
  alarm_set(btif_a2dp_source_cb.unblock_audio_start_alarm, BTIF_UNBLOCK_AUDIO_START_TOUT,
            btif_media_unblock_audio_start_alarm_cb, NULL);
  APPL_TRACE_DEBUG("%s: Unblock Audio start timer started", __func__);
}

void btif_a2dp_source_cancel_unblock_audio_start(void) {
  APPL_TRACE_DEBUG("%s: Unblock Audio start timer cancelled", __func__);
  if (btif_a2dp_source_cb.unblock_audio_start_alarm != NULL) {
    alarm_free(btif_a2dp_source_cb.unblock_audio_start_alarm);
    btif_a2dp_source_cb.unblock_audio_start_alarm = NULL;
  }
  return;
}

static void btif_a2dp_source_unblock_audio_start_timeout(void* context) {
  APPL_TRACE_DEBUG("%s: After 2 sec timeout unblock the Audio Start timer", __func__);
  if (is_block_hal_start) {
    is_block_hal_start = false;
  }
}

static void btif_a2dp_source_command_ready(fixed_queue_t* queue,
                                           UNUSED_ATTR void* context) {
  BT_HDR* p_msg = (BT_HDR*)fixed_queue_dequeue(queue);

  LOG_VERBOSE(LOG_TAG, "%s: event %d %s", __func__, p_msg->event,
              dump_media_event(p_msg->event));

  switch (p_msg->event) {
    case BTIF_MEDIA_AUDIO_TX_START:
      btif_a2dp_source_audio_tx_start_event();
      break;
    case BTIF_MEDIA_AUDIO_TX_STOP:
      btif_a2dp_source_audio_tx_stop_event();
      break;
    case BTIF_MEDIA_AUDIO_TX_FLUSH:
      btif_a2dp_source_audio_tx_flush_event(p_msg);
      break;
    case BTIF_MEDIA_SOURCE_ENCODER_INIT:
      btif_a2dp_source_encoder_init_event(p_msg);
      break;
    case BTIF_MEDIA_SOURCE_ENCODER_USER_CONFIG_UPDATE:
      btif_a2dp_source_encoder_user_config_update_event(p_msg);
      break;
    case BTIF_MEDIA_AUDIO_FEEDING_UPDATE:
      btif_a2dp_source_audio_feeding_update_event(p_msg);
      break;
    case BTIF_MEDIA_RESET_VS_STATE:
      btif_a2dp_src_vsc.tx_started = FALSE;
      btif_a2dp_src_vsc.tx_stop_initiated = FALSE;
      btif_a2dp_src_vsc.vs_configs_exchanged = FALSE;
      btif_a2dp_src_vsc.tx_start_initiated = FALSE;
      tx_enc_update_initiated = FALSE;
      break;
    default:
      APPL_TRACE_ERROR("ERROR in %s unknown event %d", __func__, p_msg->event);
      break;
  }

  osi_free(p_msg);
  LOG_VERBOSE(LOG_TAG, "%s: %s DONE", __func__, dump_media_event(p_msg->event));
}

bt_status_t btif_a2dp_source_setup_codec(tBTA_AV_HNDL hndl) {
  APPL_TRACE_EVENT("## A2DP SOURCE SETUP CODEC ##");
  bt_status_t status = BT_STATUS_FAIL;

  mutex_global_lock();
  status =  bta_av_set_a2dp_current_codec(hndl);
  if (status == BT_STATUS_SUCCESS) {
    /* Init the encoding task */
    btif_a2dp_source_encoder_init();

    A2dpCodecConfig* current_codec = bta_av_get_a2dp_current_codec();
    btav_a2dp_codec_config_t codec_config;
    APPL_TRACE_DEBUG("%s: codec_config.codec_type:%d", __func__, codec_config.codec_type);

    //get the current codec config, so that we can get the codec type.
    if (current_codec != nullptr) {
      codec_config = current_codec->getCodecConfig();
    } else {
      APPL_TRACE_ERROR("%s: current codec is null, returns fail.", __func__);
      mutex_global_unlock();
      return BT_STATUS_FAIL;
    }

    uint8_t p_codec_info[AVDT_CODEC_SIZE];
    memset(p_codec_info, 0, AVDT_CODEC_SIZE);

    //copy peer codec info to p_codec_info
    if (!current_codec->copyOutOtaCodecConfig(p_codec_info)) {
      APPL_TRACE_ERROR("%s: Fetching peer codec info returns fail.", __func__);
      mutex_global_unlock();
      return BT_STATUS_FAIL;
    }

    //int index = 0;
    //index = HANDLE_TO_INDEX(hndl);
    RawAddress peer_bda;
    btif_av_get_active_peer_addr(&peer_bda);

    tBT_FLOW_SPEC flow_spec;
    memset(&flow_spec, 0x00, sizeof(flow_spec));

    flow_spec.flow_direction = 0x00;     /* flow direction - out going */
    flow_spec.service_type = 0x02;       /* Guaranteed */
    flow_spec.token_rate = 0x00;         /* bytes/second - no token rate is specified*/
    flow_spec.token_bucket_size = 0x00;  /* bytes - no token bucket is needed*/
    flow_spec.latency = 0xFFFFFFFF;      /* microseconds - default value */

    if (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_SBC) {
      flow_spec.peak_bandwidth = (345*1000)/8; /* bytes/second */

    } else if (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_APTX)  {
      flow_spec.peak_bandwidth = (380*1000)/8; /* bytes/second */

    } else if (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_HD) {
      flow_spec.peak_bandwidth = (660*1000)/8; /* bytes/second */

    } else if (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC) {
      /* For ABR mode default peak bandwidth is 0, for static it will be fetched */
      uint32_t bitrate = 0;
      bitrate = A2DP_GetTrackBitRate(p_codec_info);
      APPL_TRACE_DEBUG(LOG_TAG,"bitrate = %d", bitrate);

      flow_spec.peak_bandwidth = bitrate/8;  /* bytes/second */

    } else if (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_AAC) {
      flow_spec.peak_bandwidth = (320*1000)/8; /* bytes/second */
    }
    APPL_TRACE_DEBUG("%s: peak_bandwidth: %d", __func__, flow_spec.peak_bandwidth);
    BTM_FlowSpec (peer_bda, &flow_spec, NULL);
  } else {
    APPL_TRACE_ERROR("%s() can not setup current codec", __func__);
    status = BT_STATUS_FAIL;
  }

  mutex_global_unlock();
  return status;
}

void btif_a2dp_source_start_audio_req(void) {
  BT_HDR* p_buf = (BT_HDR*)osi_malloc(sizeof(BT_HDR));

  p_buf->event = BTIF_MEDIA_AUDIO_TX_START;
  fixed_queue_enqueue(btif_a2dp_source_cb.cmd_msg_queue, p_buf);
  memset(&btif_a2dp_source_cb.stats, 0, sizeof(btif_media_stats_t));
  // Assign session_start_us to 1 when time_get_os_boottime_us() is 0 to
  // indicate btif_a2dp_source_start_audio_req() has been called
  btif_a2dp_source_cb.stats.session_start_us = time_get_os_boottime_us();
  if (btif_a2dp_source_cb.stats.session_start_us == 0) {
    btif_a2dp_source_cb.stats.session_start_us = 1;
  }
  btif_a2dp_source_cb.stats.session_end_us = 0;
}

void btif_a2dp_source_stop_audio_req(void) {
  BT_HDR* p_buf = (BT_HDR*)osi_malloc(sizeof(BT_HDR));
  tA2DP_CTRL_CMD pending_cmd = btif_a2dp_get_pending_command();

  p_buf->event = BTIF_MEDIA_AUDIO_TX_STOP;

  /*
   * Explicitly check whether btif_a2dp_source_cb.cmd_msg_queue is not NULL
   * to avoid a race condition during shutdown of the Bluetooth stack.
   * This race condition is triggered when A2DP audio is streaming on
   * shutdown:
   * "btif_a2dp_source_on_stopped() -> btif_a2dp_source_stop_audio_req()"
   * is called to stop the particular audio stream, and this happens right
   * after the "BTIF_AV_CLEANUP_REQ_EVT -> btif_a2dp_source_shutdown()"
   * processing during the shutdown of the Bluetooth stack.
   */
  if (btif_a2dp_source_cb.cmd_msg_queue != NULL) {
    fixed_queue_enqueue(btif_a2dp_source_cb.cmd_msg_queue, p_buf);
  } else if (pending_cmd == A2DP_CTRL_CMD_STOP ||
      pending_cmd == A2DP_CTRL_CMD_SUSPEND) {
    BTIF_TRACE_DEBUG("media msg queue null, ack pending stop/suspend");
    btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
  }
  btif_a2dp_source_cb.stats.session_end_us = time_get_os_boottime_us();
  btif_a2dp_source_update_metrics();
  btif_a2dp_source_accumulate_stats(&btif_a2dp_source_cb.stats,
                                    &btif_a2dp_source_cb.accumulated_stats);
}

static void btif_a2dp_source_encoder_init(void) {
  tBTIF_A2DP_SOURCE_ENCODER_INIT msg;

  // Check to make sure the platform has 8 bits/byte since
  // we're using that in frame size calculations now.
  CHECK(CHAR_BIT == 8);

  APPL_TRACE_DEBUG("%s", __func__);

  bta_av_co_get_peer_params(&msg.peer_params);
  btif_a2dp_source_encoder_init_req(&msg);
}

static void btif_a2dp_source_encoder_init_req(
    tBTIF_A2DP_SOURCE_ENCODER_INIT* p_msg) {
  tBTIF_A2DP_SOURCE_ENCODER_INIT* p_buf =
      (tBTIF_A2DP_SOURCE_ENCODER_INIT*)osi_malloc(
          sizeof(tBTIF_A2DP_SOURCE_ENCODER_INIT));

  memcpy(p_buf, p_msg, sizeof(tBTIF_A2DP_SOURCE_ENCODER_INIT));
  p_buf->hdr.event = BTIF_MEDIA_SOURCE_ENCODER_INIT;
  fixed_queue_enqueue(btif_a2dp_source_cb.cmd_msg_queue, p_buf);
}

void btif_media_send_reset_vendor_state() {
  APPL_TRACE_ERROR("%s:", __func__);
  BT_HDR *p_buf = (BT_HDR *)osi_malloc(sizeof(BT_HDR));
  p_buf->event = BTIF_MEDIA_RESET_VS_STATE;
  if (btif_a2dp_source_cb.cmd_msg_queue != NULL)
    fixed_queue_enqueue(btif_a2dp_source_cb.cmd_msg_queue, p_buf);
}

static void btif_a2dp_source_encoder_init_event(BT_HDR* p_msg) {
  tBTIF_A2DP_SOURCE_ENCODER_INIT* p_encoder_init =
      (tBTIF_A2DP_SOURCE_ENCODER_INIT*)p_msg;

  APPL_TRACE_DEBUG("%s", __func__);

  btif_a2dp_source_cb.encoder_interface = bta_av_co_get_encoder_interface();
  if (btif_a2dp_source_cb.encoder_interface == NULL) {
    APPL_TRACE_ERROR("%s: Cannot stream audio: no source encoder interface",
                     __func__);
    return;
  }

  A2dpCodecConfig* a2dp_codec_config = bta_av_get_a2dp_current_codec();
  if (a2dp_codec_config == nullptr) {
    APPL_TRACE_ERROR("%s: Cannot stream audio: current codec is not set",
                     __func__);
    return;
  }

  btif_a2dp_source_cb.encoder_interface->encoder_init(
      &p_encoder_init->peer_params, a2dp_codec_config,
      btif_a2dp_source_read_callback, btif_a2dp_source_enqueue_callback);

  // Save a local copy of the encoder_interval_ms
  btif_a2dp_source_cb.encoder_interval_ms =
      btif_a2dp_source_cb.encoder_interface->get_encoder_interval_ms();
}

void btif_a2dp_source_encoder_user_config_update_req(
    const btav_a2dp_codec_config_t& codec_user_config, const RawAddress& bd_addr) {
  tBTIF_A2DP_SOURCE_ENCODER_USER_CONFIG_UPDATE* p_buf =
      ( tBTIF_A2DP_SOURCE_ENCODER_USER_CONFIG_UPDATE*)osi_malloc(
          sizeof(tBTIF_A2DP_SOURCE_ENCODER_USER_CONFIG_UPDATE));

  p_buf->user_config = codec_user_config;
  p_buf->hdr.event = BTIF_MEDIA_SOURCE_ENCODER_USER_CONFIG_UPDATE;
  p_buf->bd_addr = bd_addr;
  fixed_queue_enqueue(btif_a2dp_source_cb.cmd_msg_queue, p_buf);
}

static void btif_a2dp_source_encoder_user_config_update_event(BT_HDR* p_msg) {
  tBTIF_A2DP_SOURCE_ENCODER_USER_CONFIG_UPDATE* p_user_config =
      (tBTIF_A2DP_SOURCE_ENCODER_USER_CONFIG_UPDATE*)p_msg;

  APPL_TRACE_DEBUG("%s", __func__);
  if (!bta_av_co_set_codec_user_config(p_user_config->user_config, p_user_config->bd_addr)) {
    APPL_TRACE_ERROR("%s: cannot update codec user configuration", __func__);
  }
}

void btif_a2dp_source_feeding_update_req(
    const btav_a2dp_codec_config_t& codec_audio_config) {
  tBTIF_A2DP_AUDIO_FEEDING_UPDATE* p_buf =
      (tBTIF_A2DP_AUDIO_FEEDING_UPDATE*)osi_malloc(
          sizeof(tBTIF_A2DP_AUDIO_FEEDING_UPDATE));

  p_buf->feeding_params = codec_audio_config;
  p_buf->hdr.event = BTIF_MEDIA_AUDIO_FEEDING_UPDATE;
  fixed_queue_enqueue(btif_a2dp_source_cb.cmd_msg_queue, p_buf);
}

static void btif_a2dp_source_audio_feeding_update_event(BT_HDR* p_msg) {
  tBTIF_A2DP_AUDIO_FEEDING_UPDATE* p_feeding =
      (tBTIF_A2DP_AUDIO_FEEDING_UPDATE*)p_msg;

  APPL_TRACE_DEBUG("%s", __func__);
  if (!bta_av_co_set_codec_audio_config(p_feeding->feeding_params)) {
    APPL_TRACE_ERROR("%s: cannot update codec audio feeding parameters",
                     __func__);
  }
}

void btif_a2dp_source_on_idle(void) {
  if (btif_av_is_split_a2dp_enabled())
    btif_media_send_reset_vendor_state();
  if (btif_a2dp_source_state == BTIF_A2DP_SOURCE_STATE_OFF) return;

  /* Make sure media task is stopped */
  btif_a2dp_source_stop_audio_req();
}

void btif_a2dp_source_on_stopped(tBTA_AV_SUSPEND* p_av_suspend) {
  APPL_TRACE_EVENT("## ON A2DP SOURCE STOPPED ##");
  tA2DP_CTRL_CMD pending_cmd = btif_a2dp_get_pending_command();

  if (btif_a2dp_source_state == BTIF_A2DP_SOURCE_STATE_OFF) return;

  /* allow using this api for other than suspend */
  if (p_av_suspend != NULL) {
    if (p_av_suspend->status != BTA_AV_SUCCESS) {
      APPL_TRACE_EVENT("AV STOP FAILED (%d)", p_av_suspend->status);
      if (p_av_suspend->initiator) {
        APPL_TRACE_WARNING("%s: A2DP stop request failed: status = %d",
                           __func__, p_av_suspend->status);
        if ((pending_cmd == A2DP_CTRL_CMD_STOP) ||
            (pending_cmd == A2DP_CTRL_CMD_SUSPEND)) {
          btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
          if (property_get("persist.vendor.bt.a2dp.hal.implementation", a2dp_hal_imp, "false") &&
              !strcmp(a2dp_hal_imp, "true")) {
            btif_a2dp_pending_cmds_reset();
            int index = ((p_av_suspend->hndl) & BTA_AV_HNDL_MSK) - 1;
            RawAddress addr = btif_av_get_addr_by_index(index);
            if (!addr.IsEmpty()) {
              btif_dispatch_sm_event(BTIF_AV_DISCONNECT_REQ_EVT, (void *)addr.address,
                  sizeof(RawAddress));
              BTIF_TRACE_DEBUG("%s: Disconnect for peer device on Start fail by Remote",__func__);
            }
          }
        }
      }
      return;
    }
  }

  /* ensure tx frames are immediately suspended */
  btif_a2dp_source_cb.tx_flush = true;

  /* request to stop media task */
  btif_a2dp_source_audio_tx_flush_req();
  btif_a2dp_source_stop_audio_req();

  /* once stream is fully stopped we will ack back */
}

void btif_a2dp_source_on_suspended(tBTA_AV_SUSPEND* p_av_suspend) {
  APPL_TRACE_EVENT("## ON A2DP SOURCE SUSPENDED ##");
  tA2DP_CTRL_CMD pending_cmd = btif_a2dp_get_pending_command();

  if (btif_a2dp_source_state == BTIF_A2DP_SOURCE_STATE_OFF) return;

  /* check for status failures */
  if (p_av_suspend != NULL) {
    if (p_av_suspend->status != BTA_AV_SUCCESS) {
      if (p_av_suspend->initiator) {
        APPL_TRACE_WARNING("%s: A2DP suspend request failed: status = %d",
                         __func__, p_av_suspend->status);
        if ((pending_cmd == A2DP_CTRL_CMD_STOP) ||
            (pending_cmd == A2DP_CTRL_CMD_SUSPEND)) {
          btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
          if (property_get("persist.vendor.bt.a2dp.hal.implementation", a2dp_hal_imp, "false") &&
              !strcmp(a2dp_hal_imp, "true")) {
            btif_a2dp_pending_cmds_reset();
            int index = ((p_av_suspend->hndl) & BTA_AV_HNDL_MSK) - 1;
            RawAddress addr = btif_av_get_addr_by_index(index);
            if (!addr.IsEmpty()) {
              btif_dispatch_sm_event(BTIF_AV_DISCONNECT_REQ_EVT, (void *)addr.address,
                  sizeof(RawAddress));
              BTIF_TRACE_DEBUG("%s: Disconnect for peer device on Start fail by Remote",__func__);
            }
          }
        }
      }
    }
  }

  /* once stream is fully stopped we will ack back */

  /* ensure tx frames are immediately flushed */
  btif_a2dp_source_cb.tx_flush = true;

  /* stop timer tick */
  btif_a2dp_source_stop_audio_req();
}

/* when true media task discards any tx frames */
void btif_a2dp_source_set_tx_flush(bool enable) {
  APPL_TRACE_EVENT("## DROP TX %d ##", enable);
  btif_a2dp_source_cb.tx_flush = enable;
}

static void btif_a2dp_source_audio_tx_start_event(void) {
  APPL_TRACE_DEBUG(
      "%s media_alarm is %srunning, streaming %s", __func__,
      alarm_is_scheduled(btif_a2dp_source_cb.media_alarm) ? "" : "not ",
      btif_a2dp_source_is_streaming() ? "true" : "false");

  /* Reset the media feeding state */
  CHECK(btif_a2dp_source_cb.encoder_interface != NULL);
  btif_a2dp_source_cb.encoder_interface->feeding_reset();

  APPL_TRACE_EVENT(
      "starting timer %dms",
      btif_a2dp_source_cb.encoder_interface->get_encoder_interval_ms());

  alarm_free(btif_a2dp_source_cb.media_alarm);
  btif_a2dp_source_cb.media_alarm =
      alarm_new_periodic("btif.a2dp_source_media_alarm");
  if (btif_a2dp_source_cb.media_alarm == NULL) {
    LOG_ERROR(LOG_TAG, "%s unable to allocate media alarm", __func__);
    return;
  }

  alarm_set(btif_a2dp_source_cb.media_alarm,
            btif_a2dp_source_cb.encoder_interface->get_encoder_interval_ms(),
            btif_a2dp_source_alarm_cb, NULL);
}

static void btif_a2dp_source_audio_tx_stop_event(void) {
  APPL_TRACE_DEBUG(
      "%s media_alarm is %srunning, streaming %s", __func__,
      alarm_is_scheduled(btif_a2dp_source_cb.media_alarm) ? "" : "not ",
      btif_a2dp_source_is_streaming() ? "true" : "false");

  const bool send_ack = btif_a2dp_source_is_streaming()| btif_a2dp_source_is_remote_start();

  uint8_t p_buf[AUDIO_STREAM_OUTPUT_BUFFER_SZ * 2];
  uint16_t event;

  // Keep track of audio data still left in the pipe
  btif_a2dp_control_log_bytes_read(
      UIPC_Read(UIPC_CH_ID_AV_AUDIO, &event, p_buf, sizeof(p_buf)));

  /* Stop the timer first */
  alarm_free(btif_a2dp_source_cb.media_alarm);
  btif_a2dp_source_cb.media_alarm = NULL;

  UIPC_Close(UIPC_CH_ID_AV_AUDIO);

  /*
   * Try to send acknowldegment once the media stream is
   * stopped. This will make sure that the A2DP HAL layer is
   * un-blocked on wait for acknowledgment for the sent command.
   * This resolves a corner cases AVDTP SUSPEND collision
   * when the DUT and the remote device issue SUSPEND simultaneously
   * and due to the processing of the SUSPEND request from the remote,
   * the media path is torn down. If the A2DP HAL happens to wait
   * for ACK for the initiated SUSPEND, it would never receive it casuing
   * a block/wait. Due to this acknowledgement, the A2DP HAL is guranteed
   * to get the ACK for any pending command in such cases.
   */
  tA2DP_CTRL_CMD pending_cmd = btif_a2dp_get_pending_command();
  if (send_ack && (pending_cmd == A2DP_CTRL_CMD_SUSPEND ||
          pending_cmd == A2DP_CTRL_CMD_STOP)) {
    APPL_TRACE_DEBUG("%s, Ack for pending Stop/Suspend", __func__);
    btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
  } else if (pending_cmd == A2DP_CTRL_CMD_START) {
    BTIF_TRACE_ERROR("Ack Pending Start while Disconnect in Progress");
    btif_a2dp_command_ack(A2DP_CTRL_ACK_DISCONNECT_IN_PROGRESS);
  } else {
    BTIF_TRACE_ERROR("Invalid cmd pending for ack");
  }
  /* audio engine stopped, reset tx suspended flag */
  btif_a2dp_source_cb.tx_flush = false;

  /* Reset the media feeding state */
  if (btif_a2dp_source_cb.encoder_interface != NULL)
    btif_a2dp_source_cb.encoder_interface->feeding_reset();
}

static void btif_a2dp_source_alarm_cb(UNUSED_ATTR void* context) {
  thread_post(btif_a2dp_source_cb.worker_thread,
              btif_a2dp_source_audio_handle_timer, NULL);
}

static void btif_a2dp_source_audio_handle_timer(UNUSED_ATTR void* context) {
  uint64_t timestamp_us = time_get_os_boottime_us();
  log_tstamps_us("A2DP Source tx timer", timestamp_us);

  if (alarm_is_scheduled(btif_a2dp_source_cb.media_alarm)) {
    CHECK(btif_a2dp_source_cb.encoder_interface != NULL);
    size_t transmit_queue_length =
        fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue);
#ifndef OS_GENERIC
    ATRACE_INT("btif TX queue", transmit_queue_length);
#endif
    if (btif_a2dp_source_cb.encoder_interface->set_transmit_queue_length !=
        NULL) {
      btif_a2dp_source_cb.encoder_interface->set_transmit_queue_length(
          transmit_queue_length);
    }
    btif_a2dp_source_cb.encoder_interface->send_frames(timestamp_us);
    bta_av_ci_src_data_ready(BTA_AV_CHNL_AUDIO);
    update_scheduling_stats(&btif_a2dp_source_cb.stats.tx_queue_enqueue_stats,
                            timestamp_us,
                            btif_a2dp_source_cb.encoder_interval_ms * 1000);
  } else {
    APPL_TRACE_ERROR("ERROR Media task Scheduled after Suspend");
  }
}

static uint32_t btif_a2dp_source_read_callback(uint8_t* p_buf, uint32_t len) {
  uint16_t event;
  uint32_t bytes_read = UIPC_Read(UIPC_CH_ID_AV_AUDIO, &event, p_buf, len);

  if (bytes_read < len) {
    LOG_WARN(LOG_TAG, "%s: UNDERFLOW: ONLY READ %d BYTES OUT OF %d", __func__,
             bytes_read, len);
    btif_a2dp_source_cb.stats.media_read_total_underflow_bytes +=
        (len - bytes_read);
    btif_a2dp_source_cb.stats.media_read_total_underflow_count++;
    btif_a2dp_source_cb.stats.media_read_last_underflow_us =
        time_get_os_boottime_us();
  }

  return bytes_read;
}

static bool btif_a2dp_source_enqueue_callback(BT_HDR* p_buf, size_t frames_n,
                                              uint32_t bytes_read) {
  uint64_t now_us = time_get_os_boottime_us();
  btif_a2dp_control_log_bytes_read(bytes_read);

  /* Check if timer was stopped (media task stopped) */
  if (!alarm_is_scheduled(btif_a2dp_source_cb.media_alarm)) {
    osi_free(p_buf);
    return false;
  }

  /* Check if the transmission queue has been flushed */
  if (btif_a2dp_source_cb.tx_flush) {
    LOG_VERBOSE(LOG_TAG, "%s: tx suspended, discarded frame", __func__);

    btif_a2dp_source_cb.stats.tx_queue_total_flushed_messages +=
        fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue);
    btif_a2dp_source_cb.stats.tx_queue_last_flushed_us = now_us;
    fixed_queue_flush(btif_a2dp_source_cb.tx_audio_queue, osi_free);

    osi_free(p_buf);
    return false;
  }

  // Check for TX queue overflow
  // TODO: Using frames_n here is probably wrong: should be "+ 1" instead.
  if (fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue) + frames_n >
      MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ) {
    LOG_WARN(LOG_TAG, "%s: TX queue buffer size now=%u adding=%u max=%d",
             __func__,
             (uint32_t)fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue),
             (uint32_t)frames_n, MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ);
    // Keep track of drop-outs
    btif_a2dp_source_cb.stats.tx_queue_dropouts++;
    btif_a2dp_source_cb.stats.tx_queue_last_dropouts_us = now_us;

    // Flush all queued buffers
    size_t drop_n = fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue);
    btif_a2dp_source_cb.stats.tx_queue_max_dropped_messages = std::max(
        drop_n, btif_a2dp_source_cb.stats.tx_queue_max_dropped_messages);
    while (fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue)) {
      btif_a2dp_source_cb.stats.tx_queue_total_dropped_messages++;
      osi_free(fixed_queue_try_dequeue(btif_a2dp_source_cb.tx_audio_queue));
    }

    // Request RSSI and Failed Contact Counter for log purposes if we had to
    // flush buffers.
    RawAddress peer_bda;
    btif_av_get_active_peer_addr(&peer_bda);
    tBTM_STATUS status = BTM_ReadRSSI(peer_bda, btm_read_rssi_cb);
    if (status != BTM_CMD_STARTED) {
      LOG_WARN(LOG_TAG, "%s: Cannot read RSSI: status %d", __func__, status);
    }
    status = BTM_ReadFailedContactCounter(peer_bda,
                                          btm_read_failed_contact_counter_cb);
    if (status != BTM_CMD_STARTED) {
      LOG_WARN(LOG_TAG, "%s: Cannot read Failed Contact Counter: status %d",
               __func__, status);
    }
    status = BTM_ReadAutomaticFlushTimeout(peer_bda,
                                           btm_read_automatic_flush_timeout_cb);
    if (status != BTM_CMD_STARTED) {
      LOG_WARN(LOG_TAG, "%s: Cannot read Automatic Flush Timeout: status %d",
               __func__, status);
    }
    status =
        BTM_ReadTxPower(peer_bda, BT_TRANSPORT_BR_EDR, btm_read_tx_power_cb);
    if (status != BTM_CMD_STARTED) {
      LOG_WARN(LOG_TAG, "%s: Cannot read Tx Power: status %d", __func__,
               status);
    }
  }

  /* Update the statistics */
  btif_a2dp_source_cb.stats.tx_queue_total_frames += frames_n;
  btif_a2dp_source_cb.stats.tx_queue_max_frames_per_packet = std::max(
      frames_n, btif_a2dp_source_cb.stats.tx_queue_max_frames_per_packet);
  CHECK(btif_a2dp_source_cb.encoder_interface != NULL);

  fixed_queue_enqueue(btif_a2dp_source_cb.tx_audio_queue, p_buf);

  return true;
}

static void btif_a2dp_source_audio_tx_flush_event(UNUSED_ATTR BT_HDR* p_msg) {
  /* Flush all enqueued audio buffers (encoded) */
  APPL_TRACE_DEBUG("%s", __func__);

  if (btif_a2dp_source_cb.encoder_interface != NULL)
    btif_a2dp_source_cb.encoder_interface->feeding_flush();

  btif_a2dp_source_cb.stats.tx_queue_total_flushed_messages +=
      fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue);
  btif_a2dp_source_cb.stats.tx_queue_last_flushed_us =
      time_get_os_boottime_us();
  fixed_queue_flush(btif_a2dp_source_cb.tx_audio_queue, osi_free);

  UIPC_Ioctl(UIPC_CH_ID_AV_AUDIO, UIPC_REQ_RX_FLUSH, NULL);
}

static bool btif_a2dp_source_audio_tx_flush_req(void) {
  BT_HDR* p_buf = (BT_HDR*)osi_malloc(sizeof(BT_HDR));

  p_buf->event = BTIF_MEDIA_AUDIO_TX_FLUSH;

  /*
   * Explicitly check whether the btif_a2dp_source_cb.cmd_msg_queue is not
   * NULL to avoid a race condition during shutdown of the Bluetooth stack.
   * This race condition is triggered when A2DP audio is streaming on
   * shutdown:
   * "btif_a2dp_source_on_stopped() -> btif_a2dp_source_audio_tx_flush_req()"
   * is called to stop the particular audio stream, and this happens right
   * after the "BTIF_AV_CLEANUP_REQ_EVT -> btif_a2dp_source_shutdown()"
   * processing during the shutdown of the Bluetooth stack.
   */
  if (btif_a2dp_source_cb.cmd_msg_queue != NULL)
    fixed_queue_enqueue(btif_a2dp_source_cb.cmd_msg_queue, p_buf);

  return true;
}

BT_HDR* btif_a2dp_source_audio_readbuf(void) {
  uint64_t now_us = time_get_os_boottime_us();
  BT_HDR* p_buf =
      (BT_HDR*)fixed_queue_try_dequeue(btif_a2dp_source_cb.tx_audio_queue);

  btif_a2dp_source_cb.stats.tx_queue_total_readbuf_calls++;
  btif_a2dp_source_cb.stats.tx_queue_last_readbuf_us = now_us;
  if (p_buf != NULL) {
    // Update the statistics
    update_scheduling_stats(&btif_a2dp_source_cb.stats.tx_queue_dequeue_stats,
                            now_us,
                            btif_a2dp_source_cb.encoder_interval_ms * 1000);
  }

  return p_buf;
}

static void log_tstamps_us(const char* comment, uint64_t timestamp_us) {
  static uint64_t prev_us = 0;
  APPL_TRACE_DEBUG("[%s] ts %08llu, diff : %08llu, queue sz %d", comment,
                   timestamp_us, timestamp_us - prev_us,
                   fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue));
  prev_us = timestamp_us;
}

static void update_scheduling_stats(scheduling_stats_t* stats, uint64_t now_us,
                                    uint64_t expected_delta) {
  uint64_t last_us = stats->last_update_us;

  stats->total_updates++;
  stats->last_update_us = now_us;

  if (last_us == 0) return;  // First update: expected delta doesn't apply

  uint64_t deadline_us = last_us + expected_delta;
  if (deadline_us < now_us) {
    // Overdue scheduling
    uint64_t delta_us = now_us - deadline_us;
    // Ignore extreme outliers
    if (delta_us < 10 * expected_delta) {
      stats->max_overdue_scheduling_delta_us =
          std::max(delta_us, stats->max_overdue_scheduling_delta_us);
      stats->total_overdue_scheduling_delta_us += delta_us;
      stats->overdue_scheduling_count++;
      stats->total_scheduling_time_us += now_us - last_us;
    }
  } else if (deadline_us > now_us) {
    // Premature scheduling
    uint64_t delta_us = deadline_us - now_us;
    // Ignore extreme outliers
    if (delta_us < 10 * expected_delta) {
      stats->max_premature_scheduling_delta_us =
          std::max(delta_us, stats->max_premature_scheduling_delta_us);
      stats->total_premature_scheduling_delta_us += delta_us;
      stats->premature_scheduling_count++;
      stats->total_scheduling_time_us += now_us - last_us;
    }
  } else {
    // On-time scheduling
    stats->exact_scheduling_count++;
    stats->total_scheduling_time_us += now_us - last_us;
  }
}

void btif_a2dp_source_debug_dump(int fd) {
  btif_a2dp_source_accumulate_stats(&btif_a2dp_source_cb.stats,
                                    &btif_a2dp_source_cb.accumulated_stats);
  uint64_t now_us = time_get_os_boottime_us();
  btif_media_stats_t* accumulated_stats =
      &btif_a2dp_source_cb.accumulated_stats;
  scheduling_stats_t* enqueue_stats =
      &accumulated_stats->tx_queue_enqueue_stats;
  scheduling_stats_t* dequeue_stats =
      &accumulated_stats->tx_queue_dequeue_stats;
  size_t ave_size;
  uint64_t ave_time_us;

  dprintf(fd, "\nA2DP State:\n");
  dprintf(fd, "  TxQueue:\n");

  dprintf(fd,
          "  Counts (enqueue/dequeue/readbuf)                        : %zu / "
          "%zu / %zu\n",
          enqueue_stats->total_updates, dequeue_stats->total_updates,
          accumulated_stats->tx_queue_total_readbuf_calls);

  dprintf(
      fd,
      "  Last update time ago in ms (enqueue/dequeue/readbuf)    : %llu / %llu "
      "/ %llu\n",
      (enqueue_stats->last_update_us > 0)
          ? (unsigned long long)(now_us - enqueue_stats->last_update_us) / 1000
          : 0,
      (dequeue_stats->last_update_us > 0)
          ? (unsigned long long)(now_us - dequeue_stats->last_update_us) / 1000
          : 0,
      (accumulated_stats->tx_queue_last_readbuf_us > 0)
          ? (unsigned long long)(now_us -
                                 accumulated_stats->tx_queue_last_readbuf_us) /
                1000
          : 0);

  ave_size = 0;
  if (enqueue_stats->total_updates != 0)
    ave_size =
        accumulated_stats->tx_queue_total_frames / enqueue_stats->total_updates;
  dprintf(fd,
          "  Frames per packet (total/max/ave)                       : %zu / "
          "%zu / %zu\n",
          accumulated_stats->tx_queue_total_frames,
          accumulated_stats->tx_queue_max_frames_per_packet, ave_size);

  dprintf(fd,
          "  Counts (flushed/dropped/dropouts)                       : %zu / "
          "%zu / %zu\n",
          accumulated_stats->tx_queue_total_flushed_messages,
          accumulated_stats->tx_queue_total_dropped_messages,
          accumulated_stats->tx_queue_dropouts);

  dprintf(fd,
          "  Counts (max dropped)                                    : %zu\n",
          accumulated_stats->tx_queue_max_dropped_messages);

  dprintf(
      fd,
      "  Last update time ago in ms (flushed/dropped)            : %llu / "
      "%llu\n",
      (accumulated_stats->tx_queue_last_flushed_us > 0)
          ? (unsigned long long)(now_us -
                                 accumulated_stats->tx_queue_last_flushed_us) /
                1000
          : 0,
      (accumulated_stats->tx_queue_last_dropouts_us > 0)
          ? (unsigned long long)(now_us -
                                 accumulated_stats->tx_queue_last_dropouts_us) /
                1000
          : 0);

  dprintf(fd,
          "  Counts (underflow)                                      : %zu\n",
          accumulated_stats->media_read_total_underflow_count);

  dprintf(fd,
          "  Bytes (underflow)                                       : %zu\n",
          accumulated_stats->media_read_total_underflow_bytes);

  dprintf(fd,
          "  Last update time ago in ms (underflow)                  : %llu\n",
          (accumulated_stats->media_read_last_underflow_us > 0)
              ? (unsigned long long)(now_us -
                                     accumulated_stats
                                         ->media_read_last_underflow_us) /
                    1000
              : 0);

  //
  // TxQueue enqueue stats
  //
  dprintf(
      fd,
      "  Enqueue deviation counts (overdue/premature)            : %zu / %zu\n",
      enqueue_stats->overdue_scheduling_count,
      enqueue_stats->premature_scheduling_count);

  ave_time_us = 0;
  if (enqueue_stats->overdue_scheduling_count != 0) {
    ave_time_us = enqueue_stats->total_overdue_scheduling_delta_us /
                  enqueue_stats->overdue_scheduling_count;
  }
  dprintf(
      fd,
      "  Enqueue overdue scheduling time in ms (total/max/ave)   : %llu / %llu "
      "/ %llu\n",
      (unsigned long long)enqueue_stats->total_overdue_scheduling_delta_us /
          1000,
      (unsigned long long)enqueue_stats->max_overdue_scheduling_delta_us / 1000,
      (unsigned long long)ave_time_us / 1000);

  ave_time_us = 0;
  if (enqueue_stats->premature_scheduling_count != 0) {
    ave_time_us = enqueue_stats->total_premature_scheduling_delta_us /
                  enqueue_stats->premature_scheduling_count;
  }
  dprintf(
      fd,
      "  Enqueue premature scheduling time in ms (total/max/ave) : %llu / %llu "
      "/ %llu\n",
      (unsigned long long)enqueue_stats->total_premature_scheduling_delta_us /
          1000,
      (unsigned long long)enqueue_stats->max_premature_scheduling_delta_us /
          1000,
      (unsigned long long)ave_time_us / 1000);

  //
  // TxQueue dequeue stats
  //
  dprintf(
      fd,
      "  Dequeue deviation counts (overdue/premature)            : %zu / %zu\n",
      dequeue_stats->overdue_scheduling_count,
      dequeue_stats->premature_scheduling_count);

  ave_time_us = 0;
  if (dequeue_stats->overdue_scheduling_count != 0) {
    ave_time_us = dequeue_stats->total_overdue_scheduling_delta_us /
                  dequeue_stats->overdue_scheduling_count;
  }
  dprintf(
      fd,
      "  Dequeue overdue scheduling time in ms (total/max/ave)   : %llu / %llu "
      "/ %llu\n",
      (unsigned long long)dequeue_stats->total_overdue_scheduling_delta_us /
          1000,
      (unsigned long long)dequeue_stats->max_overdue_scheduling_delta_us / 1000,
      (unsigned long long)ave_time_us / 1000);

  ave_time_us = 0;
  if (dequeue_stats->premature_scheduling_count != 0) {
    ave_time_us = dequeue_stats->total_premature_scheduling_delta_us /
                  dequeue_stats->premature_scheduling_count;
  }
  dprintf(
      fd,
      "  Dequeue premature scheduling time in ms (total/max/ave) : %llu / %llu "
      "/ %llu\n",
      (unsigned long long)dequeue_stats->total_premature_scheduling_delta_us /
          1000,
      (unsigned long long)dequeue_stats->max_premature_scheduling_delta_us /
          1000,
      (unsigned long long)ave_time_us / 1000);

  //
  // Codec-specific stats
  //
  A2dpCodecs* a2dp_codecs = bta_av_get_a2dp_codecs();
  if (a2dp_codecs != nullptr) {
    a2dp_codecs->debug_codec_dump(fd);
  }
}

void btif_a2dp_source_update_metrics(void) {
  btif_media_stats_t* stats = &btif_a2dp_source_cb.stats;
  scheduling_stats_t* enqueue_stats = &stats->tx_queue_enqueue_stats;
  A2dpSessionMetrics metrics;
  // session_start_us is 0 when btif_a2dp_source_start_audio_req() is not called
  // mark the metric duration as invalid (-1) in this case
  if (stats->session_start_us != 0) {
    int64_t session_end_us = stats->session_end_us == 0
                                 ? time_get_os_boottime_us()
                                 : stats->session_end_us;
    metrics.audio_duration_ms =
        (session_end_us - stats->session_start_us) / 1000;
  }

  if (enqueue_stats->total_updates > 1) {
    metrics.media_timer_min_ms =
        btif_a2dp_source_cb.encoder_interval_ms -
        (enqueue_stats->max_premature_scheduling_delta_us / 1000);
    metrics.media_timer_max_ms =
        btif_a2dp_source_cb.encoder_interval_ms +
        (enqueue_stats->max_overdue_scheduling_delta_us / 1000);

    metrics.total_scheduling_count = enqueue_stats->overdue_scheduling_count +
                                     enqueue_stats->premature_scheduling_count +
                                     enqueue_stats->exact_scheduling_count;
    if (metrics.total_scheduling_count > 0) {
      metrics.media_timer_avg_ms = enqueue_stats->total_scheduling_time_us /
                                   (1000 * metrics.total_scheduling_count);
    }

    metrics.buffer_overruns_max_count = stats->tx_queue_max_dropped_messages;
    metrics.buffer_overruns_total = stats->tx_queue_total_dropped_messages;
    metrics.buffer_underruns_count = stats->media_read_total_underflow_count;
    metrics.buffer_underruns_average = 0;
    if (metrics.buffer_underruns_count > 0) {
      metrics.buffer_underruns_average =
          stats->media_read_total_underflow_bytes /
          metrics.buffer_underruns_count;
    }
  }
  BluetoothMetricsLogger::GetInstance()->LogA2dpSession(metrics);
}

static void btm_read_rssi_cb(void* data) {
  if (data == nullptr) {
    LOG_ERROR(LOG_TAG, "%s Read RSSI request timed out", __func__);
    return;
  }

  tBTM_RSSI_RESULT* result = (tBTM_RSSI_RESULT*)data;
  if (result->status != BTM_SUCCESS) {
    LOG_ERROR(LOG_TAG, "%s unable to read remote RSSI (status %d)", __func__,
              result->status);
    return;
  }

  LOG_WARN(LOG_TAG, "%s device: %s, rssi: %d", __func__,
           result->rem_bda.ToString().c_str(), result->rssi);
}

static void btm_read_failed_contact_counter_cb(void* data) {
  if (data == nullptr) {
    LOG_ERROR(LOG_TAG, "%s Read Failed Contact Counter request timed out",
              __func__);
    return;
  }

  tBTM_FAILED_CONTACT_COUNTER_RESULT* result =
      (tBTM_FAILED_CONTACT_COUNTER_RESULT*)data;
  if (result->status != BTM_SUCCESS) {
    LOG_ERROR(LOG_TAG, "%s unable to read Failed Contact Counter (status %d)",
              __func__, result->status);
    return;
  }

  LOG_WARN(LOG_TAG, "%s device: %s, Failed Contact Counter: %u", __func__,
           result->rem_bda.ToString().c_str(), result->failed_contact_counter);
}

static void btm_read_automatic_flush_timeout_cb(void* data) {
  if (data == nullptr) {
    LOG_ERROR(LOG_TAG, "%s Read Automatic Flush Timeout request timed out",
              __func__);
    return;
  }

  tBTM_AUTOMATIC_FLUSH_TIMEOUT_RESULT* result =
      (tBTM_AUTOMATIC_FLUSH_TIMEOUT_RESULT*)data;
  if (result->status != BTM_SUCCESS) {
    LOG_ERROR(LOG_TAG, "%s unable to read Automatic Flush Timeout (status %d)",
              __func__, result->status);
    return;
  }

  LOG_WARN(LOG_TAG, "%s device: %s, Automatic Flush Timeout: %u", __func__,
           result->rem_bda.ToString().c_str(), result->automatic_flush_timeout);
}

static void btm_read_tx_power_cb(void* data) {
  if (data == nullptr) {
    LOG_ERROR(LOG_TAG, "%s Read Tx Power request timed out", __func__);
    return;
  }

  tBTM_TX_POWER_RESULT* result = (tBTM_TX_POWER_RESULT*)data;
  if (result->status != BTM_SUCCESS) {
    LOG_ERROR(LOG_TAG, "%s unable to read Tx Power (status %d)", __func__,
              result->status);
    return;
  }

  LOG_WARN(LOG_TAG, "%s device: %s, Tx Power: %d", __func__,
           result->rem_bda.ToString().c_str(), result->tx_power);
}

#include <pebble.h>
#include "comm.h"
#include "../app_settings.h"
#include <time.h>

typedef void(MsgReceivedCallback)(void);
typedef void(PreviewDataCallback)(uint8_t *data, size_t length);
typedef void(SendResultCallback)(bool success);

static AppTimer *response_wait_timer;
static MsgReceivedCallback *picture_taken_callback;
static PreviewDataCallback *preview_data_callback;
static SendResultCallback *pending_send_result_callback = NULL;
static SendResultCallback *pending_capture_ack_callback = NULL;
static AppTimer *capture_ack_timeout_timer = NULL;
static bool s_capture_in_progress = false;
static bool s_outbox_pending = false;

// Last AppMessage failure and whether the phone ever acknowledged a message. Shown on the
// waiting screen: "App open?" alone can't tell a watch that can't send apart from a phone
// that gets the requests but sends no image.
static AppMessageResult s_last_error = APP_MSG_OK;
static bool s_phone_reached = false;

// A capture queued behind a busy outbox gives up after this long instead of waiting silently
#define PENDING_CAPTURE_TIMEOUT_MS 3000
static AppTimer *s_pending_capture_timer = NULL;

// Pending capture state: when outbox is busy, we store the capture args here
// and send them when the outbox clears
typedef struct {
  bool pending;
  int timer_seconds;
} PendingCapture;

static PendingCapture s_pending_capture = {
  .pending = false,
  .timer_seconds = 0,
};

static void clear_capture_in_progress(void) {
  s_capture_in_progress = false;
  s_pending_capture.pending = false;
  s_pending_capture.timer_seconds = 0;
  if (s_pending_capture_timer) {
    app_timer_cancel(s_pending_capture_timer);
    s_pending_capture_timer = NULL;
  }
}

static void prv_fail_capture(void) {
  if (pending_capture_ack_callback) {
    pending_capture_ack_callback(false);
    pending_capture_ack_callback = NULL;
  }
  clear_capture_in_progress();
}

static void pending_capture_timeout_handler(void *data) {
  s_pending_capture_timer = NULL;
  APP_LOG(APP_LOG_LEVEL_WARNING, "pending_capture_timeout_handler: outbox never cleared, giving up on capture");
  prv_fail_capture();
}

static void capture_ack_timeout_handler(void *data) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "capture_ack_timeout_handler: Did not receive KEY_CAPTURE_ACK from companion app within timeout");
  capture_ack_timeout_timer = NULL;

  // Invoke callback with failure (silently handled in capture_send_result_callback)
  if (pending_capture_ack_callback) {
    pending_capture_ack_callback(false);
    pending_capture_ack_callback = NULL;
  }

  clear_capture_in_progress();
}

static char *translate_error(AppMessageResult result);

static void prv_record_error(AppMessageResult result) {
  s_last_error = result;
}

static char *translate_error(AppMessageResult result) {
  switch (result) {
    case APP_MSG_OK: return "APP_MSG_OK";
    case APP_MSG_SEND_TIMEOUT: return "APP_MSG_SEND_TIMEOUT";
    case APP_MSG_SEND_REJECTED: return "APP_MSG_SEND_REJECTED";
    case APP_MSG_NOT_CONNECTED: return "APP_MSG_NOT_CONNECTED";
    case APP_MSG_APP_NOT_RUNNING: return "APP_MSG_APP_NOT_RUNNING";
    case APP_MSG_INVALID_ARGS: return "APP_MSG_INVALID_ARGS";
    case APP_MSG_BUSY: return "APP_MSG_BUSY";
    case APP_MSG_BUFFER_OVERFLOW: return "APP_MSG_BUFFER_OVERFLOW";
    case APP_MSG_ALREADY_RELEASED: return "APP_MSG_ALREADY_RELEASED";
    case APP_MSG_CALLBACK_ALREADY_REGISTERED: return "APP_MSG_CALLBACK_ALREADY_REGISTERED";
    case APP_MSG_CALLBACK_NOT_REGISTERED: return "APP_MSG_CALLBACK_NOT_REGISTERED";
    case APP_MSG_OUT_OF_MEMORY: return "APP_MSG_OUT_OF_MEMORY";
    case APP_MSG_CLOSED: return "APP_MSG_CLOSED";
    case APP_MSG_INTERNAL_ERROR: return "APP_MSG_INTERNAL_ERROR";
    case APP_MSG_INVALID_STATE: return "APP_MSG_INVALID_STATE";
    default: return "UNKNOWN ERROR";
  }
}

void send_int_app_message_with_callback(int key, int message, void *timeout_handler) {
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);

  dict_write_int(iter, key, &message, sizeof(int), true);

  AppMessageResult send_result_code = app_message_outbox_send();

  if (send_result_code == APP_MSG_OK) {

    response_wait_timer = app_timer_register(APP_MESSAGE_TIMEOUT, timeout_handler, NULL);
  } else {
    APP_LOG(APP_LOG_LEVEL_INFO, "Failed initial send with: %s\n", translate_error(send_result_code));

    if (timeout_handler) {
      AppTimerCallback handler = timeout_handler;
      // Call timeout_handler
      handler(NULL);
    }
  }
}

void send_int_app_message_with_result_callback(int key, int message, SendResultCallback *result_callback, void *timeout_handler) {
  DictionaryIterator *iter;
  AppMessageResult begin_result = app_message_outbox_begin(&iter);

  if (begin_result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "send_int_app_message_with_result_callback: app_message_outbox_begin failed: %s", translate_error(begin_result));
    if (result_callback) {
      result_callback(false);
    }
    return;
  }

  dict_write_int(iter, key, &message, sizeof(int), true);

  AppMessageResult send_result_code = app_message_outbox_send();

  if (send_result_code == APP_MSG_OK) {
    pending_send_result_callback = result_callback;
    response_wait_timer = app_timer_register(APP_MESSAGE_TIMEOUT, timeout_handler, NULL);
  } else {
    APP_LOG(APP_LOG_LEVEL_WARNING, "send_int_app_message_with_result_callback: Failed initial send with error: %s", translate_error(send_result_code));

    // Call result callback with failure
    if (result_callback) {
      result_callback(false);
    }

    if (timeout_handler) {
      AppTimerCallback handler = timeout_handler;
      handler(NULL);
    }
  }
}

static void prv_send_capture_internal(int timer_seconds) {
  DictionaryIterator *iter;
  AppMessageResult begin_result = app_message_outbox_begin(&iter);

  if (begin_result == APP_MSG_BUSY) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "prv_send_capture_internal: outbox busy, capture pending");
    // Store as pending - will be retried when outbox clears
    s_pending_capture.pending = true;
    s_pending_capture.timer_seconds = timer_seconds;
    if (!s_pending_capture_timer) {
      s_pending_capture_timer = app_timer_register(PENDING_CAPTURE_TIMEOUT_MS, pending_capture_timeout_handler, NULL);
    }
    return;
  }
  if (begin_result != APP_MSG_OK) {
    // Anything but busy won't clear by waiting. Queuing the capture used to swallow the
    // press without any feedback.
    APP_LOG(APP_LOG_LEVEL_ERROR, "prv_send_capture_internal: app_message_outbox_begin failed: %s", translate_error(begin_result));
    prv_record_error(begin_result);
    prv_fail_capture();
    return;
  }

  // Capture format: [byte 0: reserved] [bytes 1-4: timestamp] [bytes 5-7: timer_seconds]
  uint32_t timestamp = (uint32_t)time(NULL);
  uint8_t capture_request[8] = {
    0x00,                                   // Byte 0: reserved
    (timestamp & 0xFF),                     // Byte 1: timestamp LSB
    ((timestamp >> 8) & 0xFF),              // Byte 2: timestamp
    ((timestamp >> 16) & 0xFF),             // Byte 3: timestamp
    ((timestamp >> 24) & 0xFF),             // Byte 4: timestamp MSB
    (timer_seconds & 0xFF),                 // Byte 5: timer LSB
    ((timer_seconds >> 8) & 0xFF),          // Byte 6: timer middle
    ((timer_seconds >> 16) & 0xFF)          // Byte 7: timer MSB
  };

  dict_write_data(iter, KEY_CAPTURE, capture_request, sizeof(capture_request));

  AppMessageResult send_result_code = app_message_outbox_send();

  if (send_result_code == APP_MSG_OK) {
    s_outbox_pending = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "prv_send_capture_internal: Message queued in outbox, waiting for KEY_CAPTURE_ACK from companion app");
    // Set timeout to wait for ACK - using 2 second timeout since companion app should respond immediately
    capture_ack_timeout_timer = app_timer_register(2000, capture_ack_timeout_handler, NULL);
    s_pending_capture.pending = false;
    if (s_pending_capture_timer) {
      app_timer_cancel(s_pending_capture_timer);
      s_pending_capture_timer = NULL;
    }
  } else {
    APP_LOG(APP_LOG_LEVEL_WARNING, "prv_send_capture_internal: Failed to queue message in outbox: %s", translate_error(send_result_code));
    prv_record_error(send_result_code);
    prv_fail_capture();
  }
}

bool comm_capture_in_progress() {
  return s_capture_in_progress || s_pending_capture.pending;
}

bool comm_outbox_pending() {
  return s_outbox_pending;
}

void send_capture_with_ack(int timer_seconds, SendResultCallback *ack_callback) {
  s_capture_in_progress = true;
  pending_capture_ack_callback = ack_callback;

  prv_send_capture_internal(timer_seconds);
}

void register_picture_taken_callback(void *callback) {
  picture_taken_callback = callback;
}

void register_preview_data_callback(PreviewDataCallback *callback) {
  preview_data_callback = callback;
}

uint8_t model_name_to_enum(const char *model_name) {
  if (strcmp(model_name, "aplite") == 0) return 0;
  if (strcmp(model_name, "basalt") == 0) return 1;
  if (strcmp(model_name, "diorite") == 0) return 2;
  if (strcmp(model_name, "flint") == 0) return 3;
  if (strcmp(model_name, "chalk") == 0) return 4;
  if (strcmp(model_name, "emery") == 0) return 5;
  return 1; // Default to basalt
}

void send_request_next_frame(uint8_t model_enum, uint8_t format, uint8_t dithering_algorithm) {
  // Skip frame requests while capture is in progress or pending
  if (s_capture_in_progress) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "send_request_next_frame: skipping frame request (capture in progress)");
    return;
  }

  if (s_pending_capture.pending) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "send_request_next_frame: skipping frame request (pending capture waiting for outbox)");
    return;
  }

  DictionaryIterator *iter;
  AppMessageResult begin_result = app_message_outbox_begin(&iter);

  if (begin_result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "send_request_next_frame: app_message_outbox_begin failed: %s", translate_error(begin_result));
    if (begin_result != APP_MSG_BUSY) {
      prv_record_error(begin_result);
    }
    return;
  }

  // Request format: [byte 0: reserved] [bytes 1-4: timestamp] [byte 5: model] [byte 6: format] [byte 7: dithering]
  uint32_t timestamp = (uint32_t)time(NULL);
  uint8_t frame_request[8] = {
    0x00,                                   // Byte 0: reserved
    (timestamp & 0xFF),                     // Byte 1: timestamp LSB
    ((timestamp >> 8) & 0xFF),              // Byte 2: timestamp
    ((timestamp >> 16) & 0xFF),             // Byte 3: timestamp
    ((timestamp >> 24) & 0xFF),             // Byte 4: timestamp MSB
    model_enum,                             // Byte 5: model
    format,                                 // Byte 6: format
    dithering_algorithm                     // Byte 7: dithering
  };

  dict_write_data(iter, KEY_REQUEST_NEXT_FRAME, frame_request, sizeof(frame_request));

  AppMessageResult send_result_code = app_message_outbox_send();

  if (send_result_code == APP_MSG_OK) {
    s_outbox_pending = true;
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "send_request_next_frame: outbox_send failed: %s", translate_error(send_result_code));
    prv_record_error(send_result_code);
  }
}

void send_request_next_chunk(uint8_t chunk_number, uint8_t format) {
  DictionaryIterator *iter;
  AppMessageResult begin_result = app_message_outbox_begin(&iter);

  if (begin_result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "send_request_next_chunk: app_message_outbox_begin failed: %s", translate_error(begin_result));
    if (begin_result != APP_MSG_BUSY) {
      prv_record_error(begin_result);
    }
    return;
  }

  // Request format: [byte 0: reserved] [bytes 1-4: timestamp] [byte 5: chunk_number] [byte 6: format]
  uint32_t timestamp = (uint32_t)time(NULL);
  uint8_t chunk_request[7] = {
    0x00,                                   // Byte 0: reserved
    (timestamp & 0xFF),                     // Byte 1: timestamp LSB
    ((timestamp >> 8) & 0xFF),              // Byte 2: timestamp
    ((timestamp >> 16) & 0xFF),             // Byte 3: timestamp
    ((timestamp >> 24) & 0xFF),             // Byte 4: timestamp MSB
    chunk_number,                           // Byte 5: chunk_number
    format                                  // Byte 6: format
  };

  dict_write_data(iter, KEY_REQUEST_NEXT_CHUNK, chunk_request, sizeof(chunk_request));

  AppMessageResult send_result_code = app_message_outbox_send();

  if (send_result_code == APP_MSG_OK) {
    s_outbox_pending = true;
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to request next chunk: %s", translate_error(send_result_code));
    prv_record_error(send_result_code);
  }
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  if (!iterator) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "inbox_received_callback: iterator is NULL");
    return;
  }

  s_phone_reached = true;
  s_last_error = APP_MSG_OK;

  Tuple *t = dict_read_first(iterator);

  if (!t) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "inbox_received_callback: no tuples in message");
    return;
  }

  int tuple_count = 0;
  while(t != NULL) {
    tuple_count++;

    switch(t->key) {
      case KEY_CAPTURE_ACK: {
        APP_LOG(APP_LOG_LEVEL_INFO, "KEY_CAPTURE_ACK received");
        if (t->type == TUPLE_BYTE_ARRAY && t->length >= 5) {
          uint8_t *data = (uint8_t *)t->value;
          uint32_t ack_timestamp = data[1] | ((uint32_t)data[2] << 8) | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
          uint32_t now = (uint32_t)time(NULL);

          // Calculate age with absolute value to allow for clock skew (both past and future timestamps)
          uint32_t age = (now >= ack_timestamp) ? (now - ack_timestamp) : (ack_timestamp - now);

          if (age <= STALE_MSG_THRESHOLD_S) {
            if (capture_ack_timeout_timer) {
              app_timer_cancel(capture_ack_timeout_timer);
              capture_ack_timeout_timer = NULL;
            }
            if (pending_capture_ack_callback) {
              APP_LOG(APP_LOG_LEVEL_DEBUG, "inbox: invoking pending_capture_ack_callback with true");
              pending_capture_ack_callback(true);
              pending_capture_ack_callback = NULL;
            }
            clear_capture_in_progress();
          } else {
            APP_LOG(APP_LOG_LEVEL_WARNING, "KEY_CAPTURE_ACK: dropping stale message (age=%lu)", (unsigned long)age);
          }
        } else {
          APP_LOG(APP_LOG_LEVEL_WARNING, "KEY_CAPTURE_ACK: invalid format, expected 5+ byte array");
        }
        break;
      }
      case KEY_PICTURE_TAKEN: {
        APP_LOG(APP_LOG_LEVEL_INFO, "KEY_PICTURE_TAKEN received");
        if (t->type == TUPLE_BYTE_ARRAY && t->length >= 5) {
          uint8_t *data = (uint8_t *)t->value;
          uint32_t taken_timestamp = data[1] | ((uint32_t)data[2] << 8) | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
          uint32_t now = (uint32_t)time(NULL);

          // Calculate age with absolute value to allow for clock skew (both past and future timestamps)
          uint32_t age = (now >= taken_timestamp) ? (now - taken_timestamp) : (taken_timestamp - now);

          if (age <= STALE_MSG_THRESHOLD_S) {
            if (picture_taken_callback) {
              picture_taken_callback();
            }
          } else {
            APP_LOG(APP_LOG_LEVEL_WARNING, "KEY_PICTURE_TAKEN: dropping stale message (age=%lu)", (unsigned long)age);
          }
        } else {
          APP_LOG(APP_LOG_LEVEL_WARNING, "KEY_PICTURE_TAKEN: invalid format, expected 5+ byte array");
        }
        break;
      }
      case KEY_PREVIEW_DATA:
        if (t->type == TUPLE_BYTE_ARRAY) {
          if (preview_data_callback) {
            preview_data_callback((uint8_t *)t->value, t->length);
          } else {
            APP_LOG(APP_LOG_LEVEL_WARNING, "KEY_PREVIEW_DATA: no callback registered");
          }
        } else {
          APP_LOG(APP_LOG_LEVEL_WARNING, "KEY_PREVIEW_DATA: incorrect type=%d, expected TUPLE_BYTE_ARRAY", t->type);
        }
        break;
      default:
        APP_LOG(APP_LOG_LEVEL_WARNING, "Unknown key: %d, type=%d, length=%zu", (int)t->key, t->type, t->length);
        break;
    }

    t = dict_read_next(iterator);
  }
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "inbox_dropped_handler: Inbox message dropped with error: %s (code: %d)", translate_error(reason), reason);
  prv_record_error(reason);
}

static void outbox_failed_handler(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  s_outbox_pending = false;
  APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_failed_handler: Outbox send failed with error: %s (code: %d)", translate_error(reason), reason);
  prv_record_error(reason);

  // If there's a pending result callback, call it with false to signal failure
  if (pending_send_result_callback) {
    pending_send_result_callback(false);
    pending_send_result_callback = NULL;
  }

  // Check if there's a pending capture - if so, try to send it now
  if (s_pending_capture.pending) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "outbox_failed_handler: attempting pending capture after outbox failure");
    prv_send_capture_internal(s_pending_capture.timer_seconds);
  }
}

static void outbox_sent_handler(DictionaryIterator *iterator, void *context) {
  s_outbox_pending = false;
  s_phone_reached = true;
  s_last_error = APP_MSG_OK;
  if (response_wait_timer) {
    app_timer_cancel(response_wait_timer);
    response_wait_timer = NULL;
  }

  // Call the pending result callback if one is set
  if (pending_send_result_callback) {
    pending_send_result_callback(true);
    pending_send_result_callback = NULL;
  }

  // Check if there's a pending capture waiting for the outbox to clear
  if (s_pending_capture.pending) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "outbox_sent_handler: outbox cleared, attempting pending capture");
    prv_send_capture_internal(s_pending_capture.timer_seconds);
  }
}

void init_comm() {
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_handler);
  app_message_register_outbox_sent(outbox_sent_handler);
  app_message_register_outbox_failed(outbox_failed_handler);

  // Calculate inbox size based on device type
  // For b/w devices: size is based on actual image data (1bit per pixel)
  // For color devices: the phone fills messages up to 8192 bytes of payload, which needs a
  // few bytes more for the dictionary header - so take the maximum rather than exactly 8192
  uint32_t inbox_max = app_message_inbox_size_maximum();
  uint32_t inbox_size = PBL_IF_COLOR_ELSE(
    inbox_max,
    ((PBL_DISPLAY_WIDTH * PBL_DISPLAY_HEIGHT) / 8) + 256
  );
  if (inbox_size > inbox_max) {
    inbox_size = inbox_max;
  }
  uint32_t outbox_size = 52;

  AppMessageResult result = app_message_open(inbox_size, outbox_size);
  APP_LOG(APP_LOG_LEVEL_INFO, "init_comm: app_message_open(%lu, %lu) -> %s (inbox max %lu)",
          (unsigned long)inbox_size, (unsigned long)outbox_size, translate_error(result), (unsigned long)inbox_max);
  if (result != APP_MSG_OK) {
    // Without an open outbox every send fails at app_message_outbox_begin(). A small inbox
    // can't take preview frames, but still lets captures and their acknowledgements through.
    prv_record_error(result);
    result = app_message_open(1024, outbox_size);
    APP_LOG(APP_LOG_LEVEL_WARNING, "init_comm: retry with small inbox -> %s", translate_error(result));
  }
}

AppMessageResult comm_last_error(void) {
  return s_last_error;
}

const char *comm_error_name(AppMessageResult result) {
  // drop the "APP_MSG_" prefix, the screen is narrow
  const char *name = translate_error(result);
  return strncmp(name, "APP_MSG_", 8) == 0 ? name + 8 : name;
}

bool comm_phone_reached(void) {
  return s_phone_reached;
}

void deinit_comm() {
  app_message_deregister_callbacks();
}

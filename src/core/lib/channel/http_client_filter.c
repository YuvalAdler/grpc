/*
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/lib/channel/http_client_filter.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <string.h>
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/percent_encoding.h"
#include "src/core/lib/support/string.h"
#include "src/core/lib/transport/static_metadata.h"
#include "src/core/lib/transport/transport_impl.h"

#define EXPECTED_CONTENT_TYPE "application/grpc"
#define EXPECTED_CONTENT_TYPE_LENGTH sizeof(EXPECTED_CONTENT_TYPE) - 1

/* default maximum size of payload eligable for GET request */
static const size_t kMaxPayloadSizeForGet = 2048;

typedef struct call_data {
  grpc_linked_mdelem method;
  grpc_linked_mdelem scheme;
  grpc_linked_mdelem authority;
  grpc_linked_mdelem te_trailers;
  grpc_linked_mdelem content_type;
  grpc_linked_mdelem user_agent;
  grpc_linked_mdelem payload_bin;

  grpc_metadata_batch *recv_initial_metadata;
  grpc_metadata_batch *recv_trailing_metadata;
  uint8_t *payload_bytes;

  /* Vars to read data off of send_message */
  grpc_transport_stream_op send_op;
  uint32_t send_length;
  uint32_t send_flags;
  grpc_slice incoming_slice;
  grpc_slice_buffer_stream replacement_stream;
  grpc_slice_buffer slices;
  /* flag that indicates that all slices of send_messages aren't availble */
  bool send_message_blocked;

  /** Closure to call when finished with the hc_on_recv hook */
  grpc_closure *on_done_recv_initial_metadata;
  grpc_closure *on_done_recv_trailing_metadata;
  grpc_closure *on_complete;
  grpc_closure *post_send;

  /** Receive closures are chained: we inject this closure as the on_done_recv
      up-call on transport_op, and remember to call our on_done_recv member
      after handling it. */
  grpc_closure hc_on_recv_initial_metadata;
  grpc_closure hc_on_recv_trailing_metadata;
  grpc_closure hc_on_complete;
  grpc_closure got_slice;
  grpc_closure send_done;
} call_data;

typedef struct channel_data {
  grpc_mdelem *static_scheme;
  grpc_mdelem *user_agent;
  size_t max_payload_size_for_get;
} channel_data;

typedef struct {
  grpc_call_element *elem;
  grpc_exec_ctx *exec_ctx;
} client_recv_filter_args;

static grpc_mdelem *client_recv_filter(void *user_data, grpc_mdelem *md) {
  client_recv_filter_args *a = user_data;
  if (md == GRPC_MDELEM_STATUS_200) {
    return NULL;
  } else if (md->key == GRPC_MDSTR_STATUS) {
    char *message_string;
    gpr_asprintf(&message_string, "Received http2 header with status: %s",
                 grpc_mdstr_as_c_string(md->value));
    grpc_slice message = grpc_slice_from_copied_string(message_string);
    gpr_free(message_string);
    grpc_call_element_send_close_with_message(a->exec_ctx, a->elem,
                                              GRPC_STATUS_CANCELLED, &message);
    return NULL;
  } else if (md->key == GRPC_MDSTR_GRPC_MESSAGE) {
    grpc_slice pct_decoded_msg =
        grpc_permissive_percent_decode_slice(md->value->slice);
    if (grpc_slice_is_equivalent(pct_decoded_msg, md->value->slice)) {
      grpc_slice_unref(pct_decoded_msg);
      return md;
    } else {
      return grpc_mdelem_from_metadata_strings(
          GRPC_MDSTR_GRPC_MESSAGE, grpc_mdstr_from_slice(pct_decoded_msg));
    }
  } else if (md == GRPC_MDELEM_CONTENT_TYPE_APPLICATION_SLASH_GRPC) {
    return NULL;
  } else if (md->key == GRPC_MDSTR_CONTENT_TYPE) {
    const char *value_str = grpc_mdstr_as_c_string(md->value);
    if (strncmp(value_str, EXPECTED_CONTENT_TYPE,
                EXPECTED_CONTENT_TYPE_LENGTH) == 0 &&
        (value_str[EXPECTED_CONTENT_TYPE_LENGTH] == '+' ||
         value_str[EXPECTED_CONTENT_TYPE_LENGTH] == ';')) {
      /* Although the C implementation doesn't (currently) generate them,
         any custom +-suffix is explicitly valid. */
      /* TODO(klempner): We should consider preallocating common values such
         as +proto or +json, or at least stashing them if we see them. */
      /* TODO(klempner): Should we be surfacing this to application code? */
    } else {
      /* TODO(klempner): We're currently allowing this, but we shouldn't
         see it without a proxy so log for now. */
      gpr_log(GPR_INFO, "Unexpected content-type '%s'", value_str);
    }
    return NULL;
  }
  return md;
}

static void hc_on_recv_initial_metadata(grpc_exec_ctx *exec_ctx,
                                        void *user_data, grpc_error *error) {
  grpc_call_element *elem = user_data;
  call_data *calld = elem->call_data;
  client_recv_filter_args a;
  a.elem = elem;
  a.exec_ctx = exec_ctx;
  grpc_metadata_batch_filter(calld->recv_initial_metadata, client_recv_filter,
                             &a);
  grpc_closure_run(exec_ctx, calld->on_done_recv_initial_metadata,
                   GRPC_ERROR_REF(error));
}

static void hc_on_recv_trailing_metadata(grpc_exec_ctx *exec_ctx,
                                         void *user_data, grpc_error *error) {
  grpc_call_element *elem = user_data;
  call_data *calld = elem->call_data;
  client_recv_filter_args a;
  a.elem = elem;
  a.exec_ctx = exec_ctx;
  grpc_metadata_batch_filter(calld->recv_trailing_metadata, client_recv_filter,
                             &a);
  grpc_closure_run(exec_ctx, calld->on_done_recv_trailing_metadata,
                   GRPC_ERROR_REF(error));
}

static void hc_on_complete(grpc_exec_ctx *exec_ctx, void *user_data,
                           grpc_error *error) {
  grpc_call_element *elem = user_data;
  call_data *calld = elem->call_data;
  if (calld->payload_bytes) {
    gpr_free(calld->payload_bytes);
    calld->payload_bytes = NULL;
  }
  calld->on_complete->cb(exec_ctx, calld->on_complete->cb_arg, error);
}

static void send_done(grpc_exec_ctx *exec_ctx, void *elemp, grpc_error *error) {
  grpc_call_element *elem = elemp;
  call_data *calld = elem->call_data;
  grpc_slice_buffer_reset_and_unref(&calld->slices);
  calld->post_send->cb(exec_ctx, calld->post_send->cb_arg, error);
}

static grpc_mdelem *client_strip_filter(void *user_data, grpc_mdelem *md) {
  /* eat the things we'd like to set ourselves */
  if (md->key == GRPC_MDSTR_METHOD) return NULL;
  if (md->key == GRPC_MDSTR_SCHEME) return NULL;
  if (md->key == GRPC_MDSTR_TE) return NULL;
  if (md->key == GRPC_MDSTR_CONTENT_TYPE) return NULL;
  if (md->key == GRPC_MDSTR_USER_AGENT) return NULL;
  return md;
}

static void continue_send_message(grpc_exec_ctx *exec_ctx,
                                  grpc_call_element *elem) {
  call_data *calld = elem->call_data;
  uint8_t *wrptr = calld->payload_bytes;
  while (grpc_byte_stream_next(exec_ctx, calld->send_op.send_message,
                               &calld->incoming_slice, ~(size_t)0,
                               &calld->got_slice)) {
    memcpy(wrptr, GRPC_SLICE_START_PTR(calld->incoming_slice),
           GRPC_SLICE_LENGTH(calld->incoming_slice));
    wrptr += GRPC_SLICE_LENGTH(calld->incoming_slice);
    grpc_slice_buffer_add(&calld->slices, calld->incoming_slice);
    if (calld->send_length == calld->slices.length) {
      calld->send_message_blocked = false;
      break;
    }
  }
}

static void got_slice(grpc_exec_ctx *exec_ctx, void *elemp, grpc_error *error) {
  grpc_call_element *elem = elemp;
  call_data *calld = elem->call_data;
  calld->send_message_blocked = false;
  grpc_slice_buffer_add(&calld->slices, calld->incoming_slice);
  if (calld->send_length == calld->slices.length) {
    /* Pass down the original send_message op that was blocked.*/
    grpc_slice_buffer_stream_init(&calld->replacement_stream, &calld->slices,
                                  calld->send_flags);
    calld->send_op.send_message = &calld->replacement_stream.base;
    calld->post_send = calld->send_op.on_complete;
    calld->send_op.on_complete = &calld->send_done;
    grpc_call_next_op(exec_ctx, elem, &calld->send_op);
  } else {
    continue_send_message(exec_ctx, elem);
  }
}

static void hc_mutate_op(grpc_exec_ctx *exec_ctx, grpc_call_element *elem,
                         grpc_transport_stream_op *op) {
  /* grab pointers to our data from the call element */
  call_data *calld = elem->call_data;
  channel_data *channeld = elem->channel_data;

  if (op->send_initial_metadata != NULL) {
    /* Decide which HTTP VERB to use. We use GET if the request is marked
    cacheable, and the operation contains both initial metadata and send
    message, and the payload is below the size threshold, and all the data
    for this request is immediately available. */
    grpc_mdelem *method = GRPC_MDELEM_METHOD_POST;
    calld->send_message_blocked = false;
    if ((op->send_initial_metadata_flags &
         GRPC_INITIAL_METADATA_CACHEABLE_REQUEST) &&
        op->send_message != NULL &&
        op->send_message->length < channeld->max_payload_size_for_get) {
      method = GRPC_MDELEM_METHOD_GET;
      calld->send_message_blocked = true;
    } else if (op->send_initial_metadata_flags &
               GRPC_INITIAL_METADATA_IDEMPOTENT_REQUEST) {
      method = GRPC_MDELEM_METHOD_PUT;
    }

    /* Attempt to read the data from send_message and create a header field. */
    if (method == GRPC_MDELEM_METHOD_GET) {
      /* allocate memory to hold the entire payload */
      calld->payload_bytes = gpr_malloc(op->send_message->length);

      /* read slices of send_message and copy into payload_bytes */
      calld->send_op = *op;
      calld->send_length = op->send_message->length;
      calld->send_flags = op->send_message->flags;
      continue_send_message(exec_ctx, elem);

      if (calld->send_message_blocked == false) {
        /* when all the send_message data is available, then create a MDELEM and
        append to headers */
        grpc_mdelem *payload_bin = grpc_mdelem_from_metadata_strings(
            GRPC_MDSTR_GRPC_PAYLOAD_BIN,
            grpc_mdstr_from_buffer(calld->payload_bytes,
                                   op->send_message->length));
        grpc_metadata_batch_add_tail(op->send_initial_metadata,
                                     &calld->payload_bin, payload_bin);
        calld->on_complete = op->on_complete;
        op->on_complete = &calld->hc_on_complete;
        op->send_message = NULL;
      } else {
        /* Not all data is available. Fall back to POST. */
        gpr_log(GPR_DEBUG,
                "Request is marked Cacheable but not all data is available.\
                            Falling back to POST");
        method = GRPC_MDELEM_METHOD_POST;
      }
    }

    grpc_metadata_batch_filter(op->send_initial_metadata, client_strip_filter,
                               elem);
    /* Send : prefixed headers, which have to be before any application
       layer headers. */
    grpc_metadata_batch_add_head(op->send_initial_metadata, &calld->method,
                                 method);
    grpc_metadata_batch_add_head(op->send_initial_metadata, &calld->scheme,
                                 channeld->static_scheme);
    grpc_metadata_batch_add_tail(op->send_initial_metadata, &calld->te_trailers,
                                 GRPC_MDELEM_TE_TRAILERS);
    grpc_metadata_batch_add_tail(
        op->send_initial_metadata, &calld->content_type,
        GRPC_MDELEM_CONTENT_TYPE_APPLICATION_SLASH_GRPC);
    grpc_metadata_batch_add_tail(op->send_initial_metadata, &calld->user_agent,
                                 GRPC_MDELEM_REF(channeld->user_agent));
  }

  if (op->recv_initial_metadata != NULL) {
    /* substitute our callback for the higher callback */
    calld->recv_initial_metadata = op->recv_initial_metadata;
    calld->on_done_recv_initial_metadata = op->recv_initial_metadata_ready;
    op->recv_initial_metadata_ready = &calld->hc_on_recv_initial_metadata;
  }

  if (op->recv_trailing_metadata != NULL) {
    /* substitute our callback for the higher callback */
    calld->recv_trailing_metadata = op->recv_trailing_metadata;
    calld->on_done_recv_trailing_metadata = op->on_complete;
    op->on_complete = &calld->hc_on_recv_trailing_metadata;
  }
}

static void hc_start_transport_op(grpc_exec_ctx *exec_ctx,
                                  grpc_call_element *elem,
                                  grpc_transport_stream_op *op) {
  GPR_TIMER_BEGIN("hc_start_transport_op", 0);
  GRPC_CALL_LOG_OP(GPR_INFO, elem, op);
  hc_mutate_op(exec_ctx, elem, op);
  GPR_TIMER_END("hc_start_transport_op", 0);
  call_data *calld = elem->call_data;
  if (op->send_message != NULL && calld->send_message_blocked) {
    /* Don't forward the op. send_message contains slices that aren't ready
    yet. The call will be forwarded by the op_complete of slice read call.
    */
  } else {
    grpc_call_next_op(exec_ctx, elem, op);
  }
}

/* Constructor for call_data */
static grpc_error *init_call_elem(grpc_exec_ctx *exec_ctx,
                                  grpc_call_element *elem,
                                  grpc_call_element_args *args) {
  call_data *calld = elem->call_data;
  calld->on_done_recv_initial_metadata = NULL;
  calld->on_done_recv_trailing_metadata = NULL;
  calld->on_complete = NULL;
  calld->payload_bytes = NULL;
  grpc_slice_buffer_init(&calld->slices);
  grpc_closure_init(&calld->hc_on_recv_initial_metadata,
                    hc_on_recv_initial_metadata, elem);
  grpc_closure_init(&calld->hc_on_recv_trailing_metadata,
                    hc_on_recv_trailing_metadata, elem);
  grpc_closure_init(&calld->hc_on_complete, hc_on_complete, elem);
  grpc_closure_init(&calld->got_slice, got_slice, elem);
  grpc_closure_init(&calld->send_done, send_done, elem);
  return GRPC_ERROR_NONE;
}

/* Destructor for call_data */
static void destroy_call_elem(grpc_exec_ctx *exec_ctx, grpc_call_element *elem,
                              const grpc_call_final_info *final_info,
                              void *ignored) {
  call_data *calld = elem->call_data;
  grpc_slice_buffer_destroy(&calld->slices);
}

static grpc_mdelem *scheme_from_args(const grpc_channel_args *args) {
  unsigned i;
  size_t j;
  grpc_mdelem *valid_schemes[] = {GRPC_MDELEM_SCHEME_HTTP,
                                  GRPC_MDELEM_SCHEME_HTTPS};
  if (args != NULL) {
    for (i = 0; i < args->num_args; ++i) {
      if (args->args[i].type == GRPC_ARG_STRING &&
          strcmp(args->args[i].key, GRPC_ARG_HTTP2_SCHEME) == 0) {
        for (j = 0; j < GPR_ARRAY_SIZE(valid_schemes); j++) {
          if (0 == strcmp(grpc_mdstr_as_c_string(valid_schemes[j]->value),
                          args->args[i].value.string)) {
            return valid_schemes[j];
          }
        }
      }
    }
  }
  return GRPC_MDELEM_SCHEME_HTTP;
}

static size_t max_payload_size_from_args(const grpc_channel_args *args) {
  if (args != NULL) {
    for (size_t i = 0; i < args->num_args; ++i) {
      if (0 == strcmp(args->args[i].key, GRPC_ARG_MAX_PAYLOAD_SIZE_FOR_GET)) {
        if (args->args[i].type != GRPC_ARG_INTEGER) {
          gpr_log(GPR_ERROR, "%s: must be an integer",
                  GRPC_ARG_MAX_PAYLOAD_SIZE_FOR_GET);
        } else {
          return (size_t)args->args[i].value.integer;
        }
      }
    }
  }
  return kMaxPayloadSizeForGet;
}

static grpc_mdstr *user_agent_from_args(const grpc_channel_args *args,
                                        const char *transport_name) {
  gpr_strvec v;
  size_t i;
  int is_first = 1;
  char *tmp;
  grpc_mdstr *result;

  gpr_strvec_init(&v);

  for (i = 0; args && i < args->num_args; i++) {
    if (0 == strcmp(args->args[i].key, GRPC_ARG_PRIMARY_USER_AGENT_STRING)) {
      if (args->args[i].type != GRPC_ARG_STRING) {
        gpr_log(GPR_ERROR, "Channel argument '%s' should be a string",
                GRPC_ARG_PRIMARY_USER_AGENT_STRING);
      } else {
        if (!is_first) gpr_strvec_add(&v, gpr_strdup(" "));
        is_first = 0;
        gpr_strvec_add(&v, gpr_strdup(args->args[i].value.string));
      }
    }
  }

  gpr_asprintf(&tmp, "%sgrpc-c/%s (%s; %s; %s)", is_first ? "" : " ",
               grpc_version_string(), GPR_PLATFORM_STRING, transport_name,
               grpc_g_stands_for());
  is_first = 0;
  gpr_strvec_add(&v, tmp);

  for (i = 0; args && i < args->num_args; i++) {
    if (0 == strcmp(args->args[i].key, GRPC_ARG_SECONDARY_USER_AGENT_STRING)) {
      if (args->args[i].type != GRPC_ARG_STRING) {
        gpr_log(GPR_ERROR, "Channel argument '%s' should be a string",
                GRPC_ARG_SECONDARY_USER_AGENT_STRING);
      } else {
        if (!is_first) gpr_strvec_add(&v, gpr_strdup(" "));
        is_first = 0;
        gpr_strvec_add(&v, gpr_strdup(args->args[i].value.string));
      }
    }
  }

  tmp = gpr_strvec_flatten(&v, NULL);
  gpr_strvec_destroy(&v);
  result = grpc_mdstr_from_string(tmp);
  gpr_free(tmp);

  return result;
}

/* Constructor for channel_data */
static void init_channel_elem(grpc_exec_ctx *exec_ctx,
                              grpc_channel_element *elem,
                              grpc_channel_element_args *args) {
  channel_data *chand = elem->channel_data;
  GPR_ASSERT(!args->is_last);
  GPR_ASSERT(args->optional_transport != NULL);
  chand->static_scheme = scheme_from_args(args->channel_args);
  chand->max_payload_size_for_get =
      max_payload_size_from_args(args->channel_args);
  chand->user_agent = grpc_mdelem_from_metadata_strings(
      GRPC_MDSTR_USER_AGENT,
      user_agent_from_args(args->channel_args,
                           args->optional_transport->vtable->name));
}

/* Destructor for channel data */
static void destroy_channel_elem(grpc_exec_ctx *exec_ctx,
                                 grpc_channel_element *elem) {
  channel_data *chand = elem->channel_data;
  GRPC_MDELEM_UNREF(chand->user_agent);
}

const grpc_channel_filter grpc_http_client_filter = {
    hc_start_transport_op,
    grpc_channel_next_op,
    sizeof(call_data),
    init_call_elem,
    grpc_call_stack_ignore_set_pollset_or_pollset_set,
    destroy_call_elem,
    sizeof(channel_data),
    init_channel_elem,
    destroy_channel_elem,
    grpc_call_next_get_peer,
    grpc_channel_next_get_info,
    "http-client"};

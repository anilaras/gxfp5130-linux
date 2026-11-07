#include "gxfp/flow/fdt.h"

#include "gxfp/cmd/fdt_cmd.h"
#include "gxfp/proto/goodix_cmd.h"
#include "gxfp/proto/goodix_constants.h"
#include "gxfp/proto/goodix_proto.h"
#include "gxfp/algo/common.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GXFP_FDT_BASE_TABLE_LEN 24
#define GXFP_FDT_BASE_UPDATE_ENTRIES 10

#define FDT_STATUS_DOWN_GET_UP_BASE 0x0002
#define FDT_STATUS_UP 0x0200
#define FDT_STATUS_IRQ_DOWN_GET_UP_BASE 0x0002
#define FDT_STATUS_IRQ_UP_GET_DOWN_BASE 0x0200
#define FDT_STATUS_IRQ_REVERSE_0 0x0080
#define FDT_STATUS_IRQ_REVERSE_2 0x0082

#define FDT_UP_DEFAULT_OFFSET 0x15u
#define FDT_UP_NOTOUCH_FALLBACK 0x1300u
#define FDT_WAIT_UP_RETRY_MS 750
/*
 * Re-arm budget for GXFP_FDT_MODE_WAIT_UP. That state waits for a human to
 * lift their finger: the measured down->up report latency on GF_GCC_EC_20069
 * is 1.4-1.8 s even when the operator lifts immediately, so the previous
 * budget of 3 (~2.25 s) aborted a 17-stage fprintd-enroll on the first
 * hesitation - and fprintd deletes the stored template when an enrollment
 * starts, leaving the user with no fingerprint at all. 20 re-arms = ~15 s.
 */
#define FDT_WAIT_UP_MAX_RETRIES 20

/*
 * The MCU sometimes reports a saturated (0xff) or empty (0x00) baseline.
 * Deriving the up/down threshold tables from it yields a threshold the
 * sensor can never cross, so finger-up is never reported and WAIT_UP times
 * out. Backported from Void755/gxfpmoc 42860c1.
 */
static bool fdt_base_valid(const uint8_t *raw, size_t raw_len)
{
	size_t i;

	if (!raw || raw_len < 2)
		return false;

	for (i = 0; i + 1 < raw_len; i += 2) {
		uint16_t value = gxfp_le16(raw + i);
		uint8_t half = (uint8_t)(value >> 1);

		if (half == 0x00 || half == 0xff)
			return false;
	}

	return true;
}

static void fdt_downbase_from_raw(const uint8_t *raw_base,
				  size_t raw_len,
				  uint8_t out_table[GXFP_FDT_BASE_TABLE_LEN])
{
	size_t i;
	size_t entries;

	if (!raw_base || !out_table || raw_len < 2)
		return;

	entries = raw_len / 2;
	if (entries > GXFP_FDT_BASE_UPDATE_ENTRIES)
		entries = GXFP_FDT_BASE_UPDATE_ENTRIES;

	for (i = 0; i < entries; i++) {
		uint16_t raw = gxfp_le16(raw_base + (i * 2));
		uint16_t half = (uint16_t)((raw >> 1) & 0xffffu);
		uint16_t encoded = (uint16_t)((half << 8) | 0x80u);

		out_table[(i * 2) + 0] = (uint8_t)(encoded & 0xffu);
		out_table[(i * 2) + 1] = (uint8_t)((encoded >> 8) & 0xffu);
	}
}

static void fdt_upbase_from_raw(uint16_t touchflag,
				int second_flag,
				int diff_use,
				int16_t dac_offset,
				const uint8_t *raw_base,
				size_t raw_len,
				uint8_t out_table[GXFP_FDT_BASE_TABLE_LEN])
{
	size_t i;
	size_t entries;
	uint8_t merged[GXFP_FDT_BASE_TABLE_LEN];

	if (!raw_base || !out_table || raw_len < 2)
		return;

	entries = raw_len / 2;
	if (entries > GXFP_FDT_BASE_UPDATE_ENTRIES)
		entries = GXFP_FDT_BASE_UPDATE_ENTRIES;

	memcpy(merged, out_table, sizeof(merged));

	for (i = 0; i < entries; i++) {
		uint16_t raw = gxfp_le16(raw_base + (i * 2));
		uint16_t half = (uint16_t)((raw >> 1) & 0xffffu);
		uint16_t offset = (dac_offset == 0) ? FDT_UP_DEFAULT_OFFSET : (uint16_t)dac_offset;
		uint16_t encoded = (uint16_t)(((half + offset) << 8) | (half + offset));
		uint16_t prev;

		if (second_flag == 0 && ((touchflag >> i) & 0x1u) == 0) {
			if (dac_offset == 0)
				encoded = (uint16_t)((encoded >> 8) | FDT_UP_NOTOUCH_FALLBACK);
			else
				encoded = (uint16_t)((((uint16_t)(dac_offset - 2)) << 8) | (encoded >> 8));
		}

		prev = gxfp_le16(merged + (i * 2));
		if (diff_use == 1 && encoded > prev)
			encoded = prev;

		merged[(i * 2) + 0] = (uint8_t)(encoded & 0xffu);
		merged[(i * 2) + 1] = (uint8_t)((encoded >> 8) & 0xffu);
	}

	memcpy(out_table, merged, sizeof(merged));
}

static bool fdt_base_table_update_from_frame(struct gxfp_cmd_fdt_state *state,
					     const struct gxfp_frame_parsed *frame)
{
	uint16_t irq_status;

	if (!state || !frame || !frame->payload)
		return false;

	irq_status = gxfp_le16(frame->payload);

	if (irq_status == FDT_STATUS_IRQ_DOWN_GET_UP_BASE) {
		const uint8_t *raw_base;
		size_t raw_len;
		uint16_t touchflag;
		int second_flag = state->up_second_flag;

		if (frame->payload_len < 4)
			return false;

		touchflag = gxfp_le16(frame->payload + 2);
		raw_base = frame->payload + 4;
		raw_len = frame->payload_len - 4;
		if (raw_len > (GXFP_FDT_BASE_UPDATE_ENTRIES * 2))
			raw_len = GXFP_FDT_BASE_UPDATE_ENTRIES * 2;

		if (!fdt_base_valid(raw_base, raw_len))
			return false;

		if (state->up_need_twice) {
			second_flag = (state->up_round > 0) ? 1 : 0;
			state->up_round ^= 1;
		}

		fdt_upbase_from_raw(touchflag,
				   second_flag,
				   state->up_diff_use,
				   state->up_dac_offset,
				   raw_base,
				   raw_len,
				   state->up_table_5130);
		return true;
	}

	if (irq_status == FDT_STATUS_IRQ_UP_GET_DOWN_BASE ||
	    irq_status == FDT_STATUS_IRQ_REVERSE_0 ||
	    irq_status == FDT_STATUS_IRQ_REVERSE_2) {
		const uint8_t *raw_base;
		size_t raw_len;

		if (frame->payload_len < 4)
			return false;

		raw_base = frame->payload + 4;
		raw_len = frame->payload_len - 4;
		if (raw_len > (GXFP_FDT_BASE_UPDATE_ENTRIES * 2))
			raw_len = GXFP_FDT_BASE_UPDATE_ENTRIES * 2;

		if (!fdt_base_valid(raw_base, raw_len))
			return false;

		fdt_downbase_from_raw(raw_base, raw_len, state->down_table_5130);
		return true;
	}

	return true;
}

static int fdt_update_state_from_record(struct gxfp_cmd_fdt_state *state,
					const struct gxfp_tap_hdr *tap_hdr,
					const uint8_t *payload,
					size_t payload_len,
					uint16_t *out_status,
					bool *out_base_valid)
{
	struct gxfp_frame_parsed frame;
	bool base_valid;

	if (!state)
		return -EINVAL;

	if (!tap_hdr || !payload)
		return -EINVAL;

	if (tap_hdr->type != GOODIX_MP_TYPE_CMD &&
	    tap_hdr->type != GOODIX_MP_TYPE_NOTICE)
		return -ENOENT;

	if (!gxfp_parse_goodix_body(payload, payload_len, &frame) || !frame.valid)
		return -ENOENT;

	if (frame.payload_len < 2)
		return -ENOENT;

	if (frame.cmd != GXFP_CMD_FDT_STATUS &&
	    frame.cmd != GXFP_CMD_FDT_DOWN &&
	    frame.cmd != GXFP_CMD_FDT_MODE &&
	    frame.cmd != GXFP_CMD_FDT_UP)
		return -ENOENT;

	if (!state->base_table_inited)
		gxfp_cmd_fdt_state_init(state);

	base_valid = fdt_base_table_update_from_frame(state, &frame);

	if (getenv("FP_GXFP_LOG")) {
		fprintf(stderr, "gxfp-fdt: cmd=0x%02x status=0x%04x valid=%d base=",
			frame.cmd, gxfp_le16(frame.payload), base_valid ? 1 : 0);
		for (size_t i = 2; i < frame.payload_len && i < 26; i++)
			fprintf(stderr, "%02x", frame.payload[i]);
		fprintf(stderr, "\n");
	}

	if (out_status)
		*out_status = gxfp_le16(frame.payload);
	if (out_base_valid)
		*out_base_valid = base_valid;

	return 0;
}

void gxfp_fdt_flow_init(struct gxfp_fdt_flow *flow)
{
	if (!flow)
		return;

	memset(flow, 0, sizeof(*flow));
	gxfp_cmd_fdt_state_init(&flow->cmd);
	flow->mode = GXFP_FDT_MODE_IDLE;
	flow->state = GXFP_FDT_STATE_UNKNOWN;
}

int gxfp_fdt_flow_set_mode(struct gxfp_fdt_flow *flow,
			   struct gxfp_dev *dev,
			   enum gxfp_fdt_mode mode)
{
	int r;

	if (!flow || !dev)
		return -EINVAL;
	if (mode == flow->mode)
		return 0;

	if (mode == GXFP_FDT_MODE_IDLE) {
		flow->mode = GXFP_FDT_MODE_IDLE;
		return 0;
	}

	if (mode == GXFP_FDT_MODE_WAIT_DOWN) {
		r = gxfp_cmd_fdt_set_mode(dev, &flow->cmd);
		if (r < 0)
			return r;
		r = gxfp_cmd_fdt_send_down(dev, &flow->cmd);
		if (r < 0)
			return r;
		flow->mode = GXFP_FDT_MODE_WAIT_DOWN;
		return 0;
	}

	if (mode == GXFP_FDT_MODE_WAIT_UP) {
		r = gxfp_cmd_fdt_set_mode(dev, &flow->cmd);
		if (r < 0)
			return r;
		r = gxfp_cmd_fdt_send_up(dev, &flow->cmd);
		if (r < 0)
			return r;
		memcpy(flow->armed_up_table, flow->cmd.up_table_5130,
		       sizeof(flow->armed_up_table));
		flow->wait_up_armed_ms = gxfp_monotonic_ms();
		flow->wait_up_retries = 0;
		flow->mode = GXFP_FDT_MODE_WAIT_UP;
		return 0;
	}

	return -EINVAL;
}

static uint32_t fdt_status_to_events(uint16_t status)
{
	if (status == FDT_STATUS_DOWN_GET_UP_BASE)
		return GXFP_FDT_EVENT_DOWN;
	if (status == FDT_STATUS_UP)
		return GXFP_FDT_EVENT_UP;
	if (status == FDT_STATUS_IRQ_REVERSE_0 || status == FDT_STATUS_IRQ_REVERSE_2)
		return GXFP_FDT_EVENT_REVERSE;

	return GXFP_FDT_EVENT_NONE;
}

int gxfp_fdt_flow_feed_record(struct gxfp_fdt_flow *flow,
			      struct gxfp_dev *dev,
			      const struct gxfp_tap_hdr *tap_hdr,
			      const uint8_t *payload,
			      size_t payload_len,
			      uint32_t *out_events)
{
	uint16_t status = 0;
	uint32_t events;
	bool base_valid = true;
	int r;

	if (!flow || !dev || !tap_hdr || !payload || !out_events)
		return -EINVAL;

	*out_events = GXFP_FDT_EVENT_NONE;
	r = fdt_update_state_from_record(&flow->cmd, tap_hdr, payload, payload_len,
					 &status, &base_valid);
	if (r < 0)
		return (r == -EAGAIN || r == -ENOENT) ? -EAGAIN : r;

	flow->last_status = status;

	if (!base_valid) {
		enum gxfp_fdt_mode mode = flow->mode;
		int retries = flow->wait_up_retries;

		/* Drop the report and re-arm the same mode with the old tables. */
		flow->state = GXFP_FDT_STATE_UNKNOWN;
		flow->mode = GXFP_FDT_MODE_IDLE;
		if (mode == GXFP_FDT_MODE_WAIT_UP && flow->last_good_up_valid)
			memcpy(flow->cmd.up_table_5130, flow->last_good_up_table,
			       sizeof(flow->cmd.up_table_5130));
		if (mode != GXFP_FDT_MODE_IDLE) {
			r = gxfp_fdt_flow_set_mode(flow, dev, mode);
			if (r < 0)
				return r;
		}
		/* Invalid reports must not reset the WAIT_UP re-arm budget. */
		if (mode == GXFP_FDT_MODE_WAIT_UP)
			flow->wait_up_retries = retries + 1;

		*out_events = GXFP_FDT_EVENT_BASE_INVALID;
		return 0;
	}

	events = fdt_status_to_events(status);

	if ((events & GXFP_FDT_EVENT_REVERSE) && flow->mode == GXFP_FDT_MODE_WAIT_DOWN) {
		r = gxfp_cmd_fdt_set_mode(dev, &flow->cmd);
		if (r < 0)
			return r;
		r = gxfp_cmd_fdt_send_down(dev, &flow->cmd);
		if (r < 0)
			return r;
	}

	/*
	 * 0x0080/0x0082 carry the same "finger up, here is the down baseline"
	 * report as FDT_STATUS_UP: fdt_base_table_update_from_frame() already
	 * treats all three as UP_GET_DOWN_BASE. A firmware that answers an
	 * armed FDT_UP with this encoding while no finger is present (observed
	 * on GF_GCC_EC_20069) would otherwise never complete WAIT_UP - the
	 * record is consumed as REVERSE, which only WAIT_DOWN acts on - so the
	 * caller re-arms FDT_WAIT_UP_MAX_RETRIES times and fails with
	 * -ETIMEDOUT ("fdt wait-up retry failed").
	 */
	if ((events & GXFP_FDT_EVENT_REVERSE) && flow->mode == GXFP_FDT_MODE_WAIT_UP)
		events |= GXFP_FDT_EVENT_UP;

	if (events & GXFP_FDT_EVENT_DOWN)
		flow->state = GXFP_FDT_STATE_DOWN;
	if (events & GXFP_FDT_EVENT_UP) {
		flow->state = GXFP_FDT_STATE_UP;
		memcpy(flow->last_good_up_table, flow->armed_up_table,
		       sizeof(flow->last_good_up_table));
		flow->last_good_up_valid = 1;
	}

	*out_events = events;
	if (events == GXFP_FDT_EVENT_NONE)
		return -EAGAIN;

	return 0;
}

int gxfp_fdt_flow_poll(struct gxfp_fdt_flow *flow,
		       struct gxfp_dev *dev,
		       int64_t now_ms)
{
	int due;
	int r;

	if (!flow || !dev)
		return -EINVAL;

	due = gxfp_fdt_flow_wait_up_retry_due(flow, now_ms);
	if (due <= 0)
		return due;

	if (flow->last_good_up_valid)
		memcpy(flow->cmd.up_table_5130, flow->last_good_up_table,
		       sizeof(flow->cmd.up_table_5130));

	r = gxfp_cmd_fdt_set_mode(dev, &flow->cmd);
	if (r < 0)
		return r;
	r = gxfp_cmd_fdt_send_up(dev, &flow->cmd);
	if (r < 0)
		return r;

	memcpy(flow->armed_up_table, flow->cmd.up_table_5130,
	       sizeof(flow->armed_up_table));
	flow->wait_up_armed_ms = now_ms;
	flow->wait_up_retries++;
	return 0;
}

int gxfp_fdt_flow_wait_up_retry_due(const struct gxfp_fdt_flow *flow,
				    int64_t now_ms)
{
	if (!flow)
		return -EINVAL;
	if (flow->mode != GXFP_FDT_MODE_WAIT_UP || flow->state == GXFP_FDT_STATE_UP)
		return 0;
	if (flow->wait_up_armed_ms <= 0 || now_ms < flow->wait_up_armed_ms)
		return 0;
	if (now_ms - flow->wait_up_armed_ms < FDT_WAIT_UP_RETRY_MS)
		return 0;
	if (flow->wait_up_retries >= FDT_WAIT_UP_MAX_RETRIES)
		return -ETIMEDOUT;
	return 1;
}

enum gxfp_fdt_state gxfp_fdt_flow_get_state(const struct gxfp_fdt_flow *flow)
{
	if (!flow)
		return GXFP_FDT_STATE_UNKNOWN;

	return flow->state;
}

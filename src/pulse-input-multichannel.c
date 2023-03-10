/*
Copyright (C) 2023 by Norihiro Kamae <norihiro@nagater.net>
Copyright (C) 2014 by Leonhard Oelke <leonhard@in-verted.de>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <util/platform.h>
#include <util/bmem.h>
#include <util/util_uint64.h>
#include <obs-module.h>
#include "plugin-macros.generated.h"

#include "pulse-wrapper.h"

#define NSEC_PER_SEC 1000000000LL
#define NSEC_PER_MSEC 1000000L

#define PULSE_DATA(voidptr) struct pulse_data *data = voidptr;

struct pulse_data {
	obs_source_t *source;
	pa_stream *stream;

	/* user settings */
	char *device;
	bool is_default;
	bool input;
	pa_channel_map channel_map;

	/* server info */
	enum speaker_layout speakers;
	pa_sample_format_t format;
	uint_fast32_t samples_per_sec;
	uint_fast32_t bytes_per_frame;
	uint64_t first_ts;

	/* statistics */
	uint_fast32_t packets;
	uint_fast64_t frames;
};

static void pulse_stop_recording(struct pulse_data *data);

/**
 * get obs from pulse audio format
 */
static enum audio_format pulse_to_obs_audio_format(pa_sample_format_t format)
{
	switch (format) {
	case PA_SAMPLE_U8:
		return AUDIO_FORMAT_U8BIT;
	case PA_SAMPLE_S16LE:
		return AUDIO_FORMAT_16BIT;
	case PA_SAMPLE_S32LE:
		return AUDIO_FORMAT_32BIT;
	case PA_SAMPLE_FLOAT32LE:
		return AUDIO_FORMAT_FLOAT;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}

	return AUDIO_FORMAT_UNKNOWN;
}

/**
 * Get obs speaker layout from number of channels
 *
 * @param channels number of channels reported by pulseaudio
 *
 * @return obs speaker_layout id
 *
 * @note This *might* not work for some rather unusual setups, but should work
 *       fine for the majority of cases.
 */
static enum speaker_layout
pulse_channels_to_obs_speakers(uint_fast32_t channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	}

	return SPEAKERS_UNKNOWN;
}

static inline uint64_t samples_to_ns(size_t frames, uint_fast32_t rate)
{
	return util_mul_div64(frames, NSEC_PER_SEC, rate);
}

static inline uint64_t get_sample_time(size_t frames, uint_fast32_t rate)
{
	return os_gettime_ns() - samples_to_ns(frames, rate);
}

#define STARTUP_TIMEOUT_NS (500 * NSEC_PER_MSEC)

/**
 * Callback for pulse which gets executed when new audio data is available
 *
 * @warning The function may be called even after disconnecting the stream
 */
static void pulse_stream_read(pa_stream *p, size_t nbytes, void *userdata)
{
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(nbytes);
	PULSE_DATA(userdata);

	const void *frames;
	size_t bytes;

	if (!data->stream)
		goto exit;

	pa_stream_peek(data->stream, &frames, &bytes);

	// check if we got data
	if (!bytes)
		goto exit;

	if (!frames) {
		blog(LOG_ERROR, "Got audio hole of %u bytes",
		     (unsigned int)bytes);
		pa_stream_drop(data->stream);
		goto exit;
	}

	struct obs_source_audio out;
	out.speakers = data->speakers;
	out.samples_per_sec = data->samples_per_sec;
	out.format = pulse_to_obs_audio_format(data->format);
	out.data[0] = (uint8_t *)frames;
	out.frames = bytes / data->bytes_per_frame;
	out.timestamp = get_sample_time(out.frames, out.samples_per_sec);

	if (!data->first_ts)
		data->first_ts = out.timestamp + STARTUP_TIMEOUT_NS;

	if (out.timestamp > data->first_ts)
		obs_source_output_audio(data->source, &out);

	data->packets++;
	data->frames += out.frames;

	pa_stream_drop(data->stream);
exit:
	pulse_signal(0);
}

/**
 * Server info callback
 */
static void pulse_server_info(pa_context *c, const pa_server_info *i,
			      void *userdata)
{
	UNUSED_PARAMETER(c);
	PULSE_DATA(userdata);

	blog(LOG_INFO, "Server name: '%s %s'", i->server_name,
	     i->server_version);

	if (data->is_default) {
		bfree(data->device);
		if (data->input) {
			data->device = bstrdup(i->default_source_name);

			blog(LOG_DEBUG, "Default input device: '%s'",
			     data->device);
		} else {
			char *monitor =
				bzalloc(strlen(i->default_sink_name) + 9);
			strcat(monitor, i->default_sink_name);
			strcat(monitor, ".monitor");

			data->device = bstrdup(monitor);

			blog(LOG_DEBUG, "Default output device: '%s'",
			     data->device);
			bfree(monitor);
		}
	}

	pulse_signal(0);
}

/**
 * Source info callback
 *
 * We use the default stream settings for recording here unless pulse is
 * configured to something obs can't deal with.
 */
static void pulse_source_info(pa_context *c, const pa_source_info *i, int eol,
			      void *userdata)
{
	UNUSED_PARAMETER(c);
	PULSE_DATA(userdata);
	// An error occured
	if (eol < 0) {
		data->format = PA_SAMPLE_INVALID;
		goto skip;
	}
	// Terminating call for multi instance callbacks
	if (eol > 0)
		goto skip;

	blog(LOG_INFO,
	     "Audio format: %s, %" PRIu32 " Hz"
	     ", %" PRIu8 " channels",
	     pa_sample_format_to_string(i->sample_spec.format),
	     i->sample_spec.rate, i->sample_spec.channels);

	pa_sample_format_t format = i->sample_spec.format;
	if (pulse_to_obs_audio_format(format) == AUDIO_FORMAT_UNKNOWN) {
		format = PA_SAMPLE_FLOAT32LE;

		blog(LOG_INFO,
		     "Sample format %s not supported by OBS,"
		     "using %s instead for recording",
		     pa_sample_format_to_string(i->sample_spec.format),
		     pa_sample_format_to_string(format));
	}

	data->format = format;
	data->samples_per_sec = i->sample_spec.rate;

skip:
	pulse_signal(0);
}

/**
 * Start recording
 *
 * We request the default format used by pulse here because the data will be
 * converted and possibly re-sampled by obs anyway.
 *
 * For now we request a buffer length of 25ms although pulse seems to ignore
 * this setting for monitor streams. For "real" input streams this should work
 * fine though.
 */
static int_fast32_t pulse_start_recording(struct pulse_data *data)
{
	if (pulse_get_server_info(pulse_server_info, (void *)data) < 0) {
		blog(LOG_ERROR, "Unable to get server info !");
		return -1;
	}

	if (pulse_get_source_info(pulse_source_info, data->device,
				  (void *)data) < 0) {
		blog(LOG_ERROR, "Unable to get source info !");
		return -1;
	}
	if (data->format == PA_SAMPLE_INVALID) {
		blog(LOG_ERROR,
		     "An error occurred while getting the source info!");
		return -1;
	}

	pa_sample_spec spec;
	spec.format = data->format;
	spec.rate = data->samples_per_sec;
	spec.channels = data->channel_map.channels;

	if (!pa_sample_spec_valid(&spec)) {
		blog(LOG_ERROR, "Sample spec is not valid");
		return -1;
	}

	data->speakers =
		pulse_channels_to_obs_speakers(data->channel_map.channels);
	data->bytes_per_frame = pa_frame_size(&spec);

	data->stream = pulse_stream_new(obs_source_get_name(data->source),
					&spec, &data->channel_map);
	if (!data->stream) {
		blog(LOG_ERROR, "Unable to create stream");
		return -1;
	}

	pulse_lock();
	pa_stream_set_read_callback(data->stream, pulse_stream_read,
				    (void *)data);
	pulse_unlock();

	pa_buffer_attr attr;
	attr.fragsize = pa_usec_to_bytes(25000, &spec);
	attr.maxlength = (uint32_t)-1;
	attr.minreq = (uint32_t)-1;
	attr.prebuf = (uint32_t)-1;
	attr.tlength = (uint32_t)-1;

	pa_stream_flags_t flags = PA_STREAM_ADJUST_LATENCY;
	if (!data->is_default)
		flags |= PA_STREAM_DONT_MOVE;

	pulse_lock();
	int_fast32_t ret = pa_stream_connect_record(data->stream, data->device,
						    &attr, flags);
	pulse_unlock();
	if (ret < 0) {
		pulse_stop_recording(data);
		blog(LOG_ERROR, "Unable to connect to stream");
		return -1;
	}

	if (data->is_default)
		blog(LOG_INFO, "Started recording from '%s' (default)",
		     data->device);
	else
		blog(LOG_INFO, "Started recording from '%s'", data->device);

	return 0;
}

/**
 * stop recording
 */
static void pulse_stop_recording(struct pulse_data *data)
{
	if (data->stream) {
		pulse_lock();
		pa_stream_disconnect(data->stream);
		pa_stream_unref(data->stream);
		data->stream = NULL;
		pulse_unlock();
	}

	blog(LOG_INFO, "Stopped recording from '%s'", data->device);
	blog(LOG_INFO,
	     "Got %" PRIuFAST32 " packets with %" PRIuFAST64 " frames",
	     data->packets, data->frames);

	data->first_ts = 0;
	data->packets = 0;
	data->frames = 0;
}

/**
 * input info callback
 */
static void pulse_input_info(pa_context *c, const pa_source_info *i, int eol,
			     void *userdata)
{
	UNUSED_PARAMETER(c);
	if (eol != 0 || i->monitor_of_sink != PA_INVALID_INDEX)
		goto skip;

	obs_property_list_add_string((obs_property_t *)userdata, i->description,
				     i->name);

skip:
	pulse_signal(0);
}

/**
 * output info callback
 */
static void pulse_output_info(pa_context *c, const pa_sink_info *i, int eol,
			      void *userdata)
{
	UNUSED_PARAMETER(c);
	if (eol != 0 || i->monitor_source == PA_INVALID_INDEX)
		goto skip;

	obs_property_list_add_string((obs_property_t *)userdata, i->description,
				     i->monitor_source_name);

skip:
	pulse_signal(0);
}

static void init_pa_channels_list(obs_property_t *p)
{
	const struct {
		enum speaker_layout speakers;
		const char *name;
	} list[] = {
		{1, "MONO"},    {2, "STEREO"},  {3, "2POINT1"}, {4, "4POINT0"},
		{5, "4POINT1"}, {6, "5POINT1"}, {8, "7POINT1"},
	};

	for (size_t i = 0; i < sizeof(list) / sizeof(*list); i++) {
		obs_property_list_add_int(p, obs_module_text(list[i].name),
					  list[i].speakers);
	}
}

static void init_pa_map_list(obs_property_t *p)
{
	const struct {
		enum pa_channel_position position;
		const char *name;
	} list[] = {
		{PA_CHANNEL_POSITION_FRONT_LEFT, "FRONT_LEFT"},
		{PA_CHANNEL_POSITION_FRONT_RIGHT, "FRONT_RIGHT"},
		{PA_CHANNEL_POSITION_FRONT_CENTER, "FRONT_CENTER"},
		{PA_CHANNEL_POSITION_LFE, "LFE"},
		{PA_CHANNEL_POSITION_REAR_LEFT, "REAR_LEFT"},
		{PA_CHANNEL_POSITION_REAR_RIGHT, "REAR_RIGHT"},
		{PA_CHANNEL_POSITION_SIDE_LEFT, "SIDE_LEFT"},
		{PA_CHANNEL_POSITION_SIDE_RIGHT, "SIDE_RIGHT"},

		{PA_CHANNEL_POSITION_MONO, "MONO"},

		{PA_CHANNEL_POSITION_REAR_CENTER, "REAR_CENTER"},

		{PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
		 "FRONT_LEFT_OF_CENTER"},
		{PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
		 "FRONT_RIGHT_OF_CENTER"},

		{PA_CHANNEL_POSITION_AUX0, "AUX0"},
		{PA_CHANNEL_POSITION_AUX1, "AUX1"},
		{PA_CHANNEL_POSITION_AUX2, "AUX2"},
		{PA_CHANNEL_POSITION_AUX3, "AUX3"},
		{PA_CHANNEL_POSITION_AUX4, "AUX4"},
		{PA_CHANNEL_POSITION_AUX5, "AUX5"},
		{PA_CHANNEL_POSITION_AUX6, "AUX6"},
		{PA_CHANNEL_POSITION_AUX7, "AUX7"},
		{PA_CHANNEL_POSITION_AUX8, "AUX8"},
		{PA_CHANNEL_POSITION_AUX9, "AUX9"},
		{PA_CHANNEL_POSITION_AUX10, "AUX10"},
		{PA_CHANNEL_POSITION_AUX11, "AUX11"},
		{PA_CHANNEL_POSITION_AUX12, "AUX12"},
		{PA_CHANNEL_POSITION_AUX13, "AUX13"},
		{PA_CHANNEL_POSITION_AUX14, "AUX14"},
		{PA_CHANNEL_POSITION_AUX15, "AUX15"},
		{PA_CHANNEL_POSITION_AUX16, "AUX16"},
		{PA_CHANNEL_POSITION_AUX17, "AUX17"},
		{PA_CHANNEL_POSITION_AUX18, "AUX18"},
		{PA_CHANNEL_POSITION_AUX19, "AUX19"},
		{PA_CHANNEL_POSITION_AUX20, "AUX20"},
		{PA_CHANNEL_POSITION_AUX21, "AUX21"},
		{PA_CHANNEL_POSITION_AUX22, "AUX22"},
		{PA_CHANNEL_POSITION_AUX23, "AUX23"},
		{PA_CHANNEL_POSITION_AUX24, "AUX24"},
		{PA_CHANNEL_POSITION_AUX25, "AUX25"},
		{PA_CHANNEL_POSITION_AUX26, "AUX26"},
		{PA_CHANNEL_POSITION_AUX27, "AUX27"},
		{PA_CHANNEL_POSITION_AUX28, "AUX28"},
		{PA_CHANNEL_POSITION_AUX29, "AUX29"},
		{PA_CHANNEL_POSITION_AUX30, "AUX30"},
		{PA_CHANNEL_POSITION_AUX31, "AUX31"},

		{PA_CHANNEL_POSITION_TOP_CENTER, "TOP_CENTER"},

		{PA_CHANNEL_POSITION_TOP_FRONT_LEFT, "TOP_FRONT_LEFT"},
		{PA_CHANNEL_POSITION_TOP_FRONT_RIGHT, "TOP_FRONT_RIGHT"},
		{PA_CHANNEL_POSITION_TOP_FRONT_CENTER, "TOP_FRONT_CENTER"},

		{PA_CHANNEL_POSITION_TOP_REAR_LEFT, "TOP_REAR_LEFT"},
		{PA_CHANNEL_POSITION_TOP_REAR_RIGHT, "TOP_REAR_RIGHT"},
		{PA_CHANNEL_POSITION_TOP_REAR_CENTER, "TOP_REAR_CENTER"},
	};

	for (size_t i = 0; i < sizeof(list) / sizeof(*list); i++) {
		obs_property_list_add_int(p, obs_module_text(list[i].name),
					  (long long)list[i].position);
	}
}

static bool channels_changed(obs_properties_t *props, obs_property_t *p,
			     obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	size_t pa_channels = obs_data_get_int(settings, "pa_channels");

	for (size_t i = 0; i < PA_CHANNELS_MAX; i++) {
		char name[16];
		sprintf(name, "pa_map_%zu", i);
		obs_property_t *p = obs_properties_get(props, name);
		obs_property_set_visible(p, i < pa_channels);
	}

	return true;
}

/**
 * Get plugin properties
 */
static obs_properties_t *pulse_properties(bool input)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *devices = obs_properties_add_list(
		props, "device_id", obs_module_text("Device"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	pulse_init();
	if (input)
		pulse_get_source_info_list(pulse_input_info, (void *)devices);
	else
		pulse_get_sink_info_list(pulse_output_info, (void *)devices);
	pulse_unref();

	size_t count = obs_property_list_item_count(devices);

	if (count > 0)
		obs_property_list_insert_string(
			devices, 0, obs_module_text("Default"), "default");

	obs_property_t *pa_channels = obs_properties_add_list(
		props, "pa_channels", obs_module_text("PAChannels"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	init_pa_channels_list(pa_channels);
	obs_property_set_modified_callback(pa_channels, channels_changed);

	for (size_t i = 0; i < PA_CHANNELS_MAX; i++) {
		char name[16], desc[16];
		sprintf(name, "pa_map_%zu", i);
		sprintf(desc, "PAMap.%zu", i);
		obs_property_t *pa_map = obs_properties_add_list(
			props, name, desc, OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
		init_pa_map_list(pa_map);
	}

	return props;
}

static obs_properties_t *pulse_input_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	return pulse_properties(true);
}

static obs_properties_t *pulse_output_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	return pulse_properties(false);
}

/**
 * Get plugin defaults
 */
static void pulse_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "device_id", "default");
	obs_data_set_default_int(settings, "pa_channels", 2);

	obs_data_set_default_int(settings, "pa_map_0",
				 PA_CHANNEL_POSITION_FRONT_LEFT);
	obs_data_set_default_int(settings, "pa_map_1",
				 PA_CHANNEL_POSITION_FRONT_RIGHT);
	obs_data_set_default_int(settings, "pa_map_2",
				 PA_CHANNEL_POSITION_FRONT_CENTER);
	obs_data_set_default_int(settings, "pa_map_3", PA_CHANNEL_POSITION_LFE);
	obs_data_set_default_int(settings, "pa_map_4",
				 PA_CHANNEL_POSITION_REAR_LEFT);
	obs_data_set_default_int(settings, "pa_map_5",
				 PA_CHANNEL_POSITION_REAR_RIGHT);
	obs_data_set_default_int(settings, "pa_map_6",
				 PA_CHANNEL_POSITION_SIDE_LEFT);
	obs_data_set_default_int(settings, "pa_map_7",
				 PA_CHANNEL_POSITION_SIDE_RIGHT);
}

/**
 * Returns the name of the plugin
 */
static const char *pulse_input_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PulseInputMC");
}

static const char *pulse_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PulseOutputMC");
}

/**
 * Destroy the plugin object and free all memory
 */
static void pulse_destroy(void *vptr)
{
	PULSE_DATA(vptr);

	if (!data)
		return;

	if (data->stream)
		pulse_stop_recording(data);
	pulse_unref();

	if (data->device)
		bfree(data->device);
	bfree(data);
}

static bool channel_map_compare(const pa_channel_map *a,
				const pa_channel_map *b)
{
	if (a->channels != b->channels)
		return true;

	for (uint8_t i = 0; i < a->channels; i++) {
		if (a->map[i] != b->map[i])
			return true;
	}

	return false;
}

/**
 * Update the input settings
 */
static void pulse_update(void *vptr, obs_data_t *settings)
{
	PULSE_DATA(vptr);
	bool restart = false;
	const char *new_device;
	pa_channel_map new_channel_map;

	new_device = obs_data_get_string(settings, "device_id");
	if (!data->device || strcmp(data->device, new_device) != 0) {
		if (data->device)
			bfree(data->device);
		data->device = bstrdup(new_device);
		data->is_default = strcmp("default", data->device) == 0;
		restart = true;
	}

	new_channel_map.channels = obs_data_get_int(settings, "pa_channels");
	for (size_t i = 0; i < PA_CHANNELS_MAX; i++) {
		char name[16];
		sprintf(name, "pa_map_%zu", i);
		new_channel_map.map[i] = obs_data_get_int(settings, name);
	}
	if (channel_map_compare(&new_channel_map, &data->channel_map)) {
		data->channel_map = new_channel_map;
		restart = true;
	}

	if (!restart)
		return;

	if (data->stream)
		pulse_stop_recording(data);
	pulse_start_recording(data);
}

/**
 * Create the plugin object
 */
static void *pulse_create(obs_data_t *settings, obs_source_t *source,
			  bool input)
{
	struct pulse_data *data = bzalloc(sizeof(struct pulse_data));

	data->input = input;
	data->source = source;

	pulse_init();
	pulse_update(data, settings);

	return data;
}

static void *pulse_input_create(obs_data_t *settings, obs_source_t *source)
{
	return pulse_create(settings, source, true);
}

static void *pulse_output_create(obs_data_t *settings, obs_source_t *source)
{
	return pulse_create(settings, source, false);
}

struct obs_source_info pulse_input_capture = {
	.id = ID_PREFIX "pulse_input_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = pulse_input_getname,
	.create = pulse_input_create,
	.destroy = pulse_destroy,
	.update = pulse_update,
	.get_defaults = pulse_defaults,
	.get_properties = pulse_input_properties,
	.icon_type = OBS_ICON_TYPE_AUDIO_INPUT,
};

struct obs_source_info pulse_output_capture = {
	.id = ID_PREFIX "pulse_output_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_DO_NOT_SELF_MONITOR,
	.get_name = pulse_output_getname,
	.create = pulse_output_create,
	.destroy = pulse_destroy,
	.update = pulse_update,
	.get_defaults = pulse_defaults,
	.get_properties = pulse_output_properties,
	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
};

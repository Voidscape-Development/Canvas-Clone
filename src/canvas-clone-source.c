/*
Canvas Clone
Copyright (C) 2025 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <plugin-support.h>

#include "canvas-clone-source.h"

/* Setting keys */
#define S_TARGET_TYPE "target_type"
#define S_CANVAS "canvas"
#define S_CANVAS_NAME "canvas_name"
#define S_SOURCE "source"
#define S_RENDER_MODE "render_mode"
#define S_AUDIO_MODE "audio_mode"
#define S_AUDIO_SOURCE "audio_source"

/* Setting values */
#define V_TARGET_CANVAS "canvas"
#define V_TARGET_SOURCE "source"
#define V_RENDER_OUTPUT "output"
#define V_RENDER_LIVE "live"
#define V_AUDIO_OFF "off"
#define V_AUDIO_TARGET "target"
#define V_AUDIO_SOURCE "source"

/* Sentinel used instead of a UUID so the main canvas always resolves */
#define MAIN_CANVAS_ID "main"

enum target_type {
	TARGET_CANVAS,
	TARGET_SOURCE,
};

enum render_mode {
	/* Copy of the canvas output texture, one frame behind at most */
	RENDER_OUTPUT,
	/* Re-render the canvas program source in place, no added latency */
	RENDER_LIVE,
};

enum audio_mode {
	AUDIO_OFF,
	/* Mirror the audio mix of the cloned canvas/source */
	AUDIO_TARGET,
	/* Mirror the audio mix of a separately picked source */
	AUDIO_SOURCE,
};

struct canvas_clone {
	obs_source_t *self;

	pthread_mutex_t mutex;

	/* Configuration and resolved targets, guarded by mutex */
	enum target_type target_type;
	enum render_mode render_mode;
	enum audio_mode audio_mode;
	char *canvas_id;
	char *canvas_name;
	char *source_id;
	char *audio_id;
	obs_weak_canvas_t *canvas_ref;
	obs_weak_source_t *source_ref;
	obs_weak_source_t *audio_ref;

	/* Graphics thread only */
	gs_texrender_t *texrender[2];
	enum gs_color_format texrender_format;
	size_t front;
	bool have_frame;
	bool rendering;
	bool querying_space;
	enum gs_color_space frame_space;
	uint32_t frame_cx;
	uint32_t frame_cy;

	/* Written on the graphics thread, read from anywhere */
	volatile long cx;
	volatile long cy;
};

/* ------------------------------------------------------------------------- */
/* Target resolution                                                         */

/* Returns a new reference, or NULL. Must be called with the mutex held. */
static obs_canvas_t *acquire_canvas(struct canvas_clone *clone)
{
	obs_canvas_t *canvas = NULL;

	if (clone->canvas_ref) {
		canvas = obs_weak_canvas_get_canvas(clone->canvas_ref);
		if (canvas && obs_canvas_removed(canvas)) {
			obs_canvas_release(canvas);
			canvas = NULL;
		}
		if (!canvas) {
			obs_weak_canvas_release(clone->canvas_ref);
			clone->canvas_ref = NULL;
		}
	}

	if (canvas)
		return canvas;

	if (!clone->canvas_id || !*clone->canvas_id)
		return NULL;

	if (strcmp(clone->canvas_id, MAIN_CANVAS_ID) == 0) {
		canvas = obs_get_main_canvas();
	} else {
		canvas = obs_get_canvas_by_uuid(clone->canvas_id);

		/* Canvases owned by other plugins are commonly recreated on
		 * startup with a fresh UUID, so fall back to the name that was
		 * saved alongside it. */
		if (!canvas && clone->canvas_name && *clone->canvas_name)
			canvas = obs_get_canvas_by_name(clone->canvas_name);
	}

	if (canvas && obs_canvas_removed(canvas)) {
		obs_canvas_release(canvas);
		canvas = NULL;
	}

	if (canvas)
		clone->canvas_ref = obs_canvas_get_weak_canvas(canvas);

	return canvas;
}

/* Returns a new reference, or NULL. Must be called with the mutex held. */
static obs_source_t *acquire_source(const char *uuid, obs_weak_source_t **ref)
{
	obs_source_t *source = NULL;

	if (*ref) {
		source = obs_weak_source_get_source(*ref);
		if (!source) {
			obs_weak_source_release(*ref);
			*ref = NULL;
		}
	}

	if (source)
		return source;

	if (!uuid || !*uuid)
		return NULL;

	source = obs_get_source_by_uuid(uuid);
	if (source)
		*ref = obs_source_get_weak_source(source);

	return source;
}

/* Returns a new reference to the source that produces the target's picture:
 * channel 0 of the canvas (the program transition, so scene switches and
 * transitions are included) or the picked source. */
static obs_source_t *acquire_program_source(struct canvas_clone *clone)
{
	obs_source_t *source = NULL;

	if (clone->target_type == TARGET_CANVAS) {
		obs_canvas_t *canvas = acquire_canvas(clone);
		if (canvas) {
			source = obs_canvas_get_channel(canvas, 0);
			obs_canvas_release(canvas);
		}
	} else {
		source = acquire_source(clone->source_id, &clone->source_ref);
	}

	return source;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */

static inline enum gs_color_space canvas_color_space(obs_canvas_t *canvas, uint32_t *cx, uint32_t *cy)
{
	struct obs_video_info ovi;

	if (!obs_canvas_get_video_info(canvas, &ovi)) {
		*cx = 0;
		*cy = 0;
		return GS_CS_SRGB;
	}

	*cx = ovi.base_width;
	*cy = ovi.base_height;

	return (ovi.colorspace == VIDEO_CS_2100_PQ || ovi.colorspace == VIDEO_CS_2100_HLG) ? GS_CS_709_EXTENDED
											   : GS_CS_SRGB;
}

static inline enum gs_color_space source_color_space(obs_source_t *source)
{
	static const enum gs_color_space preferred[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};

	return obs_source_get_color_space(source, OBS_COUNTOF(preferred), preferred);
}

static inline enum gs_color_format format_for_space(enum gs_color_space space)
{
	return space == GS_CS_SRGB ? GS_RGBA : GS_RGBA16F;
}

static void free_texrenders(struct canvas_clone *clone)
{
	for (size_t i = 0; i < 2; i++) {
		gs_texrender_destroy(clone->texrender[i]);
		clone->texrender[i] = NULL;
	}

	clone->have_frame = false;
}

/* Copies the target's picture into our own texture. Runs from video_tick,
 * where no canvas output texture is bound as a render target, which keeps a
 * clone that sits inside the canvas it is cloning both safe and useful: it
 * shows the previous frame instead of sampling a bound texture. */
static void capture_frame(struct canvas_clone *clone)
{
	obs_canvas_t *canvas = NULL;
	obs_source_t *source = NULL;
	enum gs_color_space space = GS_CS_SRGB;
	uint32_t cx = 0;
	uint32_t cy = 0;

	pthread_mutex_lock(&clone->mutex);
	if (clone->target_type == TARGET_CANVAS) {
		canvas = acquire_canvas(clone);
		if (canvas)
			space = canvas_color_space(canvas, &cx, &cy);
	} else {
		source = acquire_source(clone->source_id, &clone->source_ref);
		if (source && source != clone->self) {
			cx = obs_source_get_width(source);
			cy = obs_source_get_height(source);
			space = source_color_space(source);
		} else if (source) {
			obs_source_release(source);
			source = NULL;
		}
	}
	pthread_mutex_unlock(&clone->mutex);

	os_atomic_set_long(&clone->cx, (long)cx);
	os_atomic_set_long(&clone->cy, (long)cy);

	if ((!canvas && !source) || !cx || !cy) {
		clone->have_frame = false;
		goto done;
	}

	const enum gs_color_format format = format_for_space(space);

	obs_enter_graphics();

	if (clone->texrender_format != format)
		free_texrenders(clone);

	const size_t back = 1 - clone->front;

	if (!clone->texrender[back]) {
		clone->texrender[back] = gs_texrender_create(format, GS_ZS_NONE);
		clone->texrender_format = format;
	}

	gs_texrender_reset(clone->texrender[back]);

	if (gs_texrender_begin_with_color_space(clone->texrender[back], cx, cy, space)) {
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		/* A source target may contain this very clone; drawing the
		 * front buffer during re-entry is what makes that safe. */
		clone->rendering = true;
		if (canvas)
			obs_render_canvas_texture(canvas);
		else
			obs_source_video_render(source);
		clone->rendering = false;

		gs_blend_state_pop();
		gs_texrender_end(clone->texrender[back]);

		clone->front = back;
		clone->frame_space = space;
		clone->frame_cx = cx;
		clone->frame_cy = cy;
		clone->have_frame = true;
	}

	obs_leave_graphics();

done:
	obs_canvas_release(canvas);
	obs_source_release(source);
}

static void draw_captured_frame(struct canvas_clone *clone)
{
	gs_texture_t *texture = gs_texrender_get_texture(clone->texrender[clone->front]);
	if (!texture)
		return;

	/* Canvas output is premultiplied, so it is divided back out here and
	 * composited by whatever blend mode the scene item uses. */
	const enum gs_color_space current_space = gs_get_color_space();
	const char *tech_name = "DrawAlphaDivide";
	float multiplier = 1.0f;

	switch (current_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (clone->frame_space == GS_CS_709_EXTENDED)
			tech_name = "DrawAlphaDivideTonemap";
		break;
	case GS_CS_709_SCRGB:
		tech_name = "DrawMultiply";
		multiplier = obs_get_video_sdr_white_level() / 80.0f;
		break;
	case GS_CS_709_EXTENDED:
		tech_name = "Draw";
		break;
	}

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *param = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture_srgb(param, texture);
	param = gs_effect_get_param_by_name(effect, "multiplier");
	gs_effect_set_float(param, multiplier);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	while (gs_effect_loop(effect, tech_name))
		gs_draw_sprite(texture, 0, clone->frame_cx, clone->frame_cy);

	gs_enable_framebuffer_srgb(previous);
}

static void canvas_clone_video_tick(void *data, float seconds)
{
	struct canvas_clone *clone = data;

	UNUSED_PARAMETER(seconds);

	pthread_mutex_lock(&clone->mutex);
	const enum render_mode mode = clone->render_mode;
	pthread_mutex_unlock(&clone->mutex);

	if (mode == RENDER_OUTPUT) {
		capture_frame(clone);
		return;
	}

	/* Live mode draws straight from the target, so only the reported size
	 * needs refreshing here. */
	uint32_t cx = 0;
	uint32_t cy = 0;

	pthread_mutex_lock(&clone->mutex);
	if (clone->target_type == TARGET_CANVAS) {
		obs_canvas_t *canvas = acquire_canvas(clone);
		if (canvas) {
			canvas_color_space(canvas, &cx, &cy);
			obs_canvas_release(canvas);
		}
	} else {
		obs_source_t *source = acquire_source(clone->source_id, &clone->source_ref);
		if (source) {
			if (source != clone->self) {
				cx = obs_source_get_width(source);
				cy = obs_source_get_height(source);
			}
			obs_source_release(source);
		}
	}
	pthread_mutex_unlock(&clone->mutex);

	if (clone->texrender[0] || clone->texrender[1]) {
		obs_enter_graphics();
		free_texrenders(clone);
		obs_leave_graphics();
	}

	os_atomic_set_long(&clone->cx, (long)cx);
	os_atomic_set_long(&clone->cy, (long)cy);
}

static void canvas_clone_video_render(void *data, gs_effect_t *effect)
{
	struct canvas_clone *clone = data;

	UNUSED_PARAMETER(effect);

	pthread_mutex_lock(&clone->mutex);
	const enum render_mode mode = clone->render_mode;
	obs_source_t *source = mode == RENDER_LIVE ? acquire_program_source(clone) : NULL;
	pthread_mutex_unlock(&clone->mutex);

	if (mode == RENDER_OUTPUT) {
		if (clone->have_frame)
			draw_captured_frame(clone);
		return;
	}

	if (!source)
		return;

	/* Nothing is buffered in live mode, so a clone placed inside the canvas
	 * it clones has to stop here rather than recurse. */
	if (source != clone->self && !clone->rendering) {
		clone->rendering = true;
		obs_source_video_render(source);
		clone->rendering = false;
	}

	obs_source_release(source);
}

static uint32_t canvas_clone_get_width(void *data)
{
	struct canvas_clone *clone = data;

	return (uint32_t)os_atomic_load_long(&clone->cx);
}

static uint32_t canvas_clone_get_height(void *data)
{
	struct canvas_clone *clone = data;

	return (uint32_t)os_atomic_load_long(&clone->cy);
}

static enum gs_color_space canvas_clone_get_color_space(void *data, size_t count,
							const enum gs_color_space *preferred_spaces)
{
	struct canvas_clone *clone = data;
	enum gs_color_space space = GS_CS_SRGB;

	/* Composite targets ask their children for a color space, so a clone
	 * that ends up inside its own target must not answer recursively. */
	if (clone->querying_space)
		return count ? preferred_spaces[0] : space;

	clone->querying_space = true;

	pthread_mutex_lock(&clone->mutex);
	if (clone->target_type == TARGET_CANVAS) {
		obs_canvas_t *canvas = acquire_canvas(clone);
		if (canvas) {
			uint32_t cx;
			uint32_t cy;

			space = canvas_color_space(canvas, &cx, &cy);
			obs_canvas_release(canvas);
		}
	} else {
		obs_source_t *source = acquire_source(clone->source_id, &clone->source_ref);
		if (source) {
			if (source != clone->self)
				space = source_color_space(source);
			obs_source_release(source);
		}
	}
	pthread_mutex_unlock(&clone->mutex);

	clone->querying_space = false;

	for (size_t i = 0; i < count; i++) {
		if (preferred_spaces[i] == space)
			return space;
	}

	return count ? preferred_spaces[0] : space;
}

/* ------------------------------------------------------------------------- */
/* Audio                                                                     */

static bool canvas_clone_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output,
				      uint32_t mixers, size_t channels, size_t sample_rate)
{
	struct canvas_clone *clone = data;
	obs_source_t *target = NULL;

	UNUSED_PARAMETER(sample_rate);

	pthread_mutex_lock(&clone->mutex);
	if (clone->audio_mode == AUDIO_TARGET)
		target = acquire_program_source(clone);
	else if (clone->audio_mode == AUDIO_SOURCE)
		target = acquire_source(clone->audio_id, &clone->audio_ref);
	pthread_mutex_unlock(&clone->mutex);

	if (!target || target == clone->self) {
		obs_source_release(target);
		*ts_out = 0;
		return false;
	}

	/* The target composites its own mix once per audio tick; this reads the
	 * result of that, which keeps transition fades and per-source volumes
	 * intact. It requires the target to be live somewhere else in OBS. */
	if (obs_source_audio_pending(target)) {
		obs_source_release(target);
		*ts_out = 0;
		return false;
	}

	const uint64_t timestamp = obs_source_get_audio_timestamp(target);
	if (!timestamp) {
		obs_source_release(target);
		*ts_out = 0;
		return false;
	}

	struct obs_source_audio_mix target_audio;
	obs_source_get_audio_mix(target, &target_audio);

	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t ch = 0; ch < channels; ch++)
			memcpy(audio_output->output[mix].data[ch], target_audio.output[mix].data[ch],
			       AUDIO_OUTPUT_FRAMES * sizeof(float));
	}

	obs_source_release(target);

	*ts_out = timestamp;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Properties                                                                */

static bool add_canvas_to_list(void *param, obs_canvas_t *canvas)
{
	obs_property_t *list = param;

	if (!obs_canvas_has_video(canvas) || obs_canvas_removed(canvas))
		return true;

	const uint32_t flags = obs_canvas_get_flags(canvas);

	if (flags & MAIN) {
		obs_property_list_add_string(list, obs_module_text("Canvas.Main"), MAIN_CANVAS_ID);
		return true;
	}

	const char *name = obs_canvas_get_name(canvas);
	obs_property_list_add_string(list, name && *name ? name : obs_module_text("Canvas.Unnamed"),
				     obs_canvas_get_uuid(canvas));

	return true;
}

struct scene_enum_ctx {
	obs_property_t *list;
	const char *prefix;
	bool audio;
};

static void add_source_to_list(struct scene_enum_ctx *ctx, obs_source_t *source)
{
	const uint32_t flags = obs_source_get_output_flags(source);
	const uint32_t wanted = ctx->audio ? (OBS_SOURCE_AUDIO | OBS_SOURCE_COMPOSITE) : OBS_SOURCE_VIDEO;

	if ((flags & wanted) == 0)
		return;

	const char *name = obs_source_get_name(source);
	if (!name)
		return;

	if (ctx->prefix) {
		struct dstr label = {0};

		dstr_printf(&label, "%s: %s", ctx->prefix, name);
		obs_property_list_add_string(ctx->list, label.array, obs_source_get_uuid(source));
		dstr_free(&label);
	} else {
		obs_property_list_add_string(ctx->list, name, obs_source_get_uuid(source));
	}
}

static bool add_scene_to_list(void *param, obs_source_t *source)
{
	add_source_to_list(param, source);
	return true;
}

static bool add_input_to_list(void *param, obs_source_t *source)
{
	add_source_to_list(param, source);
	return true;
}

static bool add_canvas_scenes_to_list(void *param, obs_canvas_t *canvas)
{
	struct scene_enum_ctx *ctx = param;
	const uint32_t flags = obs_canvas_get_flags(canvas);
	const char *name = obs_canvas_get_name(canvas);

	if (obs_canvas_removed(canvas))
		return true;

	/* Scenes of the main canvas are listed unprefixed; scenes belonging to
	 * another canvas are prefixed with the canvas they live on. */
	ctx->prefix = (flags & MAIN) ? NULL : name;
	obs_canvas_enum_scenes(canvas, add_scene_to_list, ctx);
	ctx->prefix = NULL;

	return true;
}

static void fill_source_list(obs_property_t *list, bool audio)
{
	struct scene_enum_ctx ctx = {
		.list = list,
		.prefix = NULL,
		.audio = audio,
	};

	obs_enum_canvases(add_canvas_scenes_to_list, &ctx);
	obs_enum_sources(add_input_to_list, &ctx);
}

/* Keeps the human readable canvas name in settings so a canvas that another
 * plugin recreates with a new UUID can still be found by name. */
static void store_canvas_name(obs_data_t *settings)
{
	const char *id = obs_data_get_string(settings, S_CANVAS);

	if (!id || !*id || strcmp(id, MAIN_CANVAS_ID) == 0)
		return;

	obs_canvas_t *canvas = obs_get_canvas_by_uuid(id);
	if (!canvas)
		return;

	const char *name = obs_canvas_get_name(canvas);
	if (name && *name)
		obs_data_set_string(settings, S_CANVAS_NAME, name);

	obs_canvas_release(canvas);
}

static bool target_type_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const bool is_canvas = strcmp(obs_data_get_string(settings, S_TARGET_TYPE), V_TARGET_SOURCE) != 0;

	UNUSED_PARAMETER(property);

	obs_property_set_visible(obs_properties_get(props, S_CANVAS), is_canvas);
	obs_property_set_visible(obs_properties_get(props, S_SOURCE), !is_canvas);

	store_canvas_name(settings);

	return true;
}

static bool audio_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const bool pick_source = strcmp(obs_data_get_string(settings, S_AUDIO_MODE), V_AUDIO_SOURCE) == 0;

	UNUSED_PARAMETER(property);

	obs_property_set_visible(obs_properties_get(props, S_AUDIO_SOURCE), pick_source);

	return true;
}

/* A canvas owned by another plugin may not exist yet (or at all); keep the
 * saved selection visible in the list instead of silently resetting it. */
static void keep_missing_canvas_in_list(obs_property_t *list, struct canvas_clone *clone)
{
	pthread_mutex_lock(&clone->mutex);

	const char *id = clone->canvas_id;
	if (!id || !*id)
		goto unlock;

	const size_t count = obs_property_list_item_count(list);
	for (size_t i = 0; i < count; i++) {
		if (strcmp(obs_property_list_item_string(list, i), id) == 0)
			goto unlock;
	}

	struct dstr label = {0};

	dstr_printf(&label, "%s %s", clone->canvas_name && *clone->canvas_name ? clone->canvas_name : id,
		    obs_module_text("Canvas.Missing"));
	obs_property_list_add_string(list, label.array, id);
	dstr_free(&label);

unlock:
	pthread_mutex_unlock(&clone->mutex);
}

static obs_properties_t *canvas_clone_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;

	prop = obs_properties_add_list(props, S_TARGET_TYPE, obs_module_text("TargetType"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("TargetType.Canvas"), V_TARGET_CANVAS);
	obs_property_list_add_string(prop, obs_module_text("TargetType.Source"), V_TARGET_SOURCE);
	obs_property_set_modified_callback(prop, target_type_modified);

	prop = obs_properties_add_list(props, S_CANVAS, obs_module_text("Canvas"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_enum_canvases(add_canvas_to_list, prop);
	if (data)
		keep_missing_canvas_in_list(prop, data);

	prop = obs_properties_add_list(props, S_SOURCE, obs_module_text("Source"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Source.None"), "");
	fill_source_list(prop, false);

	prop = obs_properties_add_list(props, S_RENDER_MODE, obs_module_text("RenderMode"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("RenderMode.Output"), V_RENDER_OUTPUT);
	obs_property_list_add_string(prop, obs_module_text("RenderMode.Live"), V_RENDER_LIVE);
	obs_property_set_long_description(prop, obs_module_text("RenderMode.Description"));

	prop = obs_properties_add_list(props, S_AUDIO_MODE, obs_module_text("AudioMode"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("AudioMode.Off"), V_AUDIO_OFF);
	obs_property_list_add_string(prop, obs_module_text("AudioMode.Target"), V_AUDIO_TARGET);
	obs_property_list_add_string(prop, obs_module_text("AudioMode.Source"), V_AUDIO_SOURCE);
	obs_property_set_long_description(prop, obs_module_text("AudioMode.Description"));
	obs_property_set_modified_callback(prop, audio_mode_modified);

	prop = obs_properties_add_list(props, S_AUDIO_SOURCE, obs_module_text("AudioSource"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("Source.None"), "");
	fill_source_list(prop, true);

	return props;
}

static void canvas_clone_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_TARGET_TYPE, V_TARGET_CANVAS);
	obs_data_set_default_string(settings, S_CANVAS, MAIN_CANVAS_ID);
	obs_data_set_default_string(settings, S_RENDER_MODE, V_RENDER_OUTPUT);
	obs_data_set_default_string(settings, S_AUDIO_MODE, V_AUDIO_OFF);
}

/* ------------------------------------------------------------------------- */
/* Source callbacks                                                          */

static const char *canvas_clone_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);

	return obs_module_text("CanvasClone");
}

static inline void replace_string(char **dst, const char *value)
{
	bfree(*dst);
	*dst = bstrdup(value ? value : "");
}

static void canvas_clone_update(void *data, obs_data_t *settings)
{
	struct canvas_clone *clone = data;

	const char *target_type = obs_data_get_string(settings, S_TARGET_TYPE);
	const char *canvas_id = obs_data_get_string(settings, S_CANVAS);
	const char *canvas_name = obs_data_get_string(settings, S_CANVAS_NAME);
	const char *source_id = obs_data_get_string(settings, S_SOURCE);
	const char *render_mode = obs_data_get_string(settings, S_RENDER_MODE);
	const char *audio_mode = obs_data_get_string(settings, S_AUDIO_MODE);
	const char *audio_id = obs_data_get_string(settings, S_AUDIO_SOURCE);

	pthread_mutex_lock(&clone->mutex);

	clone->target_type = strcmp(target_type, V_TARGET_SOURCE) == 0 ? TARGET_SOURCE : TARGET_CANVAS;
	clone->render_mode = strcmp(render_mode, V_RENDER_LIVE) == 0 ? RENDER_LIVE : RENDER_OUTPUT;

	if (strcmp(audio_mode, V_AUDIO_TARGET) == 0)
		clone->audio_mode = AUDIO_TARGET;
	else if (strcmp(audio_mode, V_AUDIO_SOURCE) == 0)
		clone->audio_mode = AUDIO_SOURCE;
	else
		clone->audio_mode = AUDIO_OFF;

	if (!clone->canvas_id || strcmp(clone->canvas_id, canvas_id) != 0) {
		replace_string(&clone->canvas_id, canvas_id);
		obs_weak_canvas_release(clone->canvas_ref);
		clone->canvas_ref = NULL;
	}

	replace_string(&clone->canvas_name, canvas_name);

	if (!clone->source_id || strcmp(clone->source_id, source_id) != 0) {
		replace_string(&clone->source_id, source_id);
		obs_weak_source_release(clone->source_ref);
		clone->source_ref = NULL;
	}

	if (!clone->audio_id || strcmp(clone->audio_id, audio_id) != 0) {
		replace_string(&clone->audio_id, audio_id);
		obs_weak_source_release(clone->audio_ref);
		clone->audio_ref = NULL;
	}

	const bool audio_active = clone->audio_mode != AUDIO_OFF;

	pthread_mutex_unlock(&clone->mutex);

	/* Only put an entry in the audio mixer when audio is actually cloned. */
	obs_source_set_audio_active(clone->self, audio_active);

	store_canvas_name(settings);
}

static void *canvas_clone_create(obs_data_t *settings, obs_source_t *source)
{
	struct canvas_clone *clone = bzalloc(sizeof(struct canvas_clone));

	clone->self = source;
	clone->texrender_format = GS_RGBA;
	pthread_mutex_init(&clone->mutex, NULL);

	canvas_clone_update(clone, settings);

	return clone;
}

static void canvas_clone_destroy(void *data)
{
	struct canvas_clone *clone = data;

	obs_enter_graphics();
	free_texrenders(clone);
	obs_leave_graphics();

	obs_weak_canvas_release(clone->canvas_ref);
	obs_weak_source_release(clone->source_ref);
	obs_weak_source_release(clone->audio_ref);

	bfree(clone->canvas_id);
	bfree(clone->canvas_name);
	bfree(clone->source_id);
	bfree(clone->audio_id);

	pthread_mutex_destroy(&clone->mutex);
	bfree(clone);
}

struct obs_source_info canvas_clone_source_info = {
	.id = "canvas_clone",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE |
			OBS_SOURCE_SRGB,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
	.get_name = canvas_clone_get_name,
	.create = canvas_clone_create,
	.destroy = canvas_clone_destroy,
	.update = canvas_clone_update,
	.get_defaults = canvas_clone_defaults,
	.get_properties = canvas_clone_properties,
	.get_width = canvas_clone_get_width,
	.get_height = canvas_clone_get_height,
	.video_tick = canvas_clone_video_tick,
	.video_render = canvas_clone_video_render,
	.video_get_color_space = canvas_clone_get_color_space,
	.audio_render = canvas_clone_audio_render,
};

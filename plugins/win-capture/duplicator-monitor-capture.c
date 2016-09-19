#include <windows.h>

#include <obs-module.h>
#include <util/dstr.h>

#include "cursor-capture.h"
#include "select-region.h"

#define do_log(level, format, ...) \
	blog(level, "[duplicator-monitor-capture: '%s'] " format, \
			obs_source_get_name(capture->source), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

#define TEXT_MONITOR_CAPTURE obs_module_text("MonitorCapture")
#define TEXT_CAPTURE_CURSOR  obs_module_text("CaptureCursor")
#define TEXT_COMPATIBILITY   obs_module_text("Compatibility")
#define TEXT_MONITOR         obs_module_text("Monitor")
#define TEXT_PRIMARY_MONITOR obs_module_text("PrimaryMonitor")
#define TEXT_SUBREGION       obs_module_text("Subregion")
#define TEXT_SUBREGION_X     obs_module_text("Subregion.X")
#define TEXT_SUBREGION_Y     obs_module_text("Subregion.Y")
#define TEXT_SUBREGION_W     obs_module_text("Subregion.Width")
#define TEXT_SUBREGION_H     obs_module_text("Subregion.Height")
#define TEXT_SELECT_REGION   obs_module_text("Subregion.Select")
#define TEXT_RESET_REGION    obs_module_text("Subregion.Reset")

#define RESET_INTERVAL_SEC 3.0f

struct duplicator_capture {
	obs_source_t                   *source;
	int                            monitor;
	bool                           capture_cursor;
	bool                           showing;

	bool                           use_subregion;
	long                           subregion_x;
	long                           subregion_y;
	long                           subregion_cx;
	long                           subregion_cy;

	long                           x;
	long                           y;
	int                            rot;
	uint32_t                       width;
	uint32_t                       height;
	gs_duplicator_t                *duplicator;
	float                          reset_timeout;
	struct cursor_data             cursor_data;
};

/* ------------------------------------------------------------------------- */


static inline void update_settings(struct duplicator_capture *capture,
		obs_data_t *settings)
{
	capture->monitor        = (int)obs_data_get_int(settings, "monitor");
	capture->capture_cursor = obs_data_get_bool(settings, "capture_cursor");
	capture->use_subregion  = obs_data_get_bool(settings, "use_subregion");
#define get_long(x) (long)obs_data_get_int(settings, x)
	capture->subregion_x    = get_long("subregion_x");
	capture->subregion_y    = get_long("subregion_y");
	capture->subregion_cx   = get_long("subregion_cx");
	capture->subregion_cy   = get_long("subregion_cy");
#undef get_long

	obs_enter_graphics();

	gs_duplicator_destroy(capture->duplicator);
	capture->duplicator = gs_duplicator_create(capture->monitor);
	capture->width = 0;
	capture->height = 0;
	capture->x = 0;
	capture->y = 0;
	capture->rot = 0;
	capture->reset_timeout = 0.0f;

	obs_leave_graphics();
}

/* ------------------------------------------------------------------------- */

static const char *duplicator_capture_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_MONITOR_CAPTURE;
}

static void duplicator_capture_destroy(void *data)
{
	struct duplicator_capture *capture = data;

	obs_enter_graphics();

	gs_duplicator_destroy(capture->duplicator);
	cursor_data_free(&capture->cursor_data);

	obs_leave_graphics();

	bfree(capture);
}

static void duplicator_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "monitor", 0);
	obs_data_set_default_bool(settings, "capture_cursor", true);
}

static void duplicator_capture_update(void *data, obs_data_t *settings)
{
	struct duplicator_capture *mc = data;
	update_settings(mc, settings);
}

static void *duplicator_capture_create(obs_data_t *settings,
		obs_source_t *source)
{
	struct duplicator_capture *capture;

	capture = bzalloc(sizeof(struct duplicator_capture));
	capture->source = source;

	update_settings(capture, settings);

	return capture;
}

static void reset_capture_data(struct duplicator_capture *capture)
{
	struct gs_monitor_info monitor_info = {0};
	gs_texture_t *texture = gs_duplicator_get_texture(capture->duplicator);

	gs_get_duplicator_monitor_info(capture->monitor, &monitor_info);
	capture->width = gs_texture_get_width(texture);
	capture->height = gs_texture_get_height(texture);
	capture->x = monitor_info.x;
	capture->y = monitor_info.y;
	capture->rot = monitor_info.rotation_degrees;
}

static void free_capture_data(struct duplicator_capture *capture)
{
	gs_duplicator_destroy(capture->duplicator);
	cursor_data_free(&capture->cursor_data);
	capture->duplicator = NULL;
	capture->width = 0;
	capture->height = 0;
	capture->x = 0;
	capture->y = 0;
	capture->rot = 0;
	capture->reset_timeout = 0.0f;
}

static void duplicator_capture_tick(void *data, float seconds)
{
	struct duplicator_capture *capture = data;

	/* completely shut down monitor capture if not in use, otherwise it can
	 * sometimes generate system lag when a game is in fullscreen mode */
	if (!obs_source_showing(capture->source)) {
		if (capture->showing) {
			obs_enter_graphics();
			free_capture_data(capture);
			obs_leave_graphics();

			capture->showing = false;
		}
		return;

	/* always try to load the capture immediately when the source is first
	 * shown */
	} else if (!capture->showing) {
		capture->reset_timeout = RESET_INTERVAL_SEC;
	}

	obs_enter_graphics();

	if (!capture->duplicator) {
		capture->reset_timeout += seconds;

		if (capture->reset_timeout >= RESET_INTERVAL_SEC) {
			capture->duplicator =
				gs_duplicator_create(capture->monitor);

			capture->reset_timeout = 0.0f;
		}
	}

	if (!!capture->duplicator) {
		if (capture->capture_cursor)
			cursor_capture(&capture->cursor_data);

		if (!gs_duplicator_update_frame(capture->duplicator)) {
			free_capture_data(capture);

		} else if (capture->width == 0) {
			reset_capture_data(capture);
		}
	}

	obs_leave_graphics();

	if (!capture->showing)
		capture->showing = true;

	UNUSED_PARAMETER(seconds);
}

static uint32_t duplicator_capture_width(void *data)
{
	struct duplicator_capture *capture = data;
	if (capture->use_subregion)
		return capture->subregion_cx;

	return capture->rot % 180 == 0 ? capture->width : capture->height;
}

static uint32_t duplicator_capture_height(void *data)
{
	struct duplicator_capture *capture = data;
	if (capture->use_subregion)
		return capture->subregion_cy;

	return capture->rot % 180 == 0 ? capture->height : capture->width;
}

static void draw_cursor(struct duplicator_capture *capture)
{
	long x = -capture->x;
	long y = -capture->y;

	if (capture->use_subregion) {
		x -= capture->subregion_x;
		y -= capture->subregion_y;

		long cursor_x = capture->cursor_data.cursor_pos.x + x;
		long cursor_y = capture->cursor_data.cursor_pos.y + y;

		if (cursor_x < 0 ||
		    cursor_y < 0 ||
		    cursor_x > capture->subregion_cx ||
		    cursor_y > capture->subregion_cy)
			return;
	}

	cursor_draw(&capture->cursor_data, x, y, 1.0f, 1.0f,
		capture->rot % 180 == 0 ? capture->width : capture->height,
		capture->rot % 180 == 0 ? capture->height : capture->width);
}

static void duplicator_capture_render(void *data, gs_effect_t *effect)
{
	struct duplicator_capture *capture = data;
	gs_texture_t *texture;
	int rot;

	if (!capture->duplicator)
		return;

	texture = gs_duplicator_get_texture(capture->duplicator);
	if (!texture)
		return;

	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	rot = capture->rot;

	while (gs_effect_loop(effect, "Draw")) {
		if (rot != 0) {
			float x = 0.0f;
			float y = 0.0f;

			switch (rot) {
			case 90:
				x = (float)capture->height;
				break;
			case 180:
				x = (float)capture->width;
				y = (float)capture->height;
				break;
			case 270:
				y = (float)capture->width;
				break;
			}

			gs_matrix_push();
			gs_matrix_translate3f(x, y, 0.0f);
			gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD((float)rot));
		}

		if (capture->use_subregion) {
			gs_eparam_t *param = gs_effect_get_param_by_name(
					effect, "image");
			gs_effect_set_texture(param, texture);

			gs_draw_sprite_subregion(texture, 0,
					capture->subregion_x,
					capture->subregion_y,
					capture->subregion_cx,
					capture->subregion_cy);
		} else {
			obs_source_draw(texture, 0, 0, 0, 0, false);
		}

		if (rot != 0)
			gs_matrix_pop();
	}

	if (capture->capture_cursor) {
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

		while (gs_effect_loop(effect, "Draw")) {
			draw_cursor(capture);
		}
	}
}

static bool get_monitor_props(obs_property_t *monitor_list, int monitor_idx)
{
	struct dstr monitor_desc = {0};
	struct gs_monitor_info info;

	if (!gs_get_duplicator_monitor_info(monitor_idx, &info))
		return false;

	dstr_catf(&monitor_desc, "%s %d: %ldx%ld @ %ld,%ld",
			TEXT_MONITOR, monitor_idx,
			info.cx, info.cy, info.x, info.y);

	obs_property_list_add_int(monitor_list, monitor_desc.array,
			monitor_idx);

	dstr_free(&monitor_desc);

	return true;
}

static void select_region(void *data, POINT pos, SIZE size)
{
	struct duplicator_capture *capture = data;
	obs_data_t *s = obs_source_get_settings(capture->source);
	obs_data_set_int(s, "subregion_x", pos.x);
	obs_data_set_int(s, "subregion_y", pos.y);
	obs_data_set_int(s, "subregion_cx", size.cx);
	obs_data_set_int(s, "subregion_cy", size.cy);
	obs_data_release(s);

	obs_source_update_properties(capture->source);
	obs_source_update(capture->source, NULL);
}

static bool select_region_clicked(obs_properties_t *props, obs_property_t *p,
		void *data)
{
	struct duplicator_capture *capture = data;
	obs_data_t *s = obs_source_get_settings(capture->source);
	int idx = (int)obs_data_get_int(s, "monitor");
	struct gs_monitor_info info;
	RECT r = {0};
	bool success;

	obs_data_release(s);

	obs_enter_graphics();
	success = gs_get_duplicator_monitor_info(idx, &info);
	obs_leave_graphics();

	if (!success)
		return false;

	r.left   = info.x;
	r.top    = info.y;
	r.right  = info.x + info.cx;
	r.bottom = info.y + info.cy;
	select_region_begin(select_region, r, capture);
	
	return false;
}

static bool reset_region_clicked(obs_properties_t *props, obs_property_t *p,
		void *data)
{
	struct duplicator_capture *capture = data;
	obs_data_t *s = obs_source_get_settings(capture->source);
	int idx = (int)obs_data_get_int(s, "monitor");
	struct gs_monitor_info info;
	bool success;

	obs_enter_graphics();
	success = gs_get_duplicator_monitor_info(idx, &info);
	obs_leave_graphics();

	if (success) {
		obs_data_set_int(s, "subregion_x", 0);
		obs_data_set_int(s, "subregion_y", 0);
		obs_data_set_int(s, "subregion_cx", info.cx);
		obs_data_set_int(s, "subregion_cy", info.cy);
	}

	obs_data_release(s);

	obs_source_update(capture->source, NULL);
	return success;
}

struct dup_property_info {
	int last_monitor_idx;
};

static bool monitor_changed(obs_properties_t *props, obs_property_t *p,
		obs_data_t *s)
{
	struct dup_property_info *info = obs_properties_get_param(props);
	struct gs_monitor_info monitor_info = {0};
	int idx = (int)obs_data_get_int(s, "monitor");

	if (info->last_monitor_idx == idx)
		return false;
	
	obs_enter_graphics();
	gs_get_duplicator_monitor_info(idx, &monitor_info);
	obs_leave_graphics();

	if (info->last_monitor_idx != -1) {
		obs_data_set_int(s, "subregion_x", 0);
		obs_data_set_int(s, "subregion_y", 0);
		obs_data_set_int(s, "subregion_cx", monitor_info.cx);
		obs_data_set_int(s, "subregion_cy", monitor_info.cy);
	}

#define set_limit(x, min, max, step) \
	obs_property_int_set_limits(obs_properties_get(props, x), \
			min, max, step)

	set_limit("subregion_x", 0, monitor_info.cx, 1);
	set_limit("subregion_y", 0, monitor_info.cx, 1);
	set_limit("subregion_cx", 0, monitor_info.cx, 1);
	set_limit("subregion_cy", 0, monitor_info.cx, 1);

#undef set_limit

	info->last_monitor_idx = idx;
	return true;
}

static bool use_subregion_clicked(obs_properties_t *props, obs_property_t *p,
		obs_data_t *s)
{
	bool use_subregion = obs_data_get_bool(s, "use_subregion");

#define set_vis(prop) \
	obs_property_set_visible(obs_properties_get(props, prop), \
			use_subregion)

	set_vis("subregion_x");
	set_vis("subregion_y");
	set_vis("subregion_cx");
	set_vis("subregion_cy");
	set_vis("select_subregion");
	set_vis("reset_subregion");

#undef set_vis

	return true;
}

static obs_properties_t *duplicator_capture_properties(void *unused)
{
	int monitor_idx = 0;

	struct dup_property_info *info = bzalloc(sizeof(*info));
	info->last_monitor_idx = -1;

	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create_param(info, bfree);
	obs_property_t *p;

	obs_property_t *monitors = obs_properties_add_list(props,
		"monitor", TEXT_MONITOR,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback(monitors, monitor_changed);

	obs_properties_add_bool(props, "capture_cursor", TEXT_CAPTURE_CURSOR);

	obs_enter_graphics();

	while (get_monitor_props(monitors, monitor_idx++));

	p = obs_properties_add_bool(props, "use_subregion", TEXT_SUBREGION);
	obs_property_set_modified_callback(p, use_subregion_clicked);

	obs_properties_add_int(props, "subregion_x", TEXT_SUBREGION_X,
			0, 20000, 1);
	obs_properties_add_int(props, "subregion_y", TEXT_SUBREGION_Y,
			0, 20000, 1);
	obs_properties_add_int(props, "subregion_cx", TEXT_SUBREGION_W,
			0, 20000, 1);
	obs_properties_add_int(props, "subregion_cy", TEXT_SUBREGION_H,
			0, 20000, 1);

	obs_properties_add_button(props, "select_subregion",
			TEXT_SELECT_REGION, select_region_clicked);
	obs_properties_add_button(props, "reset_subregion",
			TEXT_RESET_REGION, reset_region_clicked);

	obs_leave_graphics();

	return props;
}

struct obs_source_info duplicator_capture_info = {
	.id             = "monitor_capture",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
	                  OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = duplicator_capture_getname,
	.create         = duplicator_capture_create,
	.destroy        = duplicator_capture_destroy,
	.video_render   = duplicator_capture_render,
	.video_tick     = duplicator_capture_tick,
	.update         = duplicator_capture_update,
	.get_width      = duplicator_capture_width,
	.get_height     = duplicator_capture_height,
	.get_defaults   = duplicator_capture_defaults,
	.get_properties = duplicator_capture_properties
};

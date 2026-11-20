#include "mango/animation/layer.h"
#include "mango/animation/common.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/ext-protocol/glass-lens.h"
#include "mango/manage/client.h"
#include "mango/manage/layer.h"
#include "mango/manage/monitor.h"
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>

static int32_t layer_open_animation_type(const LayerSurface *l) {
	return l->animation_type_open != ANIM_TYPE_UNSET
			   ? l->animation_type_open
			   : config.layer_animation_type_open;
}

static int32_t layer_close_animation_type(const LayerSurface *l) {
	return l->animation_type_close != ANIM_TYPE_UNSET
			   ? l->animation_type_close
			   : config.layer_animation_type_close;
}

static void layer_expand_origin(LayerSurface *l, const struct wlr_box *box) {
	uint32_t anchor = l->layer_surface->current.anchor;
	bool left = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	bool right = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	bool top = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	bool bottom = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	l->expand_origin_x =
		box->x + (left != right ? (right ? box->width : 0) : box->width / 2.0);
	l->expand_origin_y = box->y + (top != bottom ? (bottom ? box->height : 0)
												 : box->height / 2.0);
}

static int32_t layer_expand_radius(LayerSurface *l,
								 const struct wlr_box *box) {
	return MANGO_MIN(l->glass_radius, MANGO_MIN(box->width, box->height) / 2);
}

struct layer_expand_content {
	double opacity;
	int32_t radius;
};

static void layer_expand_content(struct wlr_scene_buffer *buffer, int32_t sx,
								 int32_t sy, void *data) {
	struct layer_expand_content *content = data;
	wlr_scene_buffer_set_opacity(buffer, content->opacity);
	wlr_scene_buffer_set_corner_radius(buffer, content->radius);
}

static void layer_expand_glass(LayerSurface *l, const struct wlr_box *box,
							   double progress) {
	if (!l->blur || !config.blur ||
		(!l->glass && (!config.blur_layer || l->no_blur)))
		return;
	/* One scene update for the box; strength and alpha only redraw. The
	 * parent is the layer's tree, or a close snapshot's glass wrapper. */
	int32_t x, y;
	wlr_scene_node_coords(&l->blur->node.parent->node, &x, &y);
	wlr_scene_blur_set_geometry(l->blur, box->x - x, box->y - y, box->width,
								box->height,
								corner_radii_all(layer_expand_radius(l, box)));
	wlr_scene_blur_set_strength(l->blur, progress);
	wlr_scene_blur_set_alpha(l->blur, progress);
	wlr_scene_blur_set_glass_lens(l->blur, NULL, 0, NULL);
}

void layer_actual_size(LayerSurface *l, int32_t *width, int32_t *height) {
	if (l->animation.running || l->expand_opening) {
		*width = l->animation.current.width;
		*height = l->animation.current.height;
	} else {
		*width = l->geom.width;
		*height = l->geom.height;
	}
}

static void layer_consume_exclusive(LayerSurface *it, struct wlr_box *box) {
	const struct wlr_layer_surface_v1_state *state =
		&it->layer_surface->current;
	if (state->exclusive_zone <= 0)
		return;

	switch (wlr_layer_surface_v1_get_exclusive_edge(it->layer_surface)) {
	case WLR_EDGE_TOP:
		box->y += state->exclusive_zone + state->margin.top;
		box->height -= state->exclusive_zone + state->margin.top;
		break;
	case WLR_EDGE_BOTTOM:
		box->height -= state->exclusive_zone + state->margin.bottom;
		break;
	case WLR_EDGE_LEFT:
		box->x += state->exclusive_zone + state->margin.left;
		box->width -= state->exclusive_zone + state->margin.left;
		break;
	case WLR_EDGE_RIGHT:
		box->width -= state->exclusive_zone + state->margin.right;
		break;
	case WLR_EDGE_NONE:
		break;
	}

	if (box->width < 0)
		box->width = 0;
	if (box->height < 0)
		box->height = 0;
}

static struct wlr_box layer_bound_area(LayerSurface *l) {
	const struct wlr_layer_surface_v1_state *state = &l->layer_surface->current;

	if (state->exclusive_zone < 0)
		return l->mon->m;
	if (state->exclusive_zone == 0)
		return l->mon->w;

	struct wlr_box box = l->mon->m;
	bool reached = false;
	for (int32_t i = 3; i >= 0 && !reached; i--) {
		LayerSurface *it;
		wl_list_for_each(it, &l->mon->layers[i], link) {
			if (it == l) {
				reached = true;
				break;
			}
			if (it->being_unmapped || !it->mapped ||
				!it->layer_surface->initialized)
				continue;
			layer_consume_exclusive(it, &box);
		}
	}

	if (!reached)
		return l->mon->m;
	return box;
}

void get_layer_area_bound(LayerSurface *l, struct wlr_box *bound) {
	*bound = layer_bound_area(l);
}
void get_layer_target_geometry(LayerSurface *l, struct wlr_box *target_box) {

	if (!l || !l->layer_surface || !l->layer_surface->surface->mapped)
		return;

	const struct wlr_layer_surface_v1_state *state = &l->layer_surface->current;

	// restriction area
	// waybar is usually greater than 0, meaning it occupies a certain amount of
	// area, so position calculation should also use the entire area as a
	// baseline if it's -1, it may mean exclusive use of all available space if
	// it's 0, it should mean using the available area outside the
	// exclusive_zone
	struct wlr_box bounds = layer_bound_area(l);

	/* Where wlr_scene_layer_surface_v1_configure places it, to the pixel:
	 * an animation that ends elsewhere jumps. Centred surfaces round as
	 * wlroots does, and take no margins. */
	struct wlr_box box = {.width = state->desired_width,
						  .height = state->desired_height};

	if (box.width == 0) {
		box.x = bounds.x + state->margin.left;
		box.width = bounds.width - (state->margin.left + state->margin.right);
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT &&
			   state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		box.x = bounds.x + bounds.width / 2 - box.width / 2;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
		box.x = bounds.x + state->margin.left;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		box.x = bounds.x + bounds.width - box.width - state->margin.right;
	} else {
		box.x = bounds.x + bounds.width / 2 - box.width / 2;
	}

	if (box.height == 0) {
		box.y = bounds.y + state->margin.top;
		box.height = bounds.height - (state->margin.top + state->margin.bottom);
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP &&
			   state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		box.y = bounds.y + bounds.height / 2 - box.height / 2;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
		box.y = bounds.y + state->margin.top;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		box.y = bounds.y + bounds.height - box.height - state->margin.bottom;
	} else {
		box.y = bounds.y + bounds.height / 2 - box.height / 2;
	}

	target_box->x = box.x;
	target_box->y = box.y;
	target_box->width = box.width;
	target_box->height = box.height;
}
void set_layer_dir_animaiton(LayerSurface *l, struct wlr_box *geo) {
	int32_t slide_direction;
	int32_t horizontal, horizontal_value;
	int32_t vertical, vertical_value;
	int32_t center_x, center_y;

	if (!l)
		return;

	struct wlr_box usable_area;
	get_layer_area_bound(l, &usable_area);

	geo->width = l->geom.width;
	geo->height = l->geom.height;

	center_x = l->geom.x + l->geom.width / 2;
	center_y = l->geom.y + l->geom.height / 2;
	horizontal =
		center_x > usable_area.x + usable_area.width / 2 ? RIGHT : LEFT;
	horizontal_value = horizontal == LEFT
						   ? center_x - usable_area.x
						   : usable_area.x + usable_area.width - center_x;
	vertical = center_y > usable_area.y + usable_area.height / 2 ? DOWN : UP;
	vertical_value = vertical == UP
						 ? center_y - l->mon->w.y
						 : usable_area.y + usable_area.height - center_y;
	slide_direction = horizontal_value < vertical_value ? horizontal : vertical;

	switch (slide_direction) {
	case UP:
		geo->x = l->geom.x;
		geo->y = l->mon->m.y - l->geom.height;
		break;
	case DOWN:
		geo->x = l->geom.x;
		geo->y = l->mon->m.y + l->mon->m.height;
		break;
	case LEFT:
		geo->x = l->mon->m.x - l->geom.width;
		geo->y = l->geom.y;
		break;
	case RIGHT:
		geo->x = l->mon->m.x + l->mon->m.width;
		geo->y = l->geom.y;
		break;
	default:
		geo->x = l->geom.x;
		geo->y = 0 - l->geom.height;
	}
}
void layer_draw_shield(LayerSurface *l) {
	int32_t width, height;

	if (!l->mapped)
		return;

	if (server.active_capture_count > 0 && l->shield_when_capture) {

		layer_actual_size(l, &width, &height);

		if (width <= 0 || height <= 0) {
			wlr_scene_node_set_enabled(&l->shield->node, false);
			return;
		}

		int32_t x, y;
		wlr_scene_node_coords(&l->scene->node, &x, &y);
		const struct wlr_box *box = l->animation.running || l->expand_opening
										? &l->animation.current
										: &l->geom;
		wlr_scene_node_raise_to_top(&l->shield->node);
		wlr_scene_node_set_position(&l->shield->node, box->x - x, box->y - y);
		wlr_scene_rect_set_size(l->shield, width, height);
		wlr_scene_node_set_enabled(&l->shield->node, true);
	} else {
		if (l->shield->node.enabled) {
			wlr_scene_node_lower_to_bottom(&l->shield->node);
			wlr_scene_node_set_position(&l->shield->node, 0, 0);
			wlr_scene_node_set_enabled(&l->shield->node, false);
		}
	}
}
void layer_draw_shadow(LayerSurface *l) {
	if (!l->mapped || !l->shadow)
		return;

	if (!config.shadows || !config.layer_shadows || l->no_shadow) {
		wlr_scene_shadow_set_size(l->shadow, 0, 0);
		return;
	}

	int32_t width, height;
	layer_actual_size(l, &width, &height);

	int32_t delta = config.shadows_size;

	struct wlr_box layer_box = {
		.x = 0,
		.y = 0,
		.width = width,
		.height = height,
	};

	struct wlr_box shadow_box = {
		.x = config.shadows_position_x,
		.y = config.shadows_position_y,
		.width = width + 2 * delta,
		.height = height + 2 * delta,
	};

	struct wlr_box intersection_box;
	wlr_box_intersection(&intersection_box, &layer_box, &shadow_box);
	intersection_box.x -= config.shadows_position_x;
	intersection_box.y -= config.shadows_position_y;

	struct clipped_region clipped_region = {
		.area = intersection_box,
		.corners = corner_radii_all(config.border_radius),
	};

	wlr_scene_node_set_position(&l->shadow->node, shadow_box.x, shadow_box.y);

	wlr_scene_shadow_set_size(l->shadow, shadow_box.width, shadow_box.height);
	wlr_scene_shadow_set_clipped_region(l->shadow, clipped_region);
}
void layer_scene_buffer_apply_effect(struct wlr_scene_buffer *buffer,
									 int32_t sx, int32_t sy, void *data) {
	BufferData *buffer_data = (BufferData *)data;

	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);

	if (scene_surface == NULL)
		return;

	struct wlr_surface *surface = scene_surface->surface;

	int32_t surface_width = surface->current.width * buffer_data->width_scale;
	int32_t surface_height =
		surface->current.height * buffer_data->height_scale;

	if (surface_height > 0 && surface_width > 0) {
		wlr_scene_buffer_set_dest_size(buffer, surface_width, surface_height);
	}
}
void fadeout_layer_animation_next_tick(LayerSurface *l) {
	if (!l)
		return;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int32_t passed_time = timespec_to_ms(&now) - l->animation.time_started;
	double animation_passed =
		l->animation.duration
			? (double)passed_time / (double)l->animation.duration
			: 1.0;

	int32_t type = l->animation.action;
	double factor = find_animation_curve_at(animation_passed, type);

	if (layer_close_animation_type(l) == ANIM_TYPE_EXPAND) {
		double progress = l->expand_progress * (1.0 - factor);
		struct wlr_box box = expand_box_at(
			&l->geom, l->expand_origin_x, l->expand_origin_y, progress);
		struct wlr_box visible;
		if (wlr_box_intersection(&visible, &box, &l->mon->m)) {
			scene_snapshot_clip_to_box(l->scene, &visible, false, 0, 0);
			struct layer_expand_content content = {
				.opacity = expand_content_opacity(progress),
				.radius = layer_expand_radius(l, &box),
			};
			wlr_scene_node_for_each_buffer(&l->scene->node,
										   layer_expand_content, &content);
		}
		layer_expand_glass(l, &box, progress);
		l->animation.current = box;
		goto finish;
	}
	int32_t width = l->animation.initial.width +
					(l->current.width - l->animation.initial.width) * factor;
	int32_t height = l->animation.initial.height +
					 (l->current.height - l->animation.initial.height) * factor;

	int32_t x = l->animation.initial.x +
				(l->current.x - l->animation.initial.x) * factor;
	int32_t y = l->animation.initial.y +
				(l->current.y - l->animation.initial.y) * factor;

	wlr_scene_node_set_position(&l->scene->node, x, y);

	if (l->mon) {
		bool scaling = layer_close_animation_type(l) == ANIM_TYPE_ZOOM;
		scene_snapshot_clip_to_box(l->scene, &l->mon->m, scaling, width,
								   height);
	}

	l->animation.current = (struct wlr_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};

	double opacity_eased_progress =
		find_animation_curve_at(animation_passed, OPAFADEOUT);

	double percent = l->close_opacity * (1.0 - opacity_eased_progress);

	double opacity = MANGO_MAX(percent, 0.0f);

	if (config.animation_fade_out)
		wlr_scene_node_for_each_buffer(&l->scene->node,
									   scene_buffer_apply_opacity, &opacity);

	if (l->blur) {
		wlr_scene_blur_set_strength(l->blur,
								   l->close_strength * (1.0 - factor));
		wlr_scene_blur_set_alpha(l->blur, l->close_alpha * (1.0 - factor));
	}

finish:
	if (animation_passed >= 1.0) {
		wl_list_remove(&l->fadeout_link);
		wlr_scene_node_destroy(&l->scene->node);
		free(l);
		l = NULL;
	}
}

static struct wlr_scene_tree *layer_surface_tree(LayerSurface *l) {
	if (!l || !l->scene) {
		return NULL;
	}

	struct wlr_scene_node *node;
	wl_list_for_each(node, &l->scene->children, link) {
		if (node->type == WLR_SCENE_NODE_TREE) {
			return wlr_scene_tree_from_node(node);
		}
	}
	return NULL;
}

/*
 * Applies the same monitor-boundary clipping used by regular client
 * animations to a live layer surface. Returns true when the surface has been
 * partially or fully clipped, in which case zoom scaling must not overwrite
 * the clipped buffer state.
 */
static bool layer_apply_output_clip(LayerSurface *l,
									const struct wlr_box *animated) {
	struct wlr_scene_tree *surface_tree = layer_surface_tree(l);
	if (!surface_tree || !l->mon) {
		return false;
	}

	struct wlr_box visible;
	if (!wlr_box_intersection(&visible, animated, &l->mon->m)) {
		wlr_scene_subsurface_tree_set_clip(&surface_tree->node, NULL);
		wlr_scene_node_set_enabled(&surface_tree->node, false);
		return true;
	}

	wlr_scene_node_set_enabled(&surface_tree->node, true);
	if (wlr_box_equal(&visible, animated)) {
		wlr_scene_subsurface_tree_set_clip(&surface_tree->node, NULL);
		return false;
	}

	int32_t surface_x, surface_y;
	wlr_scene_node_coords(&surface_tree->node, &surface_x, &surface_y);
	struct wlr_box clip = {
		.x = visible.x - surface_x,
		.y = visible.y - surface_y,
		.width = visible.width,
		.height = visible.height,
	};
	wlr_scene_subsurface_tree_set_clip(&surface_tree->node, &clip);
	return true;
}

void layer_reapply_content(LayerSurface *l) {
	if (!l || !l->layer_surface->surface->mapped)
		return;
	const struct wlr_box *box = &l->animation.current;
	if (l->expand_opening) {
		struct wlr_scene_tree *surface_tree = layer_surface_tree(l);
		if (!surface_tree || !l->mon)
			return;
		struct wlr_box visible;
		bool visible_content = wlr_box_intersection(&visible, box, &l->mon->m);
		wlr_scene_node_set_enabled(&surface_tree->node, visible_content);
		if (visible_content) {
			int32_t x, y;
			wlr_scene_node_coords(&surface_tree->node, &x, &y);
			struct wlr_box clip = {
				.x = visible.x - x,
				.y = visible.y - y,
				.width = visible.width,
				.height = visible.height,
			};
			wlr_scene_subsurface_tree_set_clip(&surface_tree->node, &clip);
		}
		struct layer_expand_content content = {
			.opacity = expand_content_opacity(l->expand_progress),
			.radius = layer_expand_radius(l, box),
		};
		wlr_scene_node_for_each_buffer(&surface_tree->node,
									   layer_expand_content, &content);
		return;
	}

	bool clipped = layer_apply_output_clip(l, box);
	if (layer_open_animation_type(l) == ANIM_TYPE_ZOOM && !clipped) {
		BufferData buffer_data = {
			.width_scale = l->current.width
							   ? (float)box->width / l->current.width : 1.0f,
			.height_scale = l->current.height
								? (float)box->height / l->current.height : 1.0f,
		};
		wlr_scene_node_for_each_buffer(
			&l->scene->node, layer_scene_buffer_apply_effect, &buffer_data);
	}
	if (config.animation_fade_in) {
		double opacity = MANGO_MIN(
			config.fadein_begin_opacity +
				find_animation_curve_at(l->animation_progress, OPAFADEIN) *
					(1.0 - config.fadein_begin_opacity),
			1.0);
		wlr_scene_node_for_each_buffer(&l->scene->node,
									   scene_buffer_apply_opacity, &opacity);
	}
}

static void layer_expand_frame(LayerSurface *l, const struct wlr_box *geometry,
							   double progress) {
	layer_expand_origin(l, geometry);
	struct wlr_box box = expand_box_at(geometry, l->expand_origin_x,
									   l->expand_origin_y, progress);
	wlr_scene_node_set_position(&l->scene->node, geometry->x, geometry->y);
	l->expand_progress = progress;
	l->expand_geometry = *geometry;
	l->animation.current = box;
	layer_reapply_content(l);
	layer_expand_glass(l, &box, progress);
}

void layer_animation_next_tick(LayerSurface *l) {
	if (!l || !l->mapped)
		return;

	if (l->animation.action == OPEN && l->blur)
		wlr_scene_blur_set_glass_lens(l->blur, NULL, 0, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int32_t passed_time = timespec_to_ms(&now) - l->animation.time_started;
	double animation_passed =
		l->animation.duration
			? (double)passed_time / (double)l->animation.duration
			: 1.0;

	int32_t type = l->animation.action == NONE ? MOVE : l->animation.action;
	double factor = find_animation_curve_at(animation_passed, type);
	l->animation_progress = MANGO_MIN(animation_passed, 1.0);

	if (l->expand_opening) {
		struct wlr_box geometry = l->geom;
		if (l->animation.action == MOVE && l->animation.running) {
			geometry = (struct wlr_box){
				.x = l->animation.initial.x +
					 (l->current.x - l->animation.initial.x) * factor,
				.y = l->animation.initial.y +
					 (l->current.y - l->animation.initial.y) * factor,
				.width =
					l->animation.initial.width +
					(l->current.width - l->animation.initial.width) * factor,
				.height =
					l->animation.initial.height +
					(l->current.height - l->animation.initial.height) * factor,
			};
		}
		double open_passed =
			l->expand_duration
				? (double)(timespec_to_ms(&now) - l->expand_time_started) /
					  l->expand_duration
				: 1.0;
		double progress = find_animation_curve_at(open_passed, OPEN);
		layer_expand_frame(l, &geometry, progress);
		if (animation_passed >= 1.0)
			l->animation.running = false;
		if (open_passed >= 1.0) {
			l->expand_opening = false;
			l->animation.action = MOVE;
			glass_lens_apply(l->layer_surface->surface);
		}
		l->need_output_flush = l->animation.running || l->expand_opening;
		return;
	}

	int32_t width = l->animation.initial.width +
					(l->current.width - l->animation.initial.width) * factor;
	int32_t height = l->animation.initial.height +
					 (l->current.height - l->animation.initial.height) * factor;

	int32_t x = l->animation.initial.x +
				(l->current.x - l->animation.initial.x) * factor;
	int32_t y = l->animation.initial.y +
				(l->current.y - l->animation.initial.y) * factor;

	double opacity_eased_progress =
		find_animation_curve_at(animation_passed, OPAFADEIN);

	double opacity = MANGO_MIN(config.fadein_begin_opacity +
								   opacity_eased_progress *
									   (1.0 - config.fadein_begin_opacity),
							   1.0f);

	if (config.animation_fade_in) {
		// Glass fades with its layer even over the wallpaper-only blur
		if (config.blur && l->blur &&
			(l->glass || (!l->no_blur && !layer_blur_uses_wallpaper(l)))) {
			wlr_scene_blur_set_strength(l->blur, opacity);
			wlr_scene_blur_set_alpha(l->blur, opacity);
		}
	}

	wlr_scene_node_set_position(&l->scene->node, x, y);

	struct wlr_box animated = {
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
	l->animation.current = animated;
	layer_reapply_content(l);

	if (config.blur && l->blur &&
		(l->glass || (config.blur_layer && !l->no_blur))) {
		wlr_scene_blur_set_geometry(l->blur, 0, 0, l->animation.current.width,
									l->animation.current.height,
									l->blur->corners);
	}

	if (animation_passed >= 1.0) {
		l->animation.running = false;
		l->need_output_flush = false;
		l->animation.action = MOVE;
		glass_lens_apply(l->layer_surface->surface);
	}
}
void init_fadeout_layers(LayerSurface *l) {
	if (!config.animations || !config.layer_animations || l->no_animation) {
		return;
	}

	if (!l->mon || l->mon->iscleanuping || !l->scene)
		return;

	if (l->shield_when_capture) {
		return;
	}

	if (layer_close_animation_type(l) == ANIM_TYPE_NONE) {
		return;
	}

	if (l->layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM ||
		l->layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND)
		return;

	LayerSurface *fadeout_layer = ecalloc(1, sizeof(*fadeout_layer));

	struct wlr_box usable_area;
	get_layer_area_bound(l, &usable_area);

	wlr_scene_node_set_enabled(&l->scene->node, true);
	fadeout_layer->scene =
		wlr_scene_tree_snapshot(&l->scene->node, server.layers[LyrFadeOut]);
	wlr_scene_node_set_enabled(&l->scene->node, false);

	if (!fadeout_layer->scene) {
		free(fadeout_layer);
		return;
	}

	if (config.blur && l->glass && l->blur) {
		fadeout_layer->blur =
			scene_snapshot_add_glass(fadeout_layer->scene, l->blur);
		fadeout_layer->close_strength = l->blur->strength;
		fadeout_layer->close_alpha = l->blur->alpha;
	}
	fadeout_layer->close_opacity = config.fadeout_begin_opacity;
	fadeout_layer->glass = l->glass;
	fadeout_layer->glass_radius = l->glass_radius;
	bool opening_expand = l->expand_opening;
	struct wlr_box close_geometry =
		opening_expand ? l->expand_geometry : l->animation.current;
	layer_expand_origin(l, &close_geometry);
	fadeout_layer->expand_origin_x = l->expand_origin_x;
	fadeout_layer->expand_origin_y = l->expand_origin_y;
	fadeout_layer->expand_progress = opening_expand ? l->expand_progress : 1.0;
	if (opening_expand)
		fadeout_layer->close_opacity =
			expand_content_opacity(l->expand_progress);

	fadeout_layer->animation.duration = config.animation_duration_close;
	fadeout_layer->geom = fadeout_layer->current =
		fadeout_layer->animainit_geom = fadeout_layer->animation.initial =
			l->animation.current;
	fadeout_layer->mon = l->mon;
	fadeout_layer->animation.action = CLOSE;
	fadeout_layer->animation_type_close = l->animation_type_close;
	fadeout_layer->animation_type_open = l->animation_type_open;

	if (layer_close_animation_type(l) == ANIM_TYPE_EXPAND) {
		fadeout_layer->geom = close_geometry;
		/* Snapshot buffers keep their layout positions: only their clip
		 * shrinks, so even an interrupted open never scales the text. */
		goto start;
	}

	// Here the coordinate setting for snap nodes uses relative coordinates;
	// absolute coordinates cannot be used This differs from ordinary nodes

	fadeout_layer->animation.initial.x = 0;
	fadeout_layer->animation.initial.y = 0;

	if (layer_close_animation_type(l) == ANIM_TYPE_ZOOM) {
		// Calculate the absolute coordinates and size to be set
		fadeout_layer->current.width =
			(float)l->animation.current.width * config.zoom_end_ratio;
		fadeout_layer->current.height =
			(float)l->animation.current.height * config.zoom_end_ratio;
		fadeout_layer->current.x = usable_area.x + usable_area.width / 2 -
								   fadeout_layer->current.width / 2;
		fadeout_layer->current.y = usable_area.y + usable_area.height / 2 -
								   fadeout_layer->current.height / 2;
		// calculate the deviation coordinates;
		// size is not needed because subsequent settings do not use its size
		// deviation, but instead directly scale the buffer
		fadeout_layer->current.x =
			fadeout_layer->current.x - l->animation.current.x;
		fadeout_layer->current.y =
			fadeout_layer->current.y - l->animation.current.y;

	} else if (layer_close_animation_type(l) == ANIM_TYPE_SLIDE) {
		// Get the ending absolute coordinates and size of the slide animation
		set_layer_dir_animaiton(l, &fadeout_layer->current);
		// Also calculate the deviation coordinates and size that can be set
		fadeout_layer->current.x = fadeout_layer->current.x - l->geom.x;
		fadeout_layer->current.y = fadeout_layer->current.y - l->geom.y;
		fadeout_layer->current.width =
			fadeout_layer->current.width - l->geom.width;
		fadeout_layer->current.height =
			fadeout_layer->current.height - l->geom.height;
	} else {
		// the coordinates and size of the fade animation do not need to change
		fadeout_layer->current.x = 0;
		fadeout_layer->current.y = 0;
		fadeout_layer->current.width = 0;
		fadeout_layer->current.height = 0;
	}

start:
	// animation start time
	fadeout_layer->animation.time_started = get_now_in_ms();

	/* Snapshot lifetime is independent of output damage or presentation. */
	wlr_scene_node_set_enabled(&fadeout_layer->scene->node, true);
	wl_list_insert(&server.fadeout_layers, &fadeout_layer->fadeout_link);
	animation_schedule_tick();
}
void layer_set_pending_state(LayerSurface *l) {
	if (!l || !l->layer_surface || !l->layer_surface->surface->mapped)
		return;

	struct wlr_box usable_area;
	get_layer_area_bound(l, &usable_area);

	l->pending = l->geom;

	if (l->animation.action == OPEN && !l->animation.running) {

		if (l->animation.duration == 0) {
			l->animainit_geom = l->geom;
		} else if (layer_open_animation_type(l) == ANIM_TYPE_EXPAND) {
			layer_expand_origin(l, &l->geom);
			l->expand_progress = 0.0;
			l->animainit_geom = expand_box_at(
				&l->geom, l->expand_origin_x, l->expand_origin_y, 0.0);
		} else if (layer_open_animation_type(l) == ANIM_TYPE_ZOOM) {
			l->animainit_geom.width = l->geom.width * config.zoom_initial_ratio;
			l->animainit_geom.height =
				l->geom.height * config.zoom_initial_ratio;
			l->animainit_geom.x = usable_area.x + usable_area.width / 2 -
								  l->animainit_geom.width / 2;
			l->animainit_geom.y = usable_area.y + usable_area.height / 2 -
								  l->animainit_geom.height / 2;
		} else if (layer_open_animation_type(l) == ANIM_TYPE_SLIDE) {

			set_layer_dir_animaiton(l, &l->animainit_geom);
		} else {
			l->animainit_geom.x = l->geom.x;
			l->animainit_geom.y = l->geom.y;
			l->animainit_geom.width = l->geom.width;
			l->animainit_geom.height = l->geom.height;
		}
	} else {
		l->animainit_geom =
			l->expand_opening ? l->expand_geometry : l->animation.current;
	}
	if (!config.animations || !config.layer_animations || l->no_animation ||
		l->layer_surface->current.layer ==
			ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
		l->layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
		l->animation.should_animate = false;
	} else {
		l->animation.should_animate = true;
	}

	if (layer_open_animation_type(l) == ANIM_TYPE_NONE &&
		l->animation.action == OPEN) {
		l->animation.should_animate = false;
	}

	// Starts the animation.
	layer_commit(l);
	l->dirty = true;
}
void layer_commit(LayerSurface *l) {
	if (!l || !l->layer_surface || !l->layer_surface->surface->mapped)
		return;

	l->current = l->pending; // set the end position of the animation

	if (l->animation.should_animate) {
		if (!l->animation.running) {
			l->animation.current = l->animainit_geom;
		}

		l->animation.initial = l->animainit_geom;
		l->animation.time_started = get_now_in_ms();
		l->animation_progress = 0.0;
		if (l->animation.action == OPEN &&
			layer_open_animation_type(l) == ANIM_TYPE_EXPAND) {
			l->expand_opening = true;
			l->expand_time_started = l->animation.time_started;
			l->expand_duration = l->animation.duration;
			l->expand_geometry = l->geom;
			layer_expand_frame(l, &l->geom, 0.0);
		}

		// mark the animation start
		l->animation.running = true;
		l->animation.should_animate = false;
		l->need_output_flush = true;
		if (!l->expand_opening) {
			wlr_scene_node_set_position(&l->scene->node,
										l->animation.current.x,
										l->animation.current.y);
			layer_reapply_content(l);
		}
	}
	animation_schedule_layer(l);
}
bool layer_draw_frame(LayerSurface *l) {
	if (!l || !l->mapped)
		return false;

	if (!l->need_output_flush)
		return false;

	if (l->animation.running || l->expand_opening) {
		if (!config.animations || !config.layer_animations || l->no_animation) {
			l->animation.time_started = get_now_in_ms() - l->animation.duration;
			l->expand_time_started = get_now_in_ms() - l->expand_duration;
		}
		layer_animation_next_tick(l);
		layer_draw_shield(l);
		layer_draw_shadow(l);
	} else {
		layer_draw_shield(l);
		layer_draw_shadow(l);
		l->need_output_flush = false;
	}
	return l->animation.running || l->expand_opening;
}
bool layer_draw_fadeout_frame(LayerSurface *l) {
	if (!l)
		return false;

	fadeout_layer_animation_next_tick(l);
	return true;
}

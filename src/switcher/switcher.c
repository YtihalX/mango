#include "mango/switcher/switcher.h"
#include "mango/animation/common.h"
#include "mango/common/scene_node.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/draw/title-message-bar.h"
#include "mango/draw/text-node.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_xdg_shell.h>

static struct switcher_state switcher_state;

/* Closing panels own only snapshots, never clients or the active switcher. */
struct switcher_motion {
	struct wlr_scene_tree *tree, *title;
	struct wlr_scene_blur *glass, *title_glass;
	struct {
		struct wlr_scene_rect *rect;
		struct wlr_box final;
		float color[4];
	} title_rects[2];
	int title_rect_count;
	struct wl_event_source *timer;
	struct wl_listener output_destroy;
	struct wlr_box final, current, title_box, lens;
	struct timespec started;
	double progress, close_start;
	bool closing;
};

static void switcher_motion_apply(struct switcher_motion *motion);

static void switcher_motion_destroy(struct switcher_motion *motion) {
	if (motion->timer)
		wl_event_source_remove(motion->timer);
	wl_list_remove(&motion->output_destroy.link);
	if (motion->title)
		wlr_scene_node_destroy(&motion->title->node);
	if (motion->tree)
		wlr_scene_node_destroy(&motion->tree->node);
	free(motion);
}

static void switcher_motion_output_destroy(struct wl_listener *listener,
										   void *data) {
	struct switcher_motion *motion =
		wl_container_of(listener, motion, output_destroy);
	if (switcher_state.motion == motion) {
		/* The output is disappearing: leave no timer referring to its monitor. */
		switcher_state.motion = NULL;
		switcher_close();
		motion->tree = NULL;
	}
	switcher_motion_destroy(motion);
}

static int switcher_motion_tick(void *data) {
	struct switcher_motion *motion = data;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed = (now.tv_sec - motion->started.tv_sec) * 1000.0 +
					 (now.tv_nsec - motion->started.tv_nsec) / 1000000.0;
	int duration = motion->closing ? config.animation_duration_close
								  : config.animation_duration_open;
	if (!config.animations)
		duration = 0;
	double t = duration > 0 ? MANGO_MIN(elapsed / duration, 1.0) : 1.0;
	double eased = find_animation_curve_at(t, motion->closing ? CLOSE : OPEN);
	motion->progress = motion->closing ? motion->close_start * (1.0 - eased)
									  : eased;
	if (motion->closing && t >= 1.0) {
		switcher_motion_destroy(motion);
		return 0;
	}
	switcher_motion_apply(motion);
	if (t < 1.0)
		wl_event_source_timer_update(motion->timer, 16);
	return 0;
}

static void switcher_title_opacity(struct wlr_scene_tree *tree, double opacity) {
	struct wlr_scene_node *node;
	wl_list_for_each(node, &tree->children, link) {
		if (node->type == WLR_SCENE_NODE_TREE) {
			switcher_title_opacity(wlr_scene_tree_from_node(node), opacity);
		} else if (node->type == WLR_SCENE_NODE_BUFFER) {
			wlr_scene_buffer_set_opacity(wlr_scene_buffer_from_node(node),
										 opacity);
		} else if (node->type == WLR_SCENE_NODE_BLUR) {
			wlr_scene_blur_set_alpha(wlr_scene_blur_from_node(node), opacity);
		} else if (node->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
			float color[4];
			memcpy(color, rect->color, sizeof(color));
			/* Title rects use these two colours; do not compound their alpha. */
			color[3] = opacity *
				(rect == switcher_state.mon->title_message_bar->border
					 ? config.title_message_bardata.border_color[3]
					 : config.title_message_bardata.focus_bg_color[3]);
			wlr_scene_rect_set_color(rect, color);
		}
	}
}

static void switcher_snapshot_opacity(struct wlr_scene_buffer *buffer,
									  int sx, int sy, void *data) {
	double *opacity = data;
	wlr_scene_buffer_set_opacity(buffer, *opacity);
}

static void switcher_title_snapshot_buffer(struct wlr_scene_buffer *buffer,
										   int sx, int sy, void *data) {
	/* SceneFX releases uploaded CPU buffers; the decoration still owns its
	 * text buffer, unlike the generic snapshot's now-empty scene buffer. */
	struct wlr_buffer *source = data;
	wlr_scene_buffer_set_buffer(buffer, source);
}

static void switcher_motion_apply(struct switcher_motion *motion) {
	double p = MANGO_MAX(0.0, MANGO_MIN(motion->progress, 1.0));
	double opacity = expand_content_opacity(p);
	motion->current = expand_box_at(&motion->final,
		motion->final.x + motion->final.width / 2.0,
		motion->final.y + motion->final.height / 2.0, p);
	struct wlr_box *box = &motion->current;
	int radius = MANGO_MIN(config.switcher_glass_radius,
						   MANGO_MIN(box->width, box->height) / 2);
	wlr_scene_node_set_position(&motion->glass->node,
		box->x - (motion->closing ? 0 : motion->final.x),
		box->y - (motion->closing ? 0 : motion->final.y));
	wlr_scene_blur_set_size(motion->glass, box->width, box->height);
	wlr_scene_blur_set_corner_radius(motion->glass, radius);
	wlr_scene_blur_set_strength(motion->glass, p);
	wlr_scene_blur_set_alpha(motion->glass, p);
	if (motion->closing) {
		struct wlr_box lens = motion->lens;
		lens.x -= box->x;
		lens.y -= box->y;
		struct wlr_box clip = {0, 0, box->width, box->height};
		wlr_scene_blur_set_glass_lens(motion->glass, &lens,
			config.border_radius + SW_PAD, &clip);
		scene_snapshot_clip_to_box(motion->tree, box, false, 0, 0);
		wlr_scene_node_for_each_buffer(&motion->tree->node,
			switcher_snapshot_opacity, &opacity);
		if (motion->title) {
			struct wlr_box title = expand_box_at(&motion->title_box,
				motion->final.x + motion->final.width / 2.0,
				motion->final.y + motion->final.height / 2.0, p);
			scene_snapshot_clip_to_box(motion->title, &title, false, 0, 0);
			wlr_scene_node_for_each_buffer(&motion->title->node,
				switcher_snapshot_opacity, &opacity);
			if (motion->title_glass) {
				wlr_scene_node_set_position(&motion->title_glass->node,
											title.x, title.y);
				wlr_scene_blur_set_size(motion->title_glass,
										title.width, title.height);
				wlr_scene_blur_set_corner_radius(motion->title_glass,
					MANGO_MIN(config.title_message_bardata.corner_radius,
							  MANGO_MIN(title.width, title.height) / 2));
				wlr_scene_blur_set_alpha(motion->title_glass, opacity);
			}
			for (int i = 0; i < motion->title_rect_count; i++) {
				struct wlr_box rect = expand_box_at(&motion->title_rects[i].final,
					motion->final.x + motion->final.width / 2.0,
					motion->final.y + motion->final.height / 2.0, p);
				struct wlr_scene_rect *node = motion->title_rects[i].rect;
				wlr_scene_node_set_position(&node->node, rect.x, rect.y);
				wlr_scene_rect_set_size(node, rect.width, rect.height);
				float color[4];
				memcpy(color, motion->title_rects[i].color, sizeof(color));
				color[3] *= opacity;
				wlr_scene_rect_set_color(node, color);
			}
		}
		return;
	}
	for (int i = 0; i < switcher_state.count; i++)
		switcher_tile_layout(switcher_state.tiles[i]);
	if (switcher_state.mon->title_message_bar)
		switcher_title_opacity(switcher_state.mon->title_message_bar->scene,
							   opacity);
	switcher_apply_highlight();
}

// map the surface tree into the fixed tile box, re-run after each commit
// because the content size or clip may change
void switcher_tile_layout(struct switcher_tile *tile) {
	struct wlr_box clip;
	float src_w, src_h;
	client_get_clip(tile->c, &clip);
	switcher_content_size(tile->c, &src_w, &src_h);
	if (src_w <= 0 || src_h <= 0) {
		struct switcher_surface *entry;
		wl_list_for_each(entry, &tile->surfaces, link)
			wlr_scene_node_set_enabled(&entry->buffer->node, false);
		return;
	}

	double scale_x = (double)tile->cw / src_w;
	double scale_y = (double)switcher_state.tile_h / src_h;
	struct wlr_box visible = {.width = tile->cw, .height = switcher_state.tile_h};
	if (switcher_state.motion) {
		int x, y;
		wlr_scene_node_coords(&tile->content->node, &x, &y);
		struct wlr_box crop = switcher_state.motion->current;
		crop.x -= x;
		crop.y -= y;
		if (!wlr_box_intersection(&visible, &visible, &crop))
			visible = (struct wlr_box){0};
	}
	struct switcher_surface *entry;
	wl_list_for_each(entry, &tile->surfaces, link) {
		struct wlr_surface *s = entry->surface;
		int px = (int)((entry->sx - clip.x) * scale_x);
		int py = (int)((entry->sy - clip.y) * scale_y);
		int dw = (int)(s->current.width * scale_x);
		int dh = (int)(s->current.height * scale_y);

		int cx0 = MANGO_MAX(px, visible.x);
		int cy0 = MANGO_MAX(py, visible.y);
		int cx1 = MANGO_MIN(px + dw, visible.x + visible.width);
		int cy1 = MANGO_MIN(py + dh, visible.y + visible.height);
		int cw = cx1 - cx0;
		int ch = cy1 - cy0;
		if (cw <= 0 || ch <= 0) {
			wlr_scene_node_set_enabled(&entry->buffer->node, false);
			continue;
		}

		/* The tile crop is in surface-local logical coordinates. */
		double ox = (double)(cx0 - px) / scale_x;
		double oy = (double)(cy0 - py) / scale_y;
		struct wlr_fbox surface_box = {
			.x = ox,
			.y = oy,
			.width = (double)cw / scale_x,
			.height = (double)ch / scale_y,
		};
		struct wlr_fbox src;
		client_surface_get_source_box(s, &surface_box, &src);
		wlr_scene_node_set_enabled(&entry->buffer->node, true);
		wlr_scene_buffer_set_source_box(entry->buffer, &src);
		wlr_scene_node_set_position(&entry->buffer->node, cx0, cy0);
		wlr_scene_buffer_set_dest_size(entry->buffer, cw, ch);
		if (switcher_state.motion) {
			const struct wlr_alpha_modifier_surface_v1_state *alpha =
				wlr_alpha_modifier_v1_get_surface_state(s);
			wlr_scene_buffer_set_opacity(entry->buffer,
				expand_content_opacity(switcher_state.motion->progress) *
				(alpha ? alpha->multiplier : 1.0));
			wlr_scene_buffer_set_corner_radius(entry->buffer,
				MANGO_MIN((int)config.border_radius, MANGO_MIN(cw, ch) / 2));
		}
	}
}

void switcher_surface_finish(struct switcher_surface *entry) {
	wl_list_remove(&entry->commit.link);
	wl_list_remove(&entry->destroy.link);
	wl_list_remove(&entry->output_sample.link);
	wl_list_remove(&entry->frame_done.link);
	wl_list_remove(&entry->link);
	free(entry);
}

void switcher_surface_update_buffer(struct switcher_surface *entry) {
	struct wlr_surface *surface = entry->surface;
	struct wlr_scene_buffer *buffer = entry->buffer;

	struct wlr_fbox source;
	wlr_surface_get_buffer_source_box(surface, &source);
	wlr_scene_buffer_set_source_box(buffer, &source);
	wlr_scene_buffer_set_transform(buffer, surface->current.transform);

	const struct wlr_alpha_modifier_surface_v1_state *alpha =
		wlr_alpha_modifier_v1_get_surface_state(surface);
	wlr_scene_buffer_set_opacity(buffer,
								 alpha ? (float)alpha->multiplier : 1.0f);

	enum wlr_color_transfer_function transfer_function =
		WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
	enum wlr_color_named_primaries primaries = WLR_COLOR_NAMED_PRIMARIES_SRGB;
	const struct wlr_image_description_v1_data *image =
		wlr_surface_get_image_description_v1_data(surface);
	if (image) {
		transfer_function =
			wlr_color_manager_v1_transfer_function_to_wlr(image->tf_named);
		primaries =
			wlr_color_manager_v1_primaries_to_wlr(image->primaries_named);
	}
	wlr_scene_buffer_set_transfer_function(buffer, transfer_function);
	wlr_scene_buffer_set_primaries(buffer, primaries);

	enum wlr_color_encoding encoding = WLR_COLOR_ENCODING_NONE;
	enum wlr_color_range range = WLR_COLOR_RANGE_NONE;
	const struct wlr_color_representation_v1_surface_state *representation =
		wlr_color_representation_v1_get_surface_state(surface);
	if (representation) {
		if (representation->coefficients)
			encoding = wlr_color_representation_v1_color_encoding_to_wlr(
				representation->coefficients);
		if (representation->range)
			range = wlr_color_representation_v1_color_range_to_wlr(
				representation->range);
	}
	wlr_scene_buffer_set_color_encoding(buffer, encoding);
	wlr_scene_buffer_set_color_range(buffer, range);

	if (!surface->buffer || surface->current.width <= 0 ||
		surface->current.height <= 0) {
		wlr_scene_buffer_set_buffer(buffer, NULL);
		return;
	}

	struct wlr_scene_buffer_set_buffer_options options = {
		.damage = &surface->buffer_damage,
	};
	struct wlr_linux_drm_syncobj_surface_v1_state *syncobj =
		wlr_linux_drm_syncobj_v1_get_surface_state(surface);
	if (syncobj) {
		options.wait_timeline = syncobj->acquire_timeline;
		options.wait_point = syncobj->acquire_point;
	}
	wlr_scene_buffer_set_buffer_with_options(buffer, &surface->buffer->base,
											 &options);
}

void handle_switcher_surface_commit(struct wl_listener *listener, void *data) {
	struct switcher_surface *entry = wl_container_of(listener, entry, commit);
	switcher_surface_update_buffer(entry);
	switcher_tile_layout(entry->tile);
}

void handle_switcher_surface_destroy(struct wl_listener *listener, void *data) {
	struct switcher_surface *entry = wl_container_of(listener, entry, destroy);
	struct wlr_scene_buffer *buffer = entry->buffer;
	switcher_surface_finish(entry);
	wlr_scene_node_destroy(&buffer->node);
}

void handle_switcher_surface_output_sample(struct wl_listener *listener,
										   void *data) {
	struct switcher_surface *entry =
		wl_container_of(listener, entry, output_sample);
	const struct wlr_scene_output_sample_event *event = data;
	if (entry->buffer->primary_output != event->output)
		return;

	struct wlr_output *output = event->output->output;
	if (event->direct_scanout)
		wlr_presentation_surface_scanned_out_on_output(entry->surface, output);
	else
		wlr_presentation_surface_textured_on_output(entry->surface, output);

	struct wlr_linux_drm_syncobj_surface_v1_state *syncobj =
		wlr_linux_drm_syncobj_v1_get_surface_state(entry->surface);
	if (syncobj && event->release_timeline)
		wlr_linux_drm_syncobj_v1_state_add_release_point(
			syncobj, event->release_timeline, event->release_point,
			output->event_loop);
}

// hidden clients are paced by the preview buffer
void handle_switcher_surface_frame_done(struct wl_listener *listener,
										void *data) {
	struct switcher_surface *entry =
		wl_container_of(listener, entry, frame_done);
	struct wlr_scene_frame_done_event *event = data;
	if (entry->buffer->primary_output == event->output)
		wlr_surface_send_frame_done(entry->surface, &event->when);
}

void switcher_tile_add_surface(struct wlr_surface *surface, int sx, int sy,
							   void *data) {
	struct switcher_tile *tile = data;

	// only the real scene surface may update wl_surface output membership
	struct wlr_scene_buffer *buffer =
		wlr_scene_buffer_create(tile->content, NULL);
	if (!buffer)
		return;

	struct switcher_surface *entry = ecalloc(1, sizeof(*entry));
	entry->tile = tile;
	entry->surface = surface;
	entry->buffer = buffer;
	entry->sx = sx;
	entry->sy = sy;
	wlr_scene_buffer_set_filter_mode(entry->buffer, WLR_SCALE_FILTER_BILINEAR);
	switcher_surface_update_buffer(entry);

	LISTEN(&surface->events.commit, &entry->commit,
		   handle_switcher_surface_commit);
	LISTEN(&surface->events.destroy, &entry->destroy,
		   handle_switcher_surface_destroy);
	LISTEN(&buffer->events.output_sample, &entry->output_sample,
		   handle_switcher_surface_output_sample);
	LISTEN(&buffer->events.frame_done, &entry->frame_done,
		   handle_switcher_surface_frame_done);
	wl_list_insert(&tile->surfaces, &entry->link);
}

void switcher_tile_create(struct switcher_tile *tile, Client *c) {
	tile->c = c;
	wl_list_init(&tile->surfaces);
	float src_w, src_h;
	switcher_content_size(c, &src_w, &src_h);
	float aspect = src_w > 0 && src_h > 0 ? src_w / src_h : 1.0f;
	aspect = MANGO_MAX(SW_ASPECT_MIN, MANGO_MIN(aspect, SW_ASPECT_MAX));
	tile->cw = MANGO_MAX(1, (int)(switcher_state.tile_h * aspect));

	tile->tree = wlr_scene_tree_create(switcher_state.tree);
	tile->frame = wlr_scene_rect_create(tile->tree, 1, 1, config.bordercolor);
	wlr_scene_node_set_enabled(&tile->frame->node, !switcher_state.motion);
	tile->content = wlr_scene_tree_create(tile->tree);
	wlr_scene_node_set_position(&tile->content->node, SW_PAD, SW_PAD);
	wlr_surface_for_each_surface(client_surface(c), switcher_tile_add_surface,
								 tile);
	switcher_tile_layout(tile);
}

void switcher_layout(void) {
	int frame_h = switcher_state.tile_h + 2 * SW_PAD;
	int maxrow_w = MANGO_MAX(
		1, (int)(switcher_state.mon->m.width * SW_PANEL_FRAC) - 2 * SW_MARGIN);

	int nrows = 1;
	int row_w = 0;
	int content_w = 0;
	for (int i = 0; i < switcher_state.count; i++) {
		struct switcher_tile *tile = switcher_state.tiles[i];
		int fw = tile->cw + 2 * SW_PAD;
		if (row_w > 0 && row_w + SW_GAP + fw > maxrow_w) {
			nrows++;
			row_w = fw;
		} else {
			row_w = row_w == 0 ? fw : row_w + SW_GAP + fw;
		}
		tile->row = nrows - 1;
		content_w = MANGO_MAX(content_w, row_w);
	}

	int *rowsw = ecalloc(nrows, sizeof(*rowsw));
	for (int i = 0; i < switcher_state.count; i++) {
		struct switcher_tile *tile = switcher_state.tiles[i];
		int fw = tile->cw + 2 * SW_PAD;
		rowsw[tile->row] += rowsw[tile->row] == 0 ? fw : SW_GAP + fw;
	}

	int panel_w = content_w + 2 * SW_MARGIN;
	int panel_h = 2 * SW_MARGIN + nrows * frame_h + (nrows - 1) * SW_GAP;
	switcher_state.panel_x =
		switcher_state.mon->m.x + (switcher_state.mon->m.width - panel_w) / 2;
	switcher_state.panel_y =
		switcher_state.mon->m.y + (switcher_state.mon->m.height - panel_h) / 2;
	switcher_state.panel_w = panel_w;
	switcher_state.panel_h = panel_h;
	wlr_scene_node_set_position(&switcher_state.tree->node,
								switcher_state.panel_x, switcher_state.panel_y);
	if (switcher_state.bg)
		wlr_scene_rect_set_size(switcher_state.bg, panel_w, panel_h);
	if (switcher_state.motion) {
		switcher_state.motion->final = (struct wlr_box){
			switcher_state.panel_x, switcher_state.panel_y, panel_w, panel_h};
	}

	int row = -1;
	int x = 0;
	for (int i = 0; i < switcher_state.count; i++) {
		struct switcher_tile *tile = switcher_state.tiles[i];
		if (tile->row != row) {
			row = tile->row;
			x = SW_MARGIN + (content_w - rowsw[row]) / 2;
		}
		wlr_scene_node_set_position(&tile->tree->node, x,
									SW_MARGIN + row * (frame_h + SW_GAP));
		wlr_scene_rect_set_size(tile->frame, tile->cw + 2 * SW_PAD, frame_h);
		x += tile->cw + 2 * SW_PAD + SW_GAP;
	}
	free(rowsw);
	title_message_bar_update(switcher_state.mon);
	if (switcher_state.motion)
		switcher_motion_apply(switcher_state.motion);
}

void switcher_apply_highlight(void) {
	if (switcher_state.motion) {
		struct switcher_motion *motion = switcher_state.motion;
		struct switcher_tile *tile = switcher_state.tiles[switcher_state.index];
		struct wlr_box lens = {
			.x = tile->tree->node.x - motion->glass->node.x,
			.y = tile->tree->node.y - motion->glass->node.y,
			.width = tile->cw + 2 * SW_PAD,
			.height = switcher_state.tile_h + 2 * SW_PAD,
		};
		motion->lens = lens;
		motion->lens.x += motion->current.x;
		motion->lens.y += motion->current.y;
		struct wlr_box clip = {0, 0, motion->glass->width, motion->glass->height};
		wlr_scene_blur_set_glass_lens(motion->glass, &lens,
									 config.border_radius + SW_PAD, &clip);
		return;
	}
	const float *highlight = config.switcher_highlight_color[3] < 0.0f
								 ? config.focuscolor
								 : config.switcher_highlight_color;
	for (int i = 0; i < switcher_state.count; i++)
		wlr_scene_rect_set_color(
			switcher_state.tiles[i]->frame,
			i == switcher_state.index ? highlight : config.bordercolor);
}

void switcher_close(void) {
	if (!switcher_is_active())
		return;
	Monitor *m = switcher_state.mon;
	struct switcher_motion *motion = switcher_state.motion;
	if (motion) {
		/* Freeze content before focus changes underneath the closing glass. */
		struct wlr_scene_tree *snapshot = wlr_scene_tree_snapshot(
			&switcher_state.tree->node, server.layers[LyrSpecialOverlay]);
		wlr_scene_node_reparent(&motion->glass->node, snapshot);
		wlr_scene_node_lower_to_bottom(&motion->glass->node);
		wlr_scene_node_set_position(&motion->glass->node,
			motion->current.x, motion->current.y);
		mango_scene_node_set_ignore_hit(&snapshot->node, true);
		motion->tree = snapshot;
		motion->closing = true;
		motion->close_start = motion->progress;
		if (m->title_message_bar && m->title_message_bar->scene->node.enabled) {
			struct wlr_scene_tree *title = m->title_message_bar->scene;
			motion->title_box = (struct wlr_box){
				title->node.x, title->node.y,
				m->title_message_bar->logical_width,
				m->title_message_bar->logical_height};
			motion->title = wlr_scene_tree_snapshot(
				&title->node, server.layers[LyrSpecialOverlay]);
			if (m->title_message_bar->buffer)
				wlr_scene_node_for_each_buffer(&motion->title->node,
					switcher_title_snapshot_buffer,
					&m->title_message_bar->buffer->base);
			mango_scene_node_set_ignore_hit(&motion->title->node, true);
			struct wlr_scene_node *node;
			wl_list_for_each_reverse(node, &title->children, link) {
				if (!node->enabled)
					continue;
				if (node->type == WLR_SCENE_NODE_BLUR) {
					struct wlr_scene_blur *source = wlr_scene_blur_from_node(node);
					motion->title_glass = wlr_scene_blur_create(
						motion->title, source->width, source->height);
					wlr_scene_blur_set_glass(motion->title_glass, true);
					wlr_scene_blur_set_should_only_blur_bottom_layer(
						motion->title_glass, false);
					wlr_scene_node_set_position(&motion->title_glass->node,
						title->node.x + node->x, title->node.y + node->y);
					wlr_scene_node_lower_to_bottom(&motion->title_glass->node);
				} else if (node->type == WLR_SCENE_NODE_RECT &&
						   motion->title_rect_count < 2) {
					int i = motion->title_rect_count++;
					struct wlr_scene_rect *source = wlr_scene_rect_from_node(node);
					motion->title_rects[i].final = (struct wlr_box){
						title->node.x + node->x, title->node.y + node->y,
						source->width, source->height};
					const float *color = source == m->title_message_bar->border
						? config.title_message_bardata.border_color
						: config.title_message_bardata.focus_bg_color;
					memcpy(motion->title_rects[i].color, color, sizeof(float) * 4);
					struct wlr_scene_rect *rect = wlr_scene_rect_create(
						motion->title, source->width, source->height, source->color);
					motion->title_rects[i].rect = rect;
					wlr_scene_rect_set_corner_radii(rect, source->corners);
					wlr_scene_node_set_position(&rect->node,
						title->node.x + node->x, title->node.y + node->y);
					wlr_scene_node_lower_to_bottom(&rect->node);
				}
			}
		}
		clock_gettime(CLOCK_MONOTONIC, &motion->started);
		wl_event_source_timer_update(motion->timer, 16);
	}
	if (m->title_message_bar)
		switcher_title_opacity(m->title_message_bar->scene, 1.0);
	for (int i = 0; i < switcher_state.count; i++) {
		struct switcher_surface *entry, *tmp;
		wl_list_for_each_safe(entry, tmp, &switcher_state.tiles[i]->surfaces,
							  link) {
			switcher_surface_finish(entry);
		}
	}
	wlr_scene_node_destroy(&switcher_state.tree->node);
	for (int i = 0; i < switcher_state.count; i++)
		free(switcher_state.tiles[i]);
	free(switcher_state.tiles);
	memset(&switcher_state, 0, sizeof(switcher_state));
	title_message_bar_hide(m);
}

// remove a single tile while the switcher stays open, then relayout
void switcher_remove_client(Client *c) {
	if (!switcher_is_active())
		return;
	int i;
	for (i = 0; i < switcher_state.count; i++) {
		if (switcher_state.tiles[i]->c == c)
			break;
	}
	if (i == switcher_state.count)
		return;

	struct switcher_surface *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &switcher_state.tiles[i]->surfaces,
						  link) {
		switcher_surface_finish(entry);
	}
	wlr_scene_node_destroy(&switcher_state.tiles[i]->tree->node);
	free(switcher_state.tiles[i]);

	if (switcher_state.index > i)
		switcher_state.index--;
	memmove(&switcher_state.tiles[i], &switcher_state.tiles[i + 1],
			(switcher_state.count - i - 1) * sizeof(*switcher_state.tiles));
	switcher_state.count--;
	if (switcher_state.count == 0) {
		switcher_close();
		return;
	}
	if (switcher_state.index >= switcher_state.count)
		switcher_state.index = switcher_state.count - 1;
	switcher_layout();
	switcher_apply_highlight();
}

static bool switcher_client_eligible(Client *c) {
	return c && c->mon && !c->iskilling && !c->isminimized && !c->isunglobal &&
		   client_surface(c) && client_surface(c)->mapped &&
		   !client_is_unmanaged(c) && !client_is_x11_popup(c);
}

void switcher_commit_client(Client *tc) {
	switcher_close();
	if (!switcher_client_eligible(tc))
		return;
	if (!VISIBLEON(tc, tc->mon))
		client_view_on_monitor(&(Arg){.ui = get_tags_first_tag(tc->tags)}, true,
							   tc->mon, true);
	client_focus(tc, 1);
}

void switcher_commit(void) {
	if (!switcher_is_active())
		return;
	switcher_commit_client(switcher_state.tiles[switcher_state.index]->c);
}

Client *switcher_client_at(double lx, double ly) {
	if (!switcher_is_active())
		return NULL;
	for (int i = 0; i < switcher_state.count; i++) {
		struct switcher_tile *tile = switcher_state.tiles[i];
		struct wlr_box box = {
			.width = tile->cw + 2 * SW_PAD,
			.height = switcher_state.tile_h + 2 * SW_PAD,
		};
		if (!wlr_scene_node_coords(&tile->tree->node, &box.x, &box.y) ||
			!wlr_box_contains_point(&box, lx, ly))
			continue;
		return tile->c;
	}
	return NULL;
}

bool switcher_title_message_bar_geometry(Monitor *m, Client **client,
										 struct wlr_box *box) {
	if (!switcher_is_active() || !m || m != switcher_state.mon ||
		switcher_state.count <= 0 || switcher_state.index < 0 ||
		switcher_state.index >= switcher_state.count)
		return false;

	if (client)
		*client = switcher_state.tiles[switcher_state.index]->c;
	if (box) {
		*box = (struct wlr_box){
			.x = switcher_state.panel_x,
			.y = switcher_state.panel_y + switcher_state.panel_h +
				 (int)config.title_message_bar_gap,
			.width = switcher_state.panel_w,
			.height = (int)config.title_message_bar_height,
		};
	}
	return true;
}

void switcher_open(int scope, int dir) {
	Client *c;
	Monitor *m;
	int n = 0;

	wl_list_for_each(m, &server.monitors, link) {
		if (m->isoverview)
			return;
	}

	switcher_state.mon = server.selected_monitor;
	switcher_state.scope = scope;

	wl_list_for_each(c, &server.focus_stack, flink) {
		if (switcher_candidate(c))
			n++;
	}
	if (n == 0)
		return;

	int max_row_w =
		(int)(switcher_state.mon->m.width * SW_PANEL_FRAC) - 2 * SW_MARGIN;
	int max_panel_h = (int)(switcher_state.mon->m.height * SW_PANEL_FRAC);
	// size against the widest allowed tile so the panel always fits
	switcher_state.tile_h = 1;
	for (int cols = 1; cols <= n; cols++) {
		int rows = (n + cols - 1) / cols;
		int h_by_w =
			(int)((max_row_w - (cols - 1) * SW_GAP - 2 * cols * SW_PAD) /
				  (cols * SW_ASPECT_MAX));
		int h_by_h =
			(max_panel_h - 2 * SW_MARGIN - (rows - 1) * SW_GAP) / rows -
			2 * SW_PAD;
		int h = MANGO_MIN((int)(switcher_state.mon->m.height * SW_TILE_FRAC),
						  MANGO_MIN(h_by_w, h_by_h));
		switcher_state.tile_h = MANGO_MAX(switcher_state.tile_h, h);
	}

	switcher_state.tree =
		wlr_scene_tree_create(server.layers[LyrSpecialOverlay]);
	if (config.switcher_glass) {
		struct switcher_motion *motion = ecalloc(1, sizeof(*motion));
		switcher_state.motion = motion;
		motion->tree = switcher_state.tree;
		motion->glass = wlr_scene_blur_create(motion->tree, 1, 1);
		wlr_scene_blur_set_glass(motion->glass, true);
		wlr_scene_blur_set_should_only_blur_bottom_layer(motion->glass, false);
		mango_scene_node_set_ignore_hit(&motion->tree->node, true);
		motion->progress = config.animations && config.animation_duration_open > 0
							   ? 0.0 : 1.0;
		clock_gettime(CLOCK_MONOTONIC, &motion->started);
		motion->timer = wl_event_loop_add_timer(server.event_loop,
											   switcher_motion_tick, motion);
		LISTEN(&switcher_state.mon->wlr_output->events.destroy,
			&motion->output_destroy, switcher_motion_output_destroy);
		/* Release monitor references before the monitor's own destroy listener. */
		wl_list_remove(&motion->output_destroy.link);
		wl_list_insert(
			&switcher_state.mon->wlr_output->events.destroy.listener_list,
			&motion->output_destroy.link);
		if (motion->progress < 1.0)
			wl_event_source_timer_update(motion->timer, 16);
	} else {
		switcher_state.bg =
			wlr_scene_rect_create(switcher_state.tree, 1, 1, switcher_panel_color);
	}
	switcher_state.tiles = ecalloc(n, sizeof(*switcher_state.tiles));
	wl_list_for_each(c, &server.focus_stack, flink) {
		if (!switcher_candidate(c))
			continue;
		struct switcher_tile *tile = ecalloc(1, sizeof(*tile));
		switcher_tile_create(tile, c);
		switcher_state.tiles[switcher_state.count++] = tile;
	}
	// The current window is at the focus_stack head (tiles[0]); selecting the
	// second one and committing inserts it at the front, so the next open puts
	// the original first as second, toggling between the two most recent
	// windows. Forward (next) therefore starts at the second tile, while
	// backward (prev) has to start from the tail (the least recent window)
	// and walk in reverse.
	if (switcher_state.count > 1)
		switcher_state.index = dir < 0 ? switcher_state.count - 1 : 1;
	else
		switcher_state.index = 0;
	switcher_layout();
	switcher_apply_highlight();

	// restart render loops of clients idling without frame callbacks so
	// tiles of hidden windows stay live, like overview cards
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	for (int i = 0; i < switcher_state.count; i++)
		client_send_frame_done(switcher_state.tiles[i]->c, &now);
}

void switcher_cycle(int dir) {
	switcher_state.index = (switcher_state.index + dir + switcher_state.count) %
						   switcher_state.count;
	switcher_apply_highlight();
	title_message_bar_update(switcher_state.mon);
	if (switcher_state.motion)
		switcher_motion_apply(switcher_state.motion);
}

int32_t switcher(const Arg *arg) {
	int dir = arg && arg->i == PREV ? -1 : 1;
	int scope = arg ? arg->i2 : SW_CURRENT_TAG;
	if (server.session_locked || !server.selected_monitor ||
		server.selected_monitor->is_jump_mode)
		return 0;
	if (switcher_is_active()) {
		if (scope != switcher_state.scope) {
			switcher_close();
			switcher_open(scope, dir);
		} else {
			switcher_cycle(dir);
		}
	} else {
		switcher_open(scope, dir);
	}
	return 0;
}
bool switcher_is_active(void) { return switcher_state.tree != NULL; }

bool switcher_candidate(Client *c) {
	if (!switcher_client_eligible(c))
		return false;

	if (switcher_state.scope == SW_CURRENT_TAG)
		return c->mon == switcher_state.mon && VISIBLEON(c, switcher_state.mon);
	if (switcher_state.scope == SW_ALL_TAG)
		return c->mon == switcher_state.mon;
	return (int32_t)c->tags > 0;
}

void switcher_content_size(Client *c, float *w, float *h) {
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_surface *s = client_surface(c);
		*w = s->current.width;
		*h = s->current.height;
		return;
	}
#endif
	*w = c->surface.xdg->geometry.width;
	*h = c->surface.xdg->geometry.height;
}

const float switcher_panel_color[4] = {0.09f, 0.09f, 0.11f, 0.92f};

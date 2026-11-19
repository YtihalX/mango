#include "mango/ext-protocol/glass-lens.h"
#include "mango-glass-lens-v1-protocol.h"
#include "mango/manage/layer.h"
#include <scenefx/types/wlr_scene.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/addon.h>

/* A lens's double-buffered state, kept with the surface's own state, so that
 * it applies with the content committed together with it. */
struct glass_lens_state {
	struct wlr_box box, clip;
	int32_t radius;
};

/* A surface's lens. It lives as long as both the surface and the client's
 * lens object; when the surface goes first, the object stays inert. */
struct glass_lens {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_addon addon; // on the surface, which has one lens at most
	struct wlr_surface_synced synced;
	struct glass_lens_state pending, current;
};

/* Shows `state`, or no lens when it is null, in the glass of the lens's layer
 * surface or popup. That glass comes and goes with its surface, so every
 * commit applies the lens again; setting the same lens twice does nothing. A
 * popup's glass starts at the corner of its window geometry, not of its
 * surface, and is made and reshaped after this commit callback: mango shows
 * the lens again then (glass_lens_apply). */
static void show_lens(struct glass_lens *lens,
					  const struct glass_lens_state *state) {
	struct wlr_scene_blur *glass = NULL;
	struct wlr_box offset = {0};
	struct wlr_layer_surface_v1 *layer_surface =
		wlr_layer_surface_v1_try_from_wlr_surface(lens->surface);
	struct wlr_xdg_popup *xdg_popup =
		wlr_xdg_popup_try_from_wlr_surface(lens->surface);
	if (layer_surface) {
		LayerSurface *l = layer_surface->data;
		glass = l ? l->blur : NULL;
		/* The mapping commit can precede the glass node. Keep the synced
		 * state, but do not draw a lens ahead of its opening slab. */
		if (l && l->animation.running && l->animation.action == OPEN)
			state = NULL;
	} else if (xdg_popup) {
		Popup *popup = xdg_popup->base->data;
		glass = popup ? popup->glass : NULL;
		offset = xdg_popup->base->geometry;
	}
	if (!glass)
		return;
	if (!state) {
		wlr_scene_blur_set_glass_lens(glass, NULL, 0, NULL);
		return;
	}
	struct wlr_box box = state->box, clip = state->clip;
	box.x -= offset.x;
	box.y -= offset.y;
	clip.x -= offset.x;
	clip.y -= offset.y;
	wlr_scene_blur_set_glass_lens(glass, &box, state->radius, &clip);
}

static void handle_lens_commit(struct wlr_surface_synced *synced) {
	struct glass_lens *lens = wl_container_of(synced, lens, synced);
	show_lens(lens, &lens->current);
}

static const struct wlr_surface_synced_impl lens_synced_impl = {
	.state_size = sizeof(struct glass_lens_state),
	.commit = handle_lens_commit,
};

static void destroy_lens(struct glass_lens *lens) {
	wlr_addon_finish(&lens->addon);
	wlr_surface_synced_finish(&lens->synced);
	wl_resource_set_user_data(lens->resource, NULL);
	free(lens);
}

/* The surface is being destroyed, the layer with it: leave its glass alone. */
static void handle_surface_gone(struct wlr_addon *addon) {
	struct glass_lens *lens = wl_container_of(addon, lens, addon);
	destroy_lens(lens);
}

static const struct wlr_addon_interface lens_addon_impl = {
	.name = "mango_glass_lens_v1",
	.destroy = handle_surface_gone,
};

void glass_lens_apply(struct wlr_surface *surface) {
	struct wlr_addon *addon =
		wlr_addon_find(&surface->addons, NULL, &lens_addon_impl);
	if (addon) {
		struct glass_lens *lens = wl_container_of(addon, lens, addon);
		show_lens(lens, &lens->current);
	}
}

static void handle_lens_set_box(struct wl_client *client,
								struct wl_resource *resource, int32_t x,
								int32_t y, int32_t width, int32_t height,
								int32_t radius) {
	struct glass_lens *lens = wl_resource_get_user_data(resource);
	if (!lens)
		return;
	lens->pending.box = (struct wlr_box){x, y, width, height};
	lens->pending.radius = radius;
}

static void handle_lens_set_clip(struct wl_client *client,
								 struct wl_resource *resource, int32_t x,
								 int32_t y, int32_t width, int32_t height) {
	struct glass_lens *lens = wl_resource_get_user_data(resource);
	if (!lens)
		return;
	lens->pending.clip = (struct wlr_box){x, y, width, height};
}

static void handle_lens_destroy(struct wl_client *client,
								struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void handle_lens_resource_destroy(struct wl_resource *resource) {
	struct glass_lens *lens = wl_resource_get_user_data(resource);
	if (!lens)
		return;
	show_lens(lens, NULL);
	destroy_lens(lens);
}

static const struct mango_glass_lens_v1_interface lens_impl = {
	.destroy = handle_lens_destroy,
	.set_box = handle_lens_set_box,
	.set_clip = handle_lens_set_clip,
};

static void handle_manager_get_lens(struct wl_client *client,
									struct wl_resource *manager, uint32_t id,
									struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (wlr_addon_find(&surface->addons, NULL, &lens_addon_impl)) {
		wl_resource_post_error(manager,
							   MANGO_GLASS_LENS_MANAGER_V1_ERROR_LENS_EXISTS,
							   "the surface has a lens already");
		return;
	}

	struct glass_lens *lens = calloc(1, sizeof(*lens));
	if (!lens) {
		wl_client_post_no_memory(client);
		return;
	}
	if (!wlr_surface_synced_init(&lens->synced, surface, &lens_synced_impl,
								 &lens->pending, &lens->current)) {
		free(lens);
		wl_client_post_no_memory(client);
		return;
	}
	lens->resource = wl_resource_create(client, &mango_glass_lens_v1_interface,
										wl_resource_get_version(manager), id);
	if (!lens->resource) {
		wlr_surface_synced_finish(&lens->synced);
		free(lens);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(lens->resource, &lens_impl, lens,
								   handle_lens_resource_destroy);
	lens->surface = surface;
	wlr_addon_init(&lens->addon, &surface->addons, NULL, &lens_addon_impl);
}

static void handle_manager_destroy(struct wl_client *client,
								   struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct mango_glass_lens_manager_v1_interface manager_impl = {
	.destroy = handle_manager_destroy,
	.get_lens = handle_manager_get_lens,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version,
						 uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &mango_glass_lens_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void glass_lens_manager_create(struct wl_display *display) {
	wl_global_create(display, &mango_glass_lens_manager_v1_interface, 1, NULL,
					 bind_manager);
}

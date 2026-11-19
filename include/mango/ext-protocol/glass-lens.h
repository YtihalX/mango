#ifndef __EXT_PROTOCOL_GLASS_LENS_H__
#define __EXT_PROTOCOL_GLASS_LENS_H__ 1

#include <wayland-server-core.h>

/*
 * mango-glass-lens-v1: a lens set into the liquid glass of a layer surface or
 * a popup, placed by the client and applied with the surface's commits.
 */
void glass_lens_manager_create(struct wl_display *display);

#endif

---
title: Window Effects
description: Add visual polish with blur, shadows, and opacity.
---

## Blur

Blur creates a frosted glass effect for transparent windows.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `blur` | `0` | Enable blur for windows. |
| `blur_layer` | `0` | Enable blur for layer surfaces (like bars/docks). |
| `im_popup_glass` | `0` | Liquid glass under an input method's popup, such as fcitx5's candidate panel, over its whole surface, when the popup leaves its middle translucent. Its theme draws no shadow. Requires `blur=1`. |
| `im_popup_glass_radius` | `12` | Corner radius of that glass. |
| `blur_optimized` | `1` | Caches the wallpaper and blur background, significantly reducing GPU usage. Disabling it will significantly increase GPU consumption and may cause rendering lag. **Highly recommended.** |
| `blur_params_radius` | `5` | The strength (radius) of the blur. |
| `blur_params_num_passes` | `1` | Number of passes. Higher = smoother but more expensive. |
| `blur_params_noise` | `0.02` | Blur noise level. |
| `blur_params_brightness` | `0.9` | Blur brightness adjustment. |
| `blur_params_contrast` | `0.9` | Blur contrast adjustment. |
| `blur_params_saturation` | `1.2` | Blur saturation adjustment. |

> **Warning:** Blur has a relatively high impact on performance. If your hardware is limited, it is not recommended to enable it. If you experience lag with blur on, ensure `blur_optimized=1` — disabling it will significantly increase GPU consumption and may cause rendering lag. To disable blur entirely, set `blur=0`.

Changing a selection lens damages the old and new clipped lens footprints,
not the surrounding glass slab. The renderer still accounts for nearby pixels
sampled by blur and refraction: as far as each glass's own rim refracts, and
its lens's rim only while it has a lens.

Each output allocates the buffers blur works in, each the size of the output,
on first use: none while nothing is blurred, and the two that cache the
blurred wallpaper only once a blur reads it (any blur with `blur_optimized=1`;
otherwise plain blur on tiled windows and the bottom layer). Brightness,
contrast, saturation and noise are applied in the blur's last pass, not in a
pass of their own.

To see what glass costs, start mango with `MANGO_DEBUG_GLASS=1`: it writes a
line to stderr for each frame's damage and the part of it blur added, and for
each glass draw, the region the draw presents and the region its blur works
over, each as `glass WHAT AREA X1,Y1,X2,Y2;...` in buffer pixels. minshell's
`dev/check-chrome-glass.sh` reads these to keep glass off work content.

---

## Shadows

Drop shadows help distinguish floating windows from the background.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `shadows` | `0` | Enable shadows. |
| `layer_shadows` | `0` | Enable shadows for layer surfaces. |
| `shadow_only_floating` | `1` | Only draw shadows for floating windows (saves performance). |
| `shadows_size` | `10` | Size of the shadow. |
| `shadows_blur` | `15` | Shadow blur amount. |
| `shadows_position_x` | `0` | Shadow X offset. |
| `shadows_position_y` | `0` | Shadow Y offset. |
| `shadows_color` | `0x000000ff` | Color of the shadow. |

```ini
# Example shadows configuration
shadows=1
layer_shadows=1
shadow_only_floating=1
shadows_size=12
shadows_blur=15
shadows_position_x=0
shadows_position_y=0
shadows_color=0x000000ff
```

---

## Opacity & Corner Radius

Control the transparency and roundness of your windows.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `border_radius` | `0` | Window corner radius in pixels. |
| `no_radius_when_single` | `0` | Disable radius if only one window is visible. |
| `focused_opacity` | `1.0` | Opacity for the active window (0.0 - 1.0). |
| `unfocused_opacity` | `1.0` | Opacity for inactive windows (0.0 - 1.0). |

```ini
# Window corner radius in pixels
border_radius=0

# Disable radius if only one window is visible
no_radius_when_single=0

# Opacity for the active window (0.0 - 1.0)
focused_opacity=1.0

# Opacity for inactive windows
unfocused_opacity=1.0
```

---

## Dim Overlay

Draws a translucent layer over the content of a window, so it can be shaded
without changing the opacity of the application itself. The layer never takes
pointer input: clicks and drags always go to the window below it. Its corners
follow `border_radius`, and it is resized together with the window, including
while windows are being animated.

Colors use the usual `0xRRGGBBAA` format, so the last two digits set the
transparency of the overlay.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `dim_enable` | `0` | Enable the dim overlay on windows. |
| `dim_focused_color` | `0x00000000` | Dim color of the focused window. |
| `dim_unfocused_color` | `0x00000055` | Dim color of the unfocused windows. |

With just `dim_enable=1` the focused window stays untouched and the unfocused
windows are shaded with `0x55` (about a third) black.

```ini
dim_enable=1
dim_focused_color=0x00000000
dim_unfocused_color=0x0000004d
```

---
title: Animations
description: Configure smooth transitions for windows and layers.
---

## Enabling Animations

mangowm supports animations for both standard windows and layer shell surfaces (like bars and notifications).

```ini
animations=1
layer_animations=1
```

Windows and layers advance on one animation clock. Each output still presents
independently and only receives animation damage where the old or new scene
content touches it, including when a window moves between outputs.

## Animation Types

You can define different animation styles for opening and closing windows and layer surfaces.

Available types: `slide`, `zoom`, `fade`, `none`.

```ini
animation_type_open=zoom
animation_type_close=slide
layer_animation_type_open=slide
layer_animation_type_close=slide
```

### Expanding layer glass

Layers also accept `expand`, without changing the default animation types:

```ini
layer_rule=animation_type_open:expand,animation_type_close:expand,layer_name:^(my-panel|my-notifications)$
```

`layer_animation_type_open=expand` and `layer_animation_type_close=expand`
select it globally for layers. An individual layer rule can override either.
Window animations do not support `expand`.

The layer's anchors choose its origin: one anchored edge gives its midpoint,
two adjacent edges their corner, and opposing or no edges the centre on that
axis. Glass grows from `zoom_initial_ratio` to its final size using the normal
open duration and curve. Its strength and alpha grow from zero; its radius is
clamped to half the current shorter side. Content stays at its real size,
clipped to the rounded slab, and fades in only after halfway. This staged fade
is intrinsic to `expand`, independent of the optional fades above.

Closing reverses the growth on a snapshot, preserving its glass until the
last frame. Interrupting an open closes from the current size and opacity.
Every other layer close type also retains the layer's glass. Mapped geometry
changes still use the move animation. A protocol lens is hidden during open
and close and restored when the open settles, including a lens sent on the
mapping commit. Animated edges are rounded once in layout coordinates.
While opening, the animation controls the glass box; new layer buffers are
clipped to the current box without resizing the glass.

## Fade Settings

Control the fade-in and fade-out effects for animations.

```ini
animation_fade_in=1
animation_fade_out=1
fade_in_begin_opacity=0.5
fade_out_begin_opacity=0.5
```

- `animation_fade_in` — Enable fade-in effect (0: disable, 1: enable)
- `animation_fade_out` — Enable fade-out effect (0: disable, 1: enable)
- `fade_in_begin_opacity` — Starting opacity for fade-in animations (0.0–1.0)
- `fade_out_begin_opacity` — Starting opacity for fade-out animations (0.0–1.0)

## Zoom Settings

Adjust the zoom ratios for zoom animations.

```ini
zoom_initial_ratio=0.4
zoom_end_ratio=0.8
```

- `zoom_initial_ratio` — Initial zoom ratio
- `zoom_end_ratio` — End zoom ratio

## Durations

Control the speed of animations (in milliseconds).

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `animation_duration_move` | integer | `500` | Move animation duration (ms) |
| `animation_duration_open` | integer | `400` | Open animation duration (ms) |
| `animation_duration_tag` | integer | `300` | Tag animation duration (ms) |
| `animation_duration_close` | integer | `300` | Close animation duration (ms) |
| `animation_duration_focus` | integer | `0` | Focus change (opacity transition) animation duration (ms) |

```ini
animation_duration_move=500
animation_duration_open=400
animation_duration_tag=300
animation_duration_close=300
animation_duration_focus=0
```

## Custom Bezier Curves

Bezier curves determine the "feel" of an animation (e.g., linear vs. bouncy). The format is `x1,y1,x2,y2`.

You can visualize and generate curve values using online tools like [cssportal.com](https://www.cssportal.com/css-cubic-bezier-generator/) or [easings.net](https://easings.net).

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `animation_curve_open` | string | `0.46,1.0,0.29,0.99` | Open animation bezier curve |
| `animation_curve_move` | string | `0.46,1.0,0.29,0.99` | Move animation bezier curve |
| `animation_curve_tag` | string | `0.46,1.0,0.29,0.99` | Tag animation bezier curve |
| `animation_curve_close` | string | `0.46,1.0,0.29,0.99` | Close animation bezier curve |
| `animation_curve_focus` | string | `0.46,1.0,0.29,0.99` | Focus change (opacity transition) animation bezier curve |
| `animation_curve_opacity_fade_in` | string | `0.46,1.0,0.29,0.99` | Open opacity animation bezier curve |
| `animation_curve_opacity_fade_out` | string | `0.5,0.5,0.5,0.5` | Close opacity animation bezier curve |

```ini
animation_curve_open=0.46,1.0,0.29,0.99
animation_curve_move=0.46,1.0,0.29,0.99
animation_curve_tag=0.46,1.0,0.29,0.99
animation_curve_close=0.46,1.0,0.29,0.99
animation_curve_focus=0.46,1.0,0.29,0.99
animation_curve_opacity_fade_in=0.46,1.0,0.29,0.99
animation_curve_opacity_fade_out=0.5,0.5,0.5,0.5
```

## Tag Animation Direction

Control the direction of tag switch animations.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `tag_animation_direction` | `1` | Tag animation direction (1: horizontal, 0: vertical) |
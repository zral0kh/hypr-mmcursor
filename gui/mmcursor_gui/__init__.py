"""mmcursor-gui — a drag-to-arrange view over the mmcursor plugin's physical layout.

Deliberately a *view*, not a second source of truth: every rectangle drawn
here comes from `hyprctl -j mmcursor`, and every drag is previewed by asking
the plugin itself (`hyprctl mmcursor place`) to rebuild through the same
`buildLayout` path it already uses. This package computes no geometry of its
own beyond screen<->mm scaling for drawing and snapping.
"""

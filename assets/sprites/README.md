This folder contains world-space sprite sheets used by the RTS runtime.

The current `projectile_trail.png`, `impact_burst.png`, `fire_loop.png`, and
`smoke_puff.png` sheets are deterministic probe fixtures for Metal/OpenGL
renderer coverage. They are intentionally small and self-contained; final
HD/4K production effects should replace or extend them after Metal parity is
closed.

`projectile_trail.png` and `impact_burst.png` are also wired into the default
Mage projectile descriptor so gameplay probes can exercise the real projectile
trail/impact emission path.

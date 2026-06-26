# lod_generation

Offline level-of-detail tooling for PFOBJ models. Two passes, both driven through Blender;
the only dependency is **Blender (>= 2.80)**.

## Passes

1. **`optimization_pass.py`** — collapses each base mesh (LOD0) as far as a quadric-error
   bound allows (default `--error 0.0006`, the largest surface deviation a collapse may
   introduce as a fraction of the model extent), binary-searching the keep-ratio per mesh down
   to a `0.5` floor. Over-tessellated hard-surface meshes reduce heavily while curved or
   organic ones barely change. Rewrites each `.pfobj` in place and keeps a one-time `.orig`
   backup, the original is always re-read from `.orig` so re-runs don't compound.

2. **`lod_generation_pass.py`** — writes `<name>.lod1.pfobj` (50%) and `<name>.lod2.pfobj`
   (25%) next to each source using COLLAPSE decimation. LOD2 backs off toward LOD1 wherever a
   model would tear, so output is hole-free by default. Pass `--max-holes 100` to force the
   full reduction on geometry where tearing is acceptable (e.g. flat ground detail).

Both walk a directory recursively (or take explicit `.pfobj` paths), skip meshes below
`--min-tris`, and reuse each model's materials, joints, animations and bounds verbatim.

## Run

    blender --background --python tools/lod_generation/optimization_pass.py   -- <models-dir>
    blender --background --python tools/lod_generation/lod_generation_pass.py -- <models-dir>

## Support modules

- **`lodlib.py`** — PFOBJ read/write and the voxel surface-coverage gate (pure Python).
- **`blender_common.py`** — Blender import / decimate / soup-export helpers shared by both passes.

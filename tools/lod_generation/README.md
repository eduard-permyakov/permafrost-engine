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

2. **`lod_generation_pass.py`** — writes `<name>.lod1.pfobj` and `<name>.lod2.pfobj` next to
   each source using COLLAPSE decimation. Each level collapses as far as a surface-error bound
   allows (`--lod1-error 0.003`, `--lod2-error 0.008` of the model extent) but no further than
   `--lod1 0.5` / `--lod2 0.25` of the triangles, so thin parts that cast shadows survive. UV
   seams are pinned and the source's corner normals are carried over to the decimated mesh, so
   textures do not stretch and hard edges keep their shading. Levels with surface holes past
   `--max-holes` are reported.

Both walk a directory recursively (or take explicit `.pfobj` paths), skip meshes below
`--min-tris`, and reuse each model's materials, joints, animations and bounds verbatim.

## Run

    blender --background --python tools/lod_generation/optimization_pass.py   -- <models-dir>
    blender --background --python tools/lod_generation/lod_generation_pass.py -- <models-dir>

## Support modules

- **`lodlib.py`** — PFOBJ read/write and the voxel surface-coverage gate (pure Python).
- **`blender_common.py`** — Blender import / decimate / soup-export helpers shared by both passes.

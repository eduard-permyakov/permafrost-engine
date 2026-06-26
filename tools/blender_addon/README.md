# Permafrost Engine PFOBJ Blender add-on

`io_scene_pfobj` adds **File > Import** and **File > Export** support for Permafrost Engine
`.pfobj` models (meshes, UVs, normals, materials, armatures, vertex weights and animations) to
Blender 2.80+. The offline LOD scripts in `../lod_generation` import it directly, so they do
not need it installed; install it only for interactive use inside Blender.

## Install

Use either method below, then tick **Permafrost Engine Object (.pfobj)** under
*Edit > Preferences > Add-ons* to enable it.

### Via Blender (any OS)

1. Zip the `io_scene_pfobj` folder (the archive must contain the folder itself).
2. Open *Edit > Preferences > Add-ons* and choose **Install...** (called **Install from Disk**
   in Blender 4.2+, in the top-right dropdown), then select the zip.

### Manual

Copy or symlink the `io_scene_pfobj` folder into Blender's add-ons directory, where
`<version>` is your Blender version (e.g. `5.1`).

Linux, `~/.config/blender/<version>/scripts/addons/` (run from this directory):

    mkdir -p ~/.config/blender/<version>/scripts/addons
    ln -s "$(pwd)/io_scene_pfobj" ~/.config/blender/<version>/scripts/addons/

Windows, `%APPDATA%\Blender Foundation\Blender\<version>\scripts\addons\`:

    xcopy /E /I io_scene_pfobj "%APPDATA%\Blender Foundation\Blender\<version>\scripts\addons\io_scene_pfobj"

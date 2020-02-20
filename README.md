## ![Logo](docs/images/logo.png) ##

Permafrost Engine is an OpenGL 3.3 Real Time Strategy game engine written in C. 
It is made in the image of old classics, but incorporating some modern ideas.

## Engine Showcase ##

###### Hellknight from DOOM 3 by id Sofware ######
![Hellknight](docs/images/hellknight.gif)
###### Sinbad by Zi Ye ######
![Sinbad](docs/images/sinbad.gif)
###### Tile-Based Map ######
![Terrain](docs/images/terrain.png)
###### Map Editor ######
![Map Editor](docs/images/editor.png)
###### Baked Navigation Grid ######
![Navigation Grid](docs/images/navigation.png)

## Engine Summary ##

* OpenGL 3.3 programmable pipeline
* Custom ASCII model format with Blender export script
* Skeletal animation with GPU skinning
* Phong reflection model with materials
* Directional light shadow mapping
* RTS camera, FPS camera
* Rendering of tile-based world parsed from ASCII file
* Water rendering (including reflection, refraction, soft edge effects)
* Export/Import of game entites to/from ASCII files
* Engine internals exposed to Python 2.7 for scripting
* Event system
* UI framework (Nuklear-based)
* Efficient raycasting
* Map/Scene editor
* Pause/Resume system
* Fast rendering of huge maps
* Map navigation graph/grid generation
* Implementation of 'boids' steering/flocking behaviours
* Hierarchial flow field pathfinding
* Handling of dynamic obstacles in pathfinding
* Dynamic collision avoidance of multiple entities using Hybrid Reciprocal Velocity Obstacles and the ClearPath algorithm
* Efficient spatial indexing using a quadtree
* RTS minimap
* RTS-style unit selection
* RTS unit combat system
* Support for different resolutions and aspect ratios
* Configurable graphics settings
* Serialization and deserialization of the Python interpreter state
* Multithreaded: simulation and rendering in a 2-stage pipeline
* Cross-platform (Linux and Windows)

## Dependencies ##

* SDL2 2.0.10
* GLEW 2.1.0
* python 2.7.17
* stb_image.h, stb_image_resize.h
* khash.h
* nuklear.h

All dependencies can be built from source and distributed
along with the game binary if desired. 

## Building Permafrost Engine ##

#### For Linux ####

1. `git clone --recursive https://github.com/eduard-permyakov/permafrost-engine.git`
2. `cd permafrost-engine`
3. `make deps` (to build the shared library dependencies to `./lib`)
4. `make pf`

Now you can invoke `make run` to launch the demo or `make run_editor` to launch the map editor.
Optionally, invoke `make launchers` to create the `./demo` and `./editor` binaries which don't 
require any arguments.

#### For Windows ####

Python must be either compiled using MSVC build tools and the solution file found in the
the source's `PCbuild` directory, or obtained in binary form (https://www.python.org/download/releases/2.7/). 
Prior to building, copy `python27.dll`, as well as the `Lib` and `DLLs` folders from the Python 
installation directory to `./lib`. Copy the file `include/pyconfig.h` from the installation directory
to `./deps/Python/Include/`.

The rest of the source code can be built using the mingw-w64 cross-compilation toolchain 
(http://mingw-w64.org/doku.php) using largely the same steps as for Linux. Passing `PLAT=WINDOWS` 
to the make environment is the only required change.

The compliation can either be done on a Linux host, or natively on Windows using MSYS2 (https://www.msys2.org/).

1. `git clone --recursive https://github.com/eduard-permyakov/permafrost-engine.git`
2. `cd permafrost-engine`
3. `make deps PLAT=WINDOWS`
4. `make pf PLAT=WINDOWS`
5. `make launchers PLAT=WINDOWS`

## License ##

Permafrost Engine is licensed under the GPLv3, with a special linking exception.

## Comments/Questions ##

Comments or questions regarding the project or the source code? E-mail: edward.permyakov@gmail.com


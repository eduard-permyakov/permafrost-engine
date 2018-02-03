## ![Logo](docs/images/logo.png) ##

Permafrost Engine is an OpenGL 3.3 RTS game engine written in C. It is still
in the early phases of development.

## Engine Showcase ##

###### Hellknight from DOOM 3 by id Sofware. ######
![Hellknight](docs/images/hellknight.gif)
###### Sinbad by Zi Ye. ######
![Sinbad](docs/images/sinbad.gif)
###### Example of Tile-based map. ######
![Terrain](docs/images/terrain.png)

## Engine Summary ##

* OpenGL 3.3 programmable pipeline
* Custom ASCII model format with Blender export script
* Skeletal animation with GPU skinning
* Phong reflection model with materials
* RTS camera, FPS camera
* Rendering of tile-based world parsed from ASCII file
* Engine internals exposed to Python 2.7 for scripting
* Cross-platform (Linux and Windows)

## Dependencies ##

* SDL2 2.0.7
* GLEW 2.1.0
* python 2.7.13
* stb_image.h
* khash.h, kvec.h, klist.h

All dependencies can be built from source and distributed
along with the game binary if desired. 

## Building Permafrost Engine ##

#### On Linux ####

1. `git clone https://github.com/eduard-permyakov/permafrost-engine.git`
2. `cd permafrost-engine`
3. Optionally `make deps` to build the shared library dependencies to `./lib`
   If you skip this step, your linker must be able to satisfy `-lGLEW`, `lSDL2`, and 
   `-lpython2.7`.
3. `make pf`
4. `make run` (runs the binary in `./bin` with default arguments)

#### On Windows ####

1. Python must be compiled using MSVC build tools and the solution file found in the
   the source's `PCbuild` directory. Afterwards, copy `python27.dll`(and extension shared
   libraries if you wish) and the `Lib` folder to `./lib`. Alternatively, you may try 
   to link against an existing Python installation elsewhere on your system.
2. The rest of the source code can be built with MinGW and MSYS using largely the same steps
   as on Linux.
3. `run.bat` will launch the binary with default arguments.


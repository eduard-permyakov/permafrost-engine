## ![Logo](docs/images/logo.png) ##

Permafrost Engine is an OpenGL 3.3 rendering engine written in C. One day it may
grow to be a fully-featured game engine.

## Engine Showcase ##

![Hellknight](docs/images/hellknight.gif)
Hellknight from DOOM 3 by id Sofware.
![Sinbad](docs/images/sinbad.gif)
Sinbad by Zi Ye.

## Engine Summary ##

* OpenGL 3.3 programmable pipeline
* Custom ASCII model format with Blender export script
* Skeletal animation with GPU skinning
* Phong reflection model with materials
* FPS camera

## Dependencies ##

* SDL2
* GLEW
* stb_image.h (part of source code)

## Building Permafrost Engine ##

The engine can only be built on Linux for the time being.

First, make sure you have `SDL2` and `GLEW` installed on your platform. Otherwise, 
the build will fail.

Then, in the top-level directory:

1. `make pf``
2. `make run` (runs the binary in `./bin/` with default arguments)


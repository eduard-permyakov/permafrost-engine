/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef CAM_CONTROL_H
#define CAM_CONTROL_H

#include "camera.h"

#include <SDL.h>

/*
 * Installation will set the specified camera as the currently active camera,
 * from whose point of view the world will be rendered.
 * The 'FPS' and 'RTS' modes control how mouse and keyboard events are used to 
 * transform the active camera.
 */
void CamControl_FPS_Install(struct camera *cam);
void CamControl_RTS_Install(struct camera *cam);
void CamControl_UninstallActive(void);

#endif

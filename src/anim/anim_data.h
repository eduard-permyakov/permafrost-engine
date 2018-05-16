/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017 Eduard Permyakov 
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

#ifndef ANIM_DATA_H
#define ANIM_DATA_H

#include "public/skeleton.h"
#include "../pf_math.h"
#include "../collision.h"

#include <stddef.h>

#define ANIM_NAME_LEN  32

struct anim_sample{
    struct SQT  *local_joint_poses;
    struct aabb  sample_aabb;
};

struct anim_clip{
    char                name[ANIM_NAME_LEN];
    struct skeleton    *skel;
    unsigned            num_frames;
    struct anim_sample *samples;
};

struct anim_data{
    unsigned          num_anims;
    struct skeleton   skel;
    struct anim_clip *anims;
};

#endif

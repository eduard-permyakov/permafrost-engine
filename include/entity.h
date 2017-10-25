#ifndef ENTITY_H
#define ENTITY_H

#include <mesh.h>
#include <anim_data.h>

#define ENTITY_NAME_LEN 32

struct entity{
    unsigned         id;
    char             name[ENTITY_NAME_LEN];
    struct mesh      mesh;    
    struct anim_data anim_data;
    char             mem[];
};

#endif

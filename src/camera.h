#ifndef CAMERA_H
#define CAMERA_H

#include "pf_math.h"

typedef struct camera camera_t;
extern const unsigned g_sizeof_camera;

#define DECL_CAMERA_STACK(_name)        \
    char _name[g_sizeof_camera]         \

struct camera *Camera_New (void);
void           Camera_Free(struct camera *cam);

/* Initialize camera to a known state if declaring it on the stack 
 * instead of getting a pointer with 'Camera_New'
 */
void           Camera_InitStack(struct camera *cam);

void           Camera_SetPos   (struct camera *cam, vec3_t pos);
void           Camera_SetFront (struct camera *cam, vec3_t dir);
void           Cameta_SetUp    (struct camera *cam, vec3_t up);
void           Camera_SetSpeed (struct camera *cam, float speed);
void           Camera_SetSens  (struct camera *cam, float sensitivity);

/* These should be called once per tick, at most. The amount moved depends 
 * on the camera speed. 
 */
void           Camera_MoveLeftTick   (struct camera *cam);
void           Camera_MoveRightTick  (struct camera *cam);
void           Camera_MoveFrontTick  (struct camera *cam);
void           Camera_MoveBackTick   (struct camera *cam);
void           Camera_ChangeDirection(struct camera *cam, int dx, int dy);

/* Should be called once per frame, after all movements have been set, but 
 * prior to rendering.
 */
void           Camera_TickFinish(struct camera *cam);

#endif

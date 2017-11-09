#ifndef CAMERA_H
#define CAMERA_H

#include "pf_math.h"

typedef struct camera camera_t;
extern const unsigned g_sizeof_camera;

#define DECL_CAMERA_STACK(_name)        \
    char _name[g_sizeof_camera];        \

struct camera *camera_new  (void);
void           camera_free (struct camera *cam);

/* Initialize camera to a known state if declaring it on the stack 
 * instead of getting a pointer with 'camera_new'
 */
void           camera_init_stack(struct camera *cam);

void           camera_set_pos   (struct camera *cam, vec3_t pos);
void           camera_set_front (struct camera *cam, vec3_t dir);
void           camera_set_up    (struct camera *cam, vec3_t up);
void           camera_set_speed (struct camera *cam, float speed);
void           camera_set_sens  (struct camera *cam, float sensitivity);

/* These should be called once per tick, at most. The amount moved depends 
 * on the camera speed. 
 */
void           camera_move_left_tick  (struct camera *cam);
void           camera_move_right_tick (struct camera *cam);
void           camera_move_front_tick (struct camera *cam);
void           camera_move_back_tick  (struct camera *cam);
void           camera_change_direction(struct camera *cam, int dx, int dy);

/* Should be called once per frame, after all movements have been set, but 
 * prior to rendering.
 */
void           camera_tick_finish (struct camera *cam);

#endif

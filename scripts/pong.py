#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2020 Eduard Permyakov 
#
#  Permafrost Engine is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  Permafrost Engine is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
# 
#  Linking this software statically or dynamically with other modules is making 
#  a combined work based on this software. Thus, the terms and conditions of 
#  the GNU General Public License cover the whole combination. 
#  
#  As a special exception, the copyright holders of Permafrost Engine give 
#  you permission to link Permafrost Engine with independent modules to produce 
#  an executable, regardless of the license terms of these independent 
#  modules, and to copy and distribute the resulting executable under 
#  terms of your choice, provided that you also meet, for each linked 
#  independent module, the terms and conditions of the license of that 
#  module. An independent module is a module which is not derived from 
#  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
#  extend this exception to your version of Permafrost Engine, but you are not 
#  obliged to do so. If you do not wish to do so, delete this exception 
#  statement from your version.
#

import pf
import math
import traceback

############################################################
# Debugging setup                                          #
############################################################

def on_task_exc(user, event):
    traceback.print_exception(event[1], event[2], event[3])

pf.register_event_handler(pf.EVENT_SCRIPT_TASK_EXCEPTION, on_task_exc, None)

############################################################
# Global configs                                           #
############################################################

pf.load_map("assets/maps", "plain.pfmap")
pf.set_ambient_light_color((1.0, 1.0, 1.0))
pf.set_emit_light_color((1.0, 1.0, 1.0))
pf.set_emit_light_pos((1664.0, 1024.0, 384.0))
pf.disable_fog_of_war()
pf.set_minimap_size(0)
pf.disable_unit_selection()

############################################################
# Camera setup                                             #
############################################################

pong_cam = pf.Camera(mode=pf.CAM_MODE_FREE, position = (0.0, 175.0, 0.0), pitch = -90.0, yaw = 180.0)
pf.set_active_camera(pong_cam)

############################################################
# Entities setup                                           #
############################################################

FIELD_WIDTH = 240
FIELD_HEIGHT = 120
PADDLE_HEIGHT = 16
PLAYER_SPEED = 2.5
COMPUTER_SPEED = 2.5
BALL_SPEED = 5.0

obelisks = []

# top, bot border
for i in range(-FIELD_WIDTH/2, FIELD_WIDTH/2, 4):
    top = pf.Entity("assets/models/props", "obelisk_2.pfobj", "border")
    top.pos = (-FIELD_HEIGHT/2.0, 0.0, i)
    obelisks += [top]

    bot = pf.Entity("assets/models/props", "obelisk_2.pfobj", "border")
    bot.pos = (FIELD_HEIGHT/2.0, 0.0, i)
    obelisks += [bot]

# left, right border
for i in range(-FIELD_HEIGHT/2, FIELD_HEIGHT/2, 4):
    left = pf.Entity("assets/models/props", "obelisk_2.pfobj", "border")
    left.pos = (i, 0.0, -FIELD_WIDTH/2.0)
    obelisks += [left]

    right = pf.Entity("assets/models/props", "obelisk_2.pfobj", "border")
    right.pos = (i, 0.0, FIELD_WIDTH/2.0)
    obelisks += [right]

ball = pf.Entity("assets/models/barrel", "barrel.pfobj", "ball")
ball.pos = (0.0, 0.0, 0.0)
ball.scale = (8.0, 8.0, 8.0)

left_paddle = pf.Entity("assets/models/props", "wood_fence_1.pfobj", "left_paddle")
left_paddle.pos = (0.0, 0.0, -FIELD_WIDTH/2.0 * 0.8)
left_paddle.scale = (2.5, 2.5, 2.5)

right_paddle = pf.Entity("assets/models/props", "wood_fence_1.pfobj", "right_paddle")
right_paddle.pos = (0.0, 0.0, FIELD_WIDTH/2.0 * 0.8)
right_paddle.scale = (2.5, 2.5, 2.5)

############################################################
# Gameplay loop                                            #
############################################################

player_score = 0
computer_score = 0

def intersect(ball, paddle):
    if ball.pos[2] < paddle.pos[2] - 3.0:
        return False
    if ball.pos[2] > paddle.pos[2] + 3.0:
        return False
    if ball.pos[0] < paddle.pos[0] - PADDLE_HEIGHT/2.0:
        return False
    if ball.pos[0] > paddle.pos[0] + PADDLE_HEIGHT/2.0:
        return False
    return True

class PlayerPaddleActor(pf.Task):
    def __run__(self):
        while True:
            scancode = self.await_event(pf.SDL_KEYDOWN)[0]
            if scancode == pf.SDL_SCANCODE_UP and left_paddle.pos[0] > -FIELD_HEIGHT/2.0 + PADDLE_HEIGHT/2.0:
                left_paddle.pos = left_paddle.pos[0] - PLAYER_SPEED, 0.0, left_paddle.pos[2]
            elif scancode == pf.SDL_SCANCODE_DOWN and left_paddle.pos[0] < FIELD_HEIGHT/2.0 - PADDLE_HEIGHT/2.0:
                left_paddle.pos = left_paddle.pos[0] + PLAYER_SPEED, 0.0, left_paddle.pos[2]

class ComputerPaddleActor(pf.Task):
    def __run__(self):
        while True:
            self.await_event(pf.EVENT_30HZ_TICK)
            if right_paddle.pos[0] > ball.pos[0] and right_paddle.pos[0] > -FIELD_HEIGHT/2.0 + PADDLE_HEIGHT/2.0:
                right_paddle.pos = right_paddle.pos[0] - COMPUTER_SPEED, 0.0, right_paddle.pos[2]
            if right_paddle.pos[0] < ball.pos[0] and right_paddle.pos[0] < FIELD_HEIGHT/2.0 - PADDLE_HEIGHT/2.0:
                right_paddle.pos = right_paddle.pos[0] + COMPUTER_SPEED, 0.0, right_paddle.pos[2]

class BallActor(pf.Task):
    def random_vel(self):
        x = 5 + pf.rand(5)
        z = 5 + pf.rand(5)
        mag = math.sqrt(x**2 + z**2)
        return [(x - 10.0) / mag * BALL_SPEED, (z - 10.0) / mag * BALL_SPEED]

    def __init__(self):
        self.velocity = self.random_vel()

    def __run__(self):
        while True:
            self.await_event(pf.EVENT_30HZ_TICK)
            ball.pos = (ball.pos[0] + self.velocity[0], 0.0, ball.pos[2] + self.velocity[1])
            if intersect(ball, left_paddle):
                self.velocity[1] *= -1.0
            if intersect(ball, right_paddle):
                self.velocity[1] *= -1.0
            if ball.pos[0] >= FIELD_HEIGHT/2.0 or ball.pos[0] <= -FIELD_HEIGHT/2.0:
                self.velocity[0] *= -1.0
            if ball.pos[2] >= FIELD_WIDTH/2.0:
                global player_score
                player_score += 1
                ball.pos = (0.0, 0.0, 0.0)
                self.velocity = self.random_vel()
            if ball.pos[2] <= -FIELD_WIDTH/2.0:
                global computer_score
                computer_score += 1
                ball.pos = (0.0, 0.0, 0.0)
                self.velocity = self.random_vel()

class UIActor(pf.Task):
    def __run__(self):
        while True:
            global player_score, computer_score
            self.await_event(pf.EVENT_UPDATE_START)
            pf.draw_text("PLAYER: {}, COMPUTER: {}".format(player_score, computer_score), 
                (25, 25, 250, 50), (255, 0, 0, 255))

PlayerPaddleActor().run()
ComputerPaddleActor().run()
BallActor().run()
UIActor().run()


import pf

pf.set_ambient_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_pos([0.0, 300.0, 0.0])

pf.new_game("./assets/maps/grass-cliffs-1", "grass-cliffs.pfmap", "grass-cliffs.pfmat")

sinbad = pf.AnimEntity("assets/models/sinbad", "Sinbad.pfobj", "Sinbad", "IdleBase")
sinbad.activate()

sinbad.pos = [0.0, 5.0, -50.0]
sinbad.scale = [1.0, 1.0, 1.0]

sinbad.play_anim("Dance")


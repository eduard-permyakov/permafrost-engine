import pf

pf.set_ambient_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_pos([0.0, 300.0, 0.0])

pf.new_game("assets/maps/grass-cliffs-1", "grass-cliffs.pfmap", "grass-cliffs.pfmat")

sinbad = pf.AnimEntity("assets/models/sinbad", "Sinbad.pfobj", "Sinbad", "Dance")
sinbad.pos = [0.0, 8.0, -50.0]
sinbad.scale = [1.0, 1.0, 1.0]
sinbad.activate()

oak_tree = pf.Entity("assets/models/oak_tree", "oak_tree.pfobj", "OakTree")
oak_tree.pos = [30.0, 8.0, -50.0]
oak_tree.scale = [2.0, 2.0, 2.0]
oak_tree.activate()

oak_leafless = pf.Entity("assets/models/oak_tree", "oak_leafless.pfobj", "OakLeafless")
oak_leafless.pos = [0.0, 8.0, -10.0]
oak_leafless.scale = [1.5, 1.5, 1.5]
oak_leafless.activate()


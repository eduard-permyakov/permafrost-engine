import pf

sinbad = pf.AnimEntity("assets/models/sinbad", "Sinbad.pfobj", "Sinbad", "Dance")
sinbad.pos = [100.0, 50.0, -25.0]
sinbad.scale = [1.0, 1.0, 1.0]

sinbad.play_anim("IdleBase")

print sinbad.pos
print sinbad.scale
print sinbad.name


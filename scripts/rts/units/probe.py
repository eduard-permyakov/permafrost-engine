import pf


RESOURCE_NAME = "wood"


def _scene_meta(path, cls, construct_args, static=True, collision=True):
    return {
        "path": path,
        "anim": False,
        "static": static,
        "collision": collision,
        "class": cls,
        "construct_args": construct_args,
    }


class ProbeWorker(pf.BuilderEntity, pf.HarvesterEntity, pf.MovableEntity, pf.GarrisonEntity):
    session_scene_meta = _scene_meta(
        "cart/cart.pfobj",
        "ProbeWorker",
        ("assets/models/cart", "cart.pfobj", "ProbeWorker"),
        static=False,
    )

    def __init__(self, path, pfobj, name, build_speed=512, **kwargs):
        del kwargs
        pf.BuilderEntity.__init__(self, path, pfobj, name, build_speed=build_speed)


class ProbeResource(pf.ResourceEntity):
    session_scene_meta = _scene_meta(
        "hay/haystack.pfobj",
        "ProbeResource",
        ("assets/models/hay", "haystack.pfobj", "ProbeResource"),
    )

    def __init__(self, path, pfobj, name, resource_name=RESOURCE_NAME, resource_amount=1, **kwargs):
        del kwargs
        pf.ResourceEntity.__init__(
            self,
            path,
            pfobj,
            name,
            resource_name=resource_name,
            resource_amount=resource_amount,
        )


class ProbeStorage(pf.StorageSiteEntity):
    session_scene_meta = _scene_meta(
        "crate/crate_1.pfobj",
        "ProbeStorage",
        ("assets/models/crate", "crate_1.pfobj", "ProbeStorage"),
    )


class ProbeBuildable(pf.BuildableEntity):
    session_scene_meta = _scene_meta(
        "build_site_marker/build-site-marker.pfobj",
        "ProbeBuildable",
        ("assets/models/build_site_marker", "build-site-marker.pfobj", "ProbeBuildable"),
    )

    def __init__(self, path, pfobj, name, required_resources=None, pathable=True, **kwargs):
        del kwargs
        if required_resources is None:
            required_resources = {}
        pf.BuildableEntity.__init__(
            self,
            path,
            pfobj,
            name,
            required_resources=required_resources,
            pathable=pathable,
        )


class ProbeGarrisonable(pf.GarrisonableEntity):
    session_scene_meta = _scene_meta(
        "cart/cart.pfobj",
        "ProbeGarrisonable",
        ("assets/models/cart", "cart.pfobj", "ProbeGarrisonable"),
    )

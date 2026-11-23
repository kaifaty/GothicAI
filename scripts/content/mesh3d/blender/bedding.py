"""The bedding of a bed without a canopy, for gen_prop.py ("own": {"module": "bedding"}): the plain and the rich bed
of city-furniture, by the recipe accepted on the master's bed (2026-11-03, reports/content/remaster/city-furniture/
decisions.md): the generator's mattress is cut away and a cloth laid by coverlet_sim.py takes its place, in a
material of its own (gen_prop "materials").

own["coverlet"]: coverlet_sim.py's npz; without it, canopy.mattress(**own["mattress"]). own["kind"]: the parts'
kind, the key of "materials" ("tick" by default).
"""
import canopy

material = canopy.material


def build(own=None, source=False):
    own = own or {}
    kind = own.get("kind", "tick")
    if own.get("coverlet"):
        return [canopy.coverlet(own["coverlet"], kind)]
    return [canopy.mattress(kind=kind, **own.get("mattress", {}))]

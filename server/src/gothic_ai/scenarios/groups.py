"""Model groups from the [2026-10-31 census](e2e-llm-census.md)."""

from importlib import import_module
from pkgutil import iter_modules

from .. import places, scenario_fight_reactions
from . import __path__

OFF, GIVEN, LIVE = set(), set(), set()
RUN = {}
_groups = {"OFF": OFF, "GIVEN": GIVEN, "LIVE": LIVE}
_modules = [places, scenario_fight_reactions]
_modules.extend(
    import_module(info.name)
    for info in iter_modules(__path__, prefix=__package__ + ".")
    if info.name != __name__
)
for _module in _modules:
    for _name, (_group, _run) in getattr(_module, "SCENARIOS", {}).items():
        _groups[_group].add(_name)
        if _run is not None:
            RUN[_name] = _run

"""The error of the world edit tool (world_edit.py, world_region.py): one class however world_edit.py is loaded.

Run as a script, world_edit.py is __main__ and world_region.py imports it again as world_edit: an EditError
defined there would be two classes, and main() would miss the one world_region raises."""


class EditError(RuntimeError):
    pass

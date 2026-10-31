import sys

# The Ryzen desktop session imports fcntl at its top and exists only on Linux; on Windows the core runs
# the Supervisor itself (test_windows.py).
collect_ignore = ["test_linux_session.py"] if sys.platform == "win32" else []

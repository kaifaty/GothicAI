"""Legacy invocation: RUN MDH MDM SKIN ROOT REPO [NECK]."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "character-tool" / "src"))
from character_tool.body.mpfb_write import write

write(*map(Path, sys.argv[1:6]), *sys.argv[7:8])

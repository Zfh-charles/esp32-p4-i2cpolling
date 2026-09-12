"""Compatibility entry point for the canonical legacy ROI utility."""
from pathlib import Path
import sys

_canonical = Path(__file__).resolve().parents[1] / 'emotion_tool_dev' / Path(__file__).name
sys.path.insert(0, str(_canonical.parent))
__file__ = str(_canonical)
exec(compile(_canonical.read_bytes(), __file__, 'exec'), globals())

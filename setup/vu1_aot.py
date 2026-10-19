"""Optional independent static-package generator; outputs stay outside checkout."""
from pathlib import Path
import runpy, sys
tool = Path(__file__).resolve().parents[1] / 'tools/vu1/package.py'
sys.path.insert(0, str(tool.parent))
runpy.run_path(str(tool), run_name='__main__')

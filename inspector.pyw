"""Double-click entry point; startup errors remain visible without a console."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))

try:
    from inspector_gui import main
    main()
except Exception:
    import ctypes
    import traceback
    ctypes.windll.user32.MessageBoxW(None, traceback.format_exc(), 'D3D11 Inspector 启动失败', 0x10)

import ctypes
def low_priority():
 if hasattr(ctypes,'windll'):ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(),0x4000)

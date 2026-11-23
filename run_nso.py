# Runs the project's tools/nso.py with a no-op c++filt (names stay mangled); this PC has no binutils.
import subprocess, sys, types, runpy
_real = subprocess.run
def fake(args, *a, **k):
    if args and args[0] == "c++filt":
        return types.SimpleNamespace(stdout=k.get("input", ""), returncode=0)
    return _real(args, *a, **k)
subprocess.run = fake
sys.argv = ["nso.py"] + sys.argv[1:]
sys.path.insert(0, r"C:\Users\wehng\mh rise recmp\mhrise-recomp\tools")
runpy.run_path(r"C:\Users\wehng\mh rise recmp\mhrise-recomp\tools\nso.py", run_name="__main__")

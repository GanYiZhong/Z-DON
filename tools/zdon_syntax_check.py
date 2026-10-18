"""zdon_syntax_check.py <src/...cpp> [...]: compile each file with -fsyntax-only using the build's
compile_commands.json (no object files, no build-dir changes; safe to run while others edit).
Headers are checked through a .cpp that includes them. Exit code 1 if any file has errors."""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
cc = json.load(open(f"{ROOT}/build/compile_commands.json"))
env = dict(os.environ, PATH="C:/msys64/mingw64/bin;" + os.environ.get("PATH", ""))
bad = False
for rel in sys.argv[1:]:
    rel = rel.replace("\\", "/")
    want = f"{ROOT}/{rel}".lower() if not os.path.isabs(rel) else rel.lower()
    e = next((e for e in cc if os.path.normpath(e["file"]).replace("\\", "/").lower() == os.path.normpath(want).replace("\\", "/").lower()), None)
    if not e:
        print(f"{rel}: not in compile_commands.json")
        bad = True
        continue
    cmd = re.sub(r"\s-o\s+\S+", " ", e["command"]) + " -fsyntax-only"
    r = subprocess.run(cmd, cwd=e["directory"], env=env, capture_output=True, text=True)
    errs = [l for l in r.stderr.splitlines() if "error" in l.lower() and "missing terminating" not in l]
    if r.returncode != 0:
        bad = True
        print(f"{rel}: FAIL")
        print("\n".join(errs[:40]) or r.stderr[-3000:])
    else:
        print(f"{rel}: ok")
sys.exit(1 if bad else 0)

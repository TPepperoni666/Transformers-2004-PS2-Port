"""Fetch the optional SDK signature header into private build inputs only.

The upstream repository's GPL text does not establish a separate database license.
Never include this private input or an analyzer embedding it in a public release.
"""
from pathlib import Path
import hashlib, os, subprocess, uuid

UPSTREAM = "https://github.com/ran-j/PS2Recomp.git"
REVISION = "7562ec14c9b3b4f9f09040a5e950a4bb67ab7a66"
SOURCE_PATH = "ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h"
SHA256 = "0fdd1168a815c8af2529cae9076ae813a1304c5c3a41e48d88d38e21e7b5833d"

def verify(data):
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError("SCE database hash mismatch; refusing to use it")
    return data

def ensure(work):
    target = Path(work) / ("sce-database-" + SHA256 + ".h")
    if target.exists():
        verify(target.read_bytes())
        return target
    checkout = Path(work) / ("sce-upstream-" + uuid.uuid4().hex)
    # A unique directory avoids reusing a user's repository/configuration.
    subprocess.run(["git", "init", str(checkout)], check=True)
    subprocess.run(["git", "-C", str(checkout), "fetch", "--depth=1", UPSTREAM, REVISION], check=True)
    data = subprocess.check_output(["git", "-C", str(checkout), "show", REVISION + ":" + SOURCE_PATH])
    verify(data)
    pending = target.with_name(target.name + "." + uuid.uuid4().hex + ".pending")
    with pending.open("xb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(pending, target)
    return target

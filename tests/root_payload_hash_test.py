import hashlib
import re
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def shell_value(source: str, name: str) -> str:
    match = re.search(rf"(?m)^{re.escape(name)}=([0-9a-f]{{64}})$", source)
    if not match:
        raise SystemExit(f"Missing finalized shell hash: {name}")
    return match.group(1)


runtime_path = Path("packaging/root/bluetooth-runtime.sh")
uninstall_path = Path("packaging/root/uninstall.sh")
install_path = Path("packaging/root/install.sh")
root_source_path = Path("src/root_feedback.c")

install = install_path.read_text(encoding="utf-8")
if shell_value(install, "RUNTIME_SOURCE_SHA") != sha256(runtime_path):
    raise SystemExit("install.sh does not authenticate the packaged runtime bytes")
if shell_value(install, "UNINSTALL_SOURCE_SHA") != sha256(uninstall_path):
    raise SystemExit("install.sh does not authenticate the packaged uninstaller bytes")

root_source = root_source_path.read_text(encoding="utf-8")
match = re.search(
    r'#define ROOT_INSTALLER_SHA256\s*\\?\s*"([0-9a-f]{64})"', root_source
)
if not match:
    raise SystemExit("root_feedback.c is missing its finalized installer hash")
if match.group(1) != sha256(install_path):
    raise SystemExit("root_feedback.c does not authenticate the packaged installer bytes")

print("Root payload hash chain verified")

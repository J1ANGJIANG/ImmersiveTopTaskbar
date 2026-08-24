import json
import os
import pathlib

packages = pathlib.Path(os.environ["LOCALAPPDATA"]) / "Packages"
settings = None
for path in packages.glob("*TranslucentTB*/RoamingState/settings.json"):
    settings = path
    break

if not settings:
    raise SystemExit("settings.json not found")

print(settings)
raw = settings.read_bytes()
print("size", len(raw), "head", raw[:32])
text = "\n".join(
    line for line in raw.decode("utf-8-sig").splitlines()
    if not line.lstrip().startswith("//")
)
data = json.loads(text)
print(json.dumps(data.get("maximized_window_appearance"), ensure_ascii=False, indent=2))
print(json.dumps(data.get("desktop_appearance"), ensure_ascii=False, indent=2))

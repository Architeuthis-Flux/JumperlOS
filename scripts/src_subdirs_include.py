"""
PlatformIO pre-build script: put every feature folder under src/ on the
include path.

src/ keeps the main files at the top level and the feature-specific ones in
folders (src/routing, src/sensing, src/hardwarestuff, ...). So that a file can
say `#include "AdcRing.h"` wherever AdcRing.h lives, this adds each immediate
subdirectory of src/ (recursively, minus a few that are not code) to CPPPATH -
no folder names in the includes, no per-folder -I lines to maintain in
platformio.ini. Runs before the build (extra_scripts pre:) so libraries and
the project sources see the same paths.
"""

Import("env")
import os

project_dir = env["PROJECT_DIR"]
src_dir = os.path.join(project_dir, "src")

# Folders that hold no headers anyone includes by bare name (tooling, fonts
# reached by their own paths). Everything else is a feature folder.
SKIP = {"fonts", ".git", "__pycache__"}

paths = []
for root, dirs, files in os.walk(src_dir):
    dirs[:] = sorted(d for d in dirs if d not in SKIP and not d.startswith("."))
    if root == src_dir:
        continue
    if any(f.endswith((".h", ".hpp", ".pio.h")) for f in files):
        paths.append(root)

if paths:
    env.Append(CPPPATH=paths)
    print("src feature folders on the include path: " + ", ".join(os.path.relpath(p, project_dir) for p in paths))

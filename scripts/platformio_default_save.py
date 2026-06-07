"""Pre-build: refresh data/save.srm from the source game save.

The SPIFFS `storage` image (built from data/ via spiffs_create_partition_image)
carries the bundled default Game Boy save. This script regenerates that bundled
save from the source game/*.srm before the image is built, so the default save can
never drift from — or be missing relative to — the source. It mirrors how
platformio_active_rom.py keeps the active ROM in sync.

The copy + 32 KB validation lives in scripts/copy_default_save.mjs (single source of
truth, also used by the webui `npm run build`). We invoke it via Node here. If Node
is unavailable we fall back to validating the already-present data/save.srm so the
build still fails on a missing/wrong-size default save.
"""

import shutil
import subprocess
from pathlib import Path

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
copy_script = project_dir / "scripts" / "copy_default_save.mjs"
bundled_save = project_dir / "data" / "save.srm"
EXPECTED_SIZE = 32768


def _abort(message):
    raise SystemExit(f"[default-save] {message}")


def _validate_existing(context):
    # The build may only proceed with a valid 32 KB bundled save. If we could not
    # refresh it, accept an already-present valid one; otherwise abort.
    if not bundled_save.exists():
        _abort(f"{context} and no bundled save to fall back on: {bundled_save}")
    size = bundled_save.stat().st_size
    if size != EXPECTED_SIZE:
        _abort(
            f"{context} and existing bundled save is {size} bytes "
            f"(expected {EXPECTED_SIZE}): {bundled_save}"
        )
    print(f"[default-save] {context}; using existing valid {bundled_save}")


if not copy_script.exists():
    print(f"[default-save] copy script not found, skipping: {copy_script}")
elif shutil.which("node") is None:
    _validate_existing("Node unavailable")
else:
    result = subprocess.run(["node", str(copy_script)], cwd=str(project_dir))
    if result.returncode != 0:
        # Node could not regenerate the save (e.g. missing source, or a restricted
        # environment). Fall back to a valid pre-existing bundle; abort only if none.
        _validate_existing("copy_default_save.mjs failed (see error above)")
    else:
        print(f"[default-save] Bundled save refreshed: {bundled_save}")

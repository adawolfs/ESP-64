from pathlib import Path

Import("env")
from SCons.Script import COMMAND_LINE_TARGETS

project_dir = Path(env.subst("$PROJECT_DIR"))
active_rom = project_dir / "roms" / "active.gb"

if active_rom.exists():
    env.Append(FLASH_EXTRA_IMAGES=[("0x190000", str(active_rom))])
    targets = set(COMMAND_LINE_TARGETS)
    if "upload" in targets and "uploadfs" not in targets:
        env.Append(UPLOADERFLAGS=["0x190000", str(active_rom)])
    print(f"Active ROM image will be flashed to 0x190000: {active_rom}")

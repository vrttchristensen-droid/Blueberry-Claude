#!/usr/bin/env bash
# Rebuilds both firmwares with PlatformIO and refreshes:
#   server/static/flash/*.bin              (browser installer at <server>/flash/)
#   firmware/arduino/*/*.ino               (single-file Arduino IDE sketches)
# Run from the repo root: ./tools/build_installer.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ESPTOOL=~/.platformio/packages/tool-esptoolpy/esptool.py
BOOT_APP0=~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin

for node in button_node camera_node; do
  # The installer builds are generic: settings come from the phone setup page.
  cp "firmware/$node/include/config.example.h" "firmware/$node/include/config.h.installer"
  [ -f "firmware/$node/include/config.h" ] && mv "firmware/$node/include/config.h" "firmware/$node/include/config.h.mine"
  mv "firmware/$node/include/config.h.installer" "firmware/$node/include/config.h"
  (cd "firmware/$node" && pio run)
  rm "firmware/$node/include/config.h"
  [ -f "firmware/$node/include/config.h.mine" ] && mv "firmware/$node/include/config.h.mine" "firmware/$node/include/config.h"

  build=$(ls -d firmware/$node/.pio/build/*/)
  python "$ESPTOOL" --chip esp32s3 merge_bin --flash_mode keep --flash_size keep \
    -o "server/static/flash/${node%_node}.bin" \
    0x0 "${build}bootloader.bin" 0x8000 "${build}partitions.bin" \
    0xe000 "$BOOT_APP0" 0x10000 "${build}firmware.bin"
done

python3 - <<'PY'
import re
for node, sketch in [("button_node", "ButtonNode"), ("camera_node", "CameraNode")]:
    src = open(f"firmware/{node}/src/main.cpp").read()
    cfg = open(f"firmware/{node}/include/config.example.h").read().replace("#pragma once\n", "")
    cfg = re.sub(r"^// Copy this file.*\n// config.h is git-ignored.*\n", "", cfg, flags=re.M)
    block = re.search(r'#if __has_include\("config.h"\)\n.*?#endif\n', src, re.S).group(0)
    settings = ("// ============================ YOUR SETTINGS ============================\n"
                + cfg.strip() + "\n// =======================================================================\n")
    open(f"firmware/arduino/{sketch}/{sketch}.ino", "w").write(src.replace(block, settings))
PY
echo "done"

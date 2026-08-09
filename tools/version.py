Import("env")

import os
import shutil
import subprocess
from pathlib import Path


def get_version_string():
    forced_version = os.getenv("FW_VERSION")
    if forced_version:
        return forced_version.strip()

    try:
        result = subprocess.run(
            ["git", "describe", "--tags", "--dirty", "--always"],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()
    except Exception:
        return "dev"


version_string = get_version_string().replace('"', '\\"')
header_path = Path(env.get("PROJECT_DIR")) / "include" / "generated_version.h"
header_path.write_text(
    "#pragma once\n"
    f"#define FIRMWARE_VERSION \"{version_string}\"\n",
    encoding="utf-8",
)


# Embed the payload decoder (chirpstack/decoder.js) into a PROGMEM string so it
# can be served, version-matched, from the WiFi config portal (/decoder route).
# Single source of truth: the repo's decoder.js is the only copy to maintain.
decoder_src = Path(env.get("PROJECT_DIR")) / "chirpstack" / "decoder.js"
decoder_hdr = Path(env.get("PROJECT_DIR")) / "include" / "decoder_js.h"
if decoder_src.exists():
    js = decoder_src.read_text(encoding="utf-8")
    delim = "DECODERJS"
    if f"){delim}\"" in js:
        raise RuntimeError("decoder.js contains the raw-string delimiter )" + delim)
    decoder_hdr.write_text(
        "#pragma once\n"
        f'static const char DECODER_JS[] PROGMEM = R"{delim}(\n'
        + js
        + f'\n){delim}";\n',
        encoding="utf-8",
    )


# Embed README.md into a PROGMEM string so it can be served as a rendered
# help page from the WiFi config portal and the persistent web server
# (/help route). Single source of truth: the repo's README.md is the only
# copy to maintain. The "Project in Pictures" image gallery is stripped since
# the referenced JPEG/PNG files aren't embedded and would just show broken
# image icons.
readme_src = Path(env.get("PROJECT_DIR")) / "README.md"
readme_hdr = Path(env.get("PROJECT_DIR")) / "include" / "readme_md.h"
if readme_src.exists():
    lines = readme_src.read_text(encoding="utf-8").splitlines()
    out_lines = []
    skipping = False
    for line in lines:
        if not skipping and line.strip() == "## Project in Pictures":
            skipping = True
            continue
        if skipping:
            if line.strip() == "---":
                skipping = False
            continue
        out_lines.append(line)
    md = "\n".join(out_lines)

    delim = "READMEMD"
    if f"){delim}\"" in md:
        raise RuntimeError("README.md contains the raw-string delimiter )" + delim)
    readme_hdr.write_text(
        "#pragma once\n"
        f'static const char README_MD[] PROGMEM = R"{delim}(\n'
        + md
        + f'\n){delim}";\n',
        encoding="utf-8",
    )


def copy_firmware(*args, **kwargs):
    build_dir = Path(env.subst("$BUILD_DIR"))
    prog_name = env.subst("$PROGNAME")
    src = build_dir / f"{prog_name}.bin"
    if not src.exists():
        return

    firmware_dir = Path(env.get("PROJECT_DIR")) / "firmware"
    firmware_dir.mkdir(parents=True, exist_ok=True)
    dst = firmware_dir / f"lorawan-weather-node-{version_string}.bin"
    shutil.copyfile(src, dst)
    print(f"Firmware copied to {dst}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)

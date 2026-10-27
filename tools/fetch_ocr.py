#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

"""
fetch_ocr.py — download the OCR runtime and models Makine Live ships.

    python tools/fetch_ocr.py [--cache DIR]

Every file is pinned by URL and SHA-256 and lands in DIR (default: .cache/ocr in
this repository):

    bin/RapidOcrOnnx.dll        RapidOcrOnnx 1.2.3 C API; ONNX Runtime 1.15.1 and
                                OpenCV 4.8.1 are linked into it, nothing else is needed
    models/                     PP-OCR: English detection, text direction, English
                                recognition + its character list
    licenses/                   the licenses of all of the above

CMake copies bin/, models/ and licenses/ into build/package (cache variable
LIVE_OCR_DIR). None of it belongs in git. The runtime comes as a .7z: 7-Zip on PATH
or `pip install py7zr`. A second run checks the hashes and downloads nothing.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request

# RapidOcrOnnx release: the C-library build for VS2022 with the static CRT (-mt).
RUNTIME = {
    "url": "https://github.com/RapidAI/RapidOcrOnnx/releases/download/1.2.3/windows-clib-vs2022-mt.7z",
    "sha256": "94422782ad7e81671cd5b2eea2a227e24d681ae287383a5a4e67652075f719c7",
    "member": "windows-clib-vs2022-mt/win-CLIB-CPU-x64/bin/RapidOcrOnnx.dll",
    "dest": "bin/RapidOcrOnnx.dll",
    "dest_sha256": "49f4875badff975e0c2ca7851731d3fdeae1b62a01cf3ff1f697719d5a80cd41",
}

# PaddleOCR models as converted to ONNX by RapidOCR (URLs and hashes from RapidOCR's
# python/rapidocr/default_models.yaml). The names must match src/ocr_rapid.cpp.
MODELS = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv4"
PADDLE = "https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR"
FILES = [
    ("models/en_PP-OCRv3_det_mobile.onnx", f"{MODELS}/det/en_PP-OCRv3_det_mobile.onnx",
     "ea07c15d38ac40cd69da3c493444ec75b44ff23840553ff8ba102c1219ed39c2"),
    ("models/ch_ppocr_mobile_v2.0_cls_mobile.onnx", f"{MODELS}/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx",
     "e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c"),
    ("models/en_PP-OCRv4_rec_mobile.onnx", f"{MODELS}/rec/en_PP-OCRv4_rec_mobile.onnx",
     "e8770c967605983d1570cdf5352041dfb68fa0c21664f49f47b155abd3e0e318"),
    # The recognition model's character list (also embedded in its ONNX metadata,
    # which RapidOcrOnnx 1.2.3 does not read).
    ("models/en_dict.txt", f"{PADDLE}/ff853c78c97729a6497b0f93968fcfa039c26d8a/ppocr/utils/en_dict.txt",
     "5662df9d2d03f0e8ca0d3b0649d6acbab904b6a14b3d3521463c71c37c668ce3"),

    ("licenses/RapidOcrOnnx-LICENSE.txt",   # Apache-2.0
     "https://raw.githubusercontent.com/RapidAI/RapidOcrOnnx/9de89a6934cd962b80db6ab8ad5627e06a709764/LICENSE",
     "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4"),
    ("licenses/onnxruntime-LICENSE.txt",    # MIT
     "https://raw.githubusercontent.com/microsoft/onnxruntime/baeece44ba075009c6bfe95891a8c1b3d4571cb3/LICENSE",
     "2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c"),
    ("licenses/onnxruntime-ThirdPartyNotices.txt",
     "https://raw.githubusercontent.com/microsoft/onnxruntime/baeece44ba075009c6bfe95891a8c1b3d4571cb3/ThirdPartyNotices.txt",
     "e2f0c1fa97141c2fb91484bb9016ed0a558534bab537c90ee12ce471412aa4d0"),
    ("licenses/opencv-LICENSE.txt",         # Apache-2.0
     "https://raw.githubusercontent.com/opencv/opencv/5199850039ad23f1f0e6cccea5061a9fea5efca6/LICENSE",
     "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"),
    ("licenses/PaddleOCR-LICENSE.txt",      # Apache-2.0: models and en_dict.txt
     f"{PADDLE}/b9da97bea9db53ea5cbb1c32bf5c53b9d09be7b1/LICENSE",
     "3840c5c0c61c294264d2dd77b8777be6ddd90121ef4e0e64abcd22edea581d6e"),
    ("licenses/RapidOCR-LICENSE.txt",       # Apache-2.0: the ONNX conversions
     "https://raw.githubusercontent.com/RapidAI/RapidOCR/ece63a28fe00c36b0872325068774fbc1d189b09/LICENSE",
     "3e0af25fdd06aa9586ae97adb00ea927ebe5a3805ac77d2d3a81ce5f55693333"),
]
OUTPUT_DIRS = ("bin", "models", "licenses")
STAMP = "fetched.json"


def die(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(1)


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def has(path: str, sha256: str) -> bool:
    return os.path.isfile(path) and sha256_of(path) == sha256


def download(url: str, sha256: str, path: str) -> None:
    if has(path, sha256):
        print(f"ok        {os.path.basename(path)}")
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    part = path + ".part"
    print(f"download  {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "makine-live-fetch_ocr"})
    h = hashlib.sha256()
    with urllib.request.urlopen(request, timeout=60) as response, open(part, "wb") as out:
        for chunk in iter(lambda: response.read(1 << 20), b""):
            h.update(chunk)
            out.write(chunk)
    if h.hexdigest() != sha256:
        os.remove(part)
        die(f"{url}: sha256 {h.hexdigest()} is not the pinned {sha256}")
    os.replace(part, path)


def extract(archive: str, member: str, target: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        seven = shutil.which("7z") or shutil.which("7za")
        if not seven and os.path.isfile(r"C:\Program Files\7-Zip\7z.exe"):
            seven = r"C:\Program Files\7-Zip\7z.exe"
        if seven:
            subprocess.run([seven, "x", "-y", f"-o{tmp}", archive], check=True, stdout=subprocess.DEVNULL)
        else:
            try:
                import py7zr
            except ImportError:
                die("7-Zip is not on PATH: install it or run `pip install py7zr`")
            with py7zr.SevenZipFile(archive) as z:
                z.extractall(tmp)
        source = os.path.join(tmp, *member.split("/"))
        if not os.path.isfile(source):
            die(f"{member} is not in {os.path.basename(archive)}")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(source, target)


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", default=os.path.join(here, "..", ".cache", "ocr"))
    args = parser.parse_args()
    cache = os.path.abspath(args.cache)

    # A pin that changed leaves no stale file behind, and no stamp until all is in place.
    if os.path.exists(os.path.join(cache, STAMP)):
        os.remove(os.path.join(cache, STAMP))
    wanted = {dest: sha for dest, _, sha in FILES}
    wanted[RUNTIME["dest"]] = RUNTIME["dest_sha256"]
    for sub in OUTPUT_DIRS:
        for root, _, names in os.walk(os.path.join(cache, sub)):
            for name in names:
                rel = os.path.relpath(os.path.join(root, name), cache).replace(os.sep, "/")
                if rel not in wanted:
                    os.remove(os.path.join(root, name))

    for dest, url, sha in FILES:
        download(url, sha, os.path.join(cache, dest))

    runtime = os.path.join(cache, RUNTIME["dest"])
    if has(runtime, RUNTIME["dest_sha256"]):
        print(f"ok        {os.path.basename(runtime)}")
    else:
        archive = os.path.join(cache, "downloads", os.path.basename(RUNTIME["url"]))
        download(RUNTIME["url"], RUNTIME["sha256"], archive)
        extract(archive, RUNTIME["member"], runtime)
        if not has(runtime, RUNTIME["dest_sha256"]):
            die(f"{RUNTIME['member']} does not have the pinned sha256")

    with open(os.path.join(cache, STAMP), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"files": wanted}, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"OCR runtime ready in {cache}")


if __name__ == "__main__":
    main()

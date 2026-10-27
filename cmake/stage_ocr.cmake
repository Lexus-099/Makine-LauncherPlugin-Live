# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.
#
# Copies what tools/fetch_ocr.py downloaded (bin/, models/, licenses/) into the
# plugin package. Runs at build time, so a fetch after configuring still counts:
#   cmake -DSRC=<LIVE_OCR_DIR> -DDST=<package dir> [-DREQUIRE=ON] -P stage_ocr.cmake

if(NOT EXISTS "${SRC}/fetched.json")
    set(msg "OCR runtime not fetched into ${SRC}: run `python tools/fetch_ocr.py` and build again. "
            "Without it the plugin refuses to start.")
    if(REQUIRE)
        message(FATAL_ERROR ${msg})
    endif()
    message(WARNING ${msg})
    return()
endif()

foreach(dir bin models licenses)
    file(COPY "${SRC}/${dir}" DESTINATION "${DST}")
endforeach()

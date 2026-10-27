# PlatformIO compiles Edge Impulse's vendored ESP-DSP .S files with
# assembler-with-cpp, but the Arduino framework only injects the ESP32-S3
# Xtensa include prefix into C/C++ flags. Without the same prefix, those
# assembly units fail on `#include_next <xtensa/config/core-isa.h>`.

Import("env")

import os

board = env.BoardConfig()
mcu = board.get("build.mcu", "").lower()
libs_dir = env.PioPlatform().get_package_dir(
    "framework-arduinoespressif32-%s-libs" % mcu
)
prefix = os.path.join(libs_dir, "include") + os.sep
env.Append(
    ASPPFLAGS=[
        "-iprefix",
        prefix,
        "-iwithprefixbefore",
        "xtensa/%s/include" % mcu,
    ]
)

#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build the Pocket AMP hardware test: examples/pa-hwtest/build/pa-hwtest.bin (an app.bin; nothing in it writes the flash).
#   JIELI_TOOLCHAIN=~/.fm1/jieli/toolchain examples/pa-hwtest/build.sh
set -e
TC="${JIELI_TOOLCHAIN:?JIELI_TOOLCHAIN must point at the JieLi Linux toolchain}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
O="$HERE/build"
rm -rf "$O"; mkdir -p "$O"
CC="$TC/pi32v2/bin/clang -target pi32v2"
$CC -c "$ROOT/chip/crt0.S" -o "$O/crt0.o"
$CC -c "$ROOT/chip/fm1_vec.S" -o "$O/vec.o"
$CC -c "$ROOT/chip/fm1_isr.S" -o "$O/isr.o"
$CC -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function \
    -I"$ROOT/chip" -I"$ROOT/boards/pocketamp" -I"$ROOT/boards/fm1" -c "$HERE/pa_hwtest.c" -o "$O/pa_hwtest.o"
"$TC/pi32v2/bin/ld" -T "$ROOT/examples/hwtest/app.ld" "$O/crt0.o" "$O/vec.o" "$O/isr.o" "$O/pa_hwtest.o" -o "$O/pa_hwtest.elf"
"$TC/common/bin/objcopy" -O binary -j .text "$O/pa_hwtest.elf" "$O/text.bin"
"$TC/common/bin/objcopy" -O binary -j .data "$O/pa_hwtest.elf" "$O/data.bin"
"$TC/common/bin/objcopy" -O binary -j .ram_text "$O/pa_hwtest.elf" "$O/rt.bin"
"$TC/common/bin/objdump" -t "$O/pa_hwtest.elf" > "$O/syms.txt"
python3 - "$O" <<'PY'
import re, sys
out = sys.argv[1]
syms = open(f"{out}/syms.txt").read()
sym = lambda n: int(re.search(r"^([0-9a-f]+) .*\s" + n + "$", syms, re.M).group(1), 16)
assert sym("_start") == 0x02000120, hex(sym("_start"))
img = bytearray(open(f"{out}/text.bin", "rb").read())
for at, part in sorted([(sym("_rt_load"), open(f"{out}/rt.bin", "rb").read()), (sym("_data_load"), open(f"{out}/data.bin", "rb").read())]):
    if part:
        assert at - 0x02000120 >= len(img), "sections overlap"
        img += b"\xff" * (at - 0x02000120 - len(img)) + part
img += b"\xff" * (-len(img) % 4)
open(f"{out}/pa-hwtest.bin", "wb").write(img)
print(f"pa-hwtest.bin: {len(img)} bytes")
PY

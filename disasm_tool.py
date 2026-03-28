#!/usr/bin/env python3
"""
Disassemble x86-64 machine code from hex DWORD dumps.
Module-relative base address: 0x1F08D9
Module base: 0x140000000
"""
import struct
import sys

# The DWORD data, 8 rows of 16 DWORDs each
dword_rows = [
    # +000
    [0x00107B80, 0x3E804A74, 0x490B7400, 0x8B48D48B, 0xF491E8CB, 0x7B80FFFF, 0x34740010, 0x18838B4C,
     0xF3000002, 0x209B100F, 0x48000002, 0x33104B8D, 0x3331E8D2, 0x0F660021, 0x5B0FF06E, 0x4115E8F6],
    # +040
    [0x0FF30004, 0x0FF3F05E, 0x0110B311, 0x0AEB0000, 0x011083C7, 0x00000000, 0x3E800000, 0x4C337400,
     0x0218838B, 0xF3000002, 0x209B100F, 0x48000002, 0x33104B8D, 0x3331E8D2, 0x0F660021, 0x5B0FF06E],
    # +080
    [0x000440D3, 0xF05E0FF3, 0xB3110FF3, 0x00000214, 0x83C70AEB, 0x00000214, 0x00000000, 0x48C38B4C,
     0x8B49D78B, 0x0C7DE8CF, 0x8B4C0000, 0xD78B48C3, 0xF3CF8B49, 0x0843110F, 0x0007BAE8, 0xC38B4C00],
    # +0C0
    [0x49D78B48, 0x0FF3CF8B, 0xE80C4311, 0x00000DF7, 0x245C8D4C, 0x74280F40, 0x280F3024, 0x4920247C,
     0x49305B8B, 0x49406B8B, 0x4948738B, 0x5F41E38B, 0x5D415E41, 0xC35F5C41, 0x57001F0F, 0x06001F07],
    # +100
    [0x15001F07, 0x24001F07, 0x2B001F07, 0x32001F07, 0x39001F07, 0x48001F07, 0x63001F07, 0xB5001F08,
     0x2A001F08, 0xD9001F08, 0x7F001F08, 0x7F001F08, 0x7F001F08, 0x9A001F08, 0x9A001F08, 0x9A001F08],
    # +140
    [0xCC001F08, 0x48CCCCCC, 0x08245C89, 0x247C8948, 0x8D485510, 0x48A9246C, 0x00E0EC81, 0x8B480000,
     0xE8D98BFA, 0x0001BC5F, 0x850FC085, 0x000001CC, 0x29A1158B, 0x8D4C0141, 0x8B487745, 0xF185E8CF],
    # +180
    [0xB60F0026, 0xC9847A4D, 0x01AE840F, 0x0FF30000, 0x45291510, 0xB60F0052, 0x0F667745, 0xB60FC06E,
     0x5B0F7845, 0x590FF3C0, 0x110FF3C2, 0x0F669745, 0xB60FC06E, 0x0F667945, 0x5B0FC86E, 0xC1B60FC0],
    # +1C0
    [0xC2590FF3, 0xF3C95B0F, 0x9B45110F, 0xC06E0F66, 0xC0058B48, 0xF304B2E1, 0xF3CA590F, 0x9F4D110F,
     0xF3C05B0F, 0xF3C2590F, 0xA345110F, 0x00107883, 0xCB8B4174, 0x20EADEE8, 0x74C08500, 0x100FF336],
]

def dwords_to_bytes(dword_list):
    """Convert list of DWORDs (LE) to byte array."""
    result = bytearray()
    for dw in dword_list:
        result.extend(struct.pack('<I', dw))
    return result

# Convert all rows to a single byte stream
all_bytes = bytearray()
for row in dword_rows:
    all_bytes.extend(dwords_to_bytes(row))

print(f"Total bytes: {len(all_bytes)}")
print()

# Print hex dump for verification
for i in range(0, len(all_bytes), 16):
    hex_str = ' '.join(f'{b:02X}' for b in all_bytes[i:i+16])
    print(f"+{i:03X}: {hex_str}")

# Write binary file
with open(r'c:\Games\Call Of Duty - Ghosts\KOR_PATCH\hud_code.bin', 'wb') as f:
    f.write(all_bytes)
print(f"\nWrote {len(all_bytes)} bytes to hud_code.bin")

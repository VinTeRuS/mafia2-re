#!/usr/bin/env python3
# dis.py 0xVA [bytes] : disassemble the exe at a guest address
import sys, pefile, capstone, os
pe = pefile.PE(os.path.expanduser(os.environ.get('DISASM_PE', '~/swbf2-re-data/game/BattlefrontII.exe')))
b = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
a = int(sys.argv[1], 16); n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x80
for i in md.disasm(pe.get_data(a - b, n), a): print(hex(i.address), i.mnemonic, i.op_str)

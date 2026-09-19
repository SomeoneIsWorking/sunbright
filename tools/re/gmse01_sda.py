"""The GMSE01 small-data bases, in one place.

PowerPC's ABI reserves r13 and r2 for the small-data areas, and every reference to a global in
`.sdata`, `.sbss`, `.sdata2` or `.sbss2` is a displacement from one of them rather than an address
built with `lis`. Two tools need the bases -- `dol_sda.py`, which reads what a function touches, and
`dataref.py`, which reads what touches an address -- and a second copy of a number that the linker
chose is a second thing to get wrong.

The values come from the image's own prologue: `__start` loads r13 and r2 from `_SDA_BASE_` and
`_SDA2_BASE_`, which the GMSE01 map places at these addresses.
"""

SDA_BASE = 0x804141C0   # r13 -- .sdata/.sbss: engine singletons and game state
SDA2_BASE = 0x80416BA0  # r2  -- .sdata2/.sbss2: float and double constants

# The whole small-data window, used to tell "no reference exists" from "the reference is one this
# scan does not model".
SDA_REGION_START = 0x80400000
SDA_REGION_END = 0x80420000

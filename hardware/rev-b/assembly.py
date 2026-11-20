"""The two flash populations for the same Rev B PCB."""

DEFAULT_MCU = "RP2354B"
EXTERNAL_VARIANT = "External-16MiB"
EXTERNAL_MCU = "RP2350B"
EXTERNAL_DESCRIPTION = "RP2350B with a fitted W25Q128JVSIQ primary flash"

EXTERNAL_FLASH_PARTS = ("U303", "C302", "R310")
OVERRIDES = {
    "U301": {"fields": {"Value": EXTERNAL_MCU, "MPN": EXTERNAL_MCU}},
    **{ref: {"dnp": False} for ref in EXTERNAL_FLASH_PARTS},
}

/* SPDX-License-Identifier: BSD-2-Clause-Patent */

DefinitionBlock ("", "DSDT", 2, "LIUQIN", "LIUQIN", 2)
{
    Scope (\_SB)
    {
        Device (CPU0) { Name (_HID, "ACPI0007") Name (_UID, 0) Name (_STA, 0x0F) }
        Device (CPU1) { Name (_HID, "ACPI0007") Name (_UID, 1) Name (_STA, 0x0F) }
        Device (CPU2) { Name (_HID, "ACPI0007") Name (_UID, 2) Name (_STA, 0x0F) }
        Device (CPU3) { Name (_HID, "ACPI0007") Name (_UID, 3) Name (_STA, 0x0F) }
        Device (CPU4) { Name (_HID, "ACPI0007") Name (_UID, 4) Name (_STA, 0x0F) }
        Device (CPU5) { Name (_HID, "ACPI0007") Name (_UID, 5) Name (_STA, 0x0F) }
        Device (CPU6) { Name (_HID, "ACPI0007") Name (_UID, 6) Name (_STA, 0x0F) }
        Device (CPU7) { Name (_HID, "ACPI0007") Name (_UID, 7) Name (_STA, 0x0F) }

        Include ("I2cControllers.aslinc")
        Include ("BoardDevices.aslinc")
        Include ("PlatformInputs.aslinc")
        Include ("PlatformPower.aslinc")
    }
}

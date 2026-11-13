/* SPDX-License-Identifier: BSD-2-Clause-Patent */

DefinitionBlock ("", "SSDT", 2, "LIUQIN", "LIUQIO", 2)
{
    External (\_SB.UHP0, DeviceObj)
    External (\_SB.USP0, DeviceObj)
    External (\_SB.PGLO, DeviceObj)
    Scope (\_SB)
    {
        Include ("Ufs.aslinc")
        Include ("UsbHost.aslinc")
    }
}

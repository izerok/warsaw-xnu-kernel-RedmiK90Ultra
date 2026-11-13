# APRIORI.inc -- adapted from annibalePkg (Mu pin 90721af9): driver load
# order list with Binaries/annibale paths rewritten to Binaries/warsaw.
# The Binaries/warsaw prebuilt drivers do not exist yet (no warsaw dir in
# Project-Silicium/Device-Binaries) -- hard prerequisite for any FD build.
# Entry set itself is [S2]: same SoC/platform, unverified on warsaw.
APRIORI DXE {
  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
  INF QcomPkg/Drivers/IortDtDxe/IortDtDxe.inf

  INF Binaries/warsaw/QcomPkg/Drivers/EnvDxe/EnvDxeEnhanced.inf

  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF Binaries/warsaw/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/IPCCDxe/IPCCDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/MailboxDxe/MailboxDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/QcomScmiDxe/QcomScmiDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/ICBDxe/ICBDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
  INF Binaries/warsaw/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
  INF Binaries/warsaw/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/QupDxe/QupDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf

  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf

  INF Binaries/warsaw/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf

  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  INF Binaries/warsaw/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/UFSDxe/UFSDxe.inf

  INF FatPkg/EnhancedFatDxe/Fat.inf

  INF Binaries/warsaw/QcomPkg/Drivers/TzDxe/TzDxeLA.inf

  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf

  INF Binaries/warsaw/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
  # PHASE9 trial-build local fix (Mu-tree copy only; fork scaffold needs the same removal):
  # OplusProject.inf is absent from warsaw's stock FV (Oplus driver, dodge leftover) -- see PHASE9 §2.
  #INF Binaries/warsaw/QcomPkg/Drivers/OcdtDxe/OplusProject.inf
  INF Binaries/warsaw/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/PILDxe/PILDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf

  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  INF Binaries/warsaw/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/PmicGlinkDxe/PmicGlinkDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
  INF Binaries/warsaw/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/XhciDxe/XhciDxe.inf

  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  INF Binaries/warsaw/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/TsensDxe/TsensDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/warsaw/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

!if $(USE_CUSTOM_DRIVER) == 1
  INF Binaries/warsaw/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
!endif
  INF Binaries/warsaw/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/UCDxe/UCDxe.inf
  INF Binaries/warsaw/QcomPkg/Drivers/RNGDxe/RngDxe.inf

  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

  INF Binaries/warsaw/QcomPkg/Drivers/RscDxe/RscDxe.inf
}

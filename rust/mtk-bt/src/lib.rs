//! MediaTek USB Bluetooth core: chip identification and firmware selection.
//! Patch download (WMT) and the HCI fixes for bluetoothd follow in phase 1.
#![cfg_attr(not(test), no_std)]

pub mod chip;

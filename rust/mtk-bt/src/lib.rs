//! MediaTek USB Bluetooth core (MT7921 / MT7922): chip identification,
//! ROM patch download over WMT, and the HCI rewrites that make bluetoothd
//! work with these controllers.
#![cfg_attr(not(test), no_std)]

pub mod chip;
pub mod fw;
pub mod hci;
pub mod le_features;
pub mod setup;
pub mod wmt;

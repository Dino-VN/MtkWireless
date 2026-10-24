//! MediaTek Wi-Fi core.
//!
//! Hardware is described by [`chip::ChipDesc`] entries grouped by
//! [`chip::Family`]. Code that differs between families (MCU command set,
//! DMA layout, register map) lives under a family module; MT7921/MT7922
//! (Connac2) is the first one. A new chip of an existing family is a table
//! entry, a new family is a new module.
#![cfg_attr(not(test), no_std)]

pub mod awdl;
pub mod chip;
pub mod connac2;
pub mod sae;
pub mod scan;

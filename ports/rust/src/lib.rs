//! Relais — the actuator's arbiter, reimplemented in Rust (mission M32, idea 74).
//!
//! The point of this crate is NOT a second product: it is a demonstration that
//! the mandate rules of `core/src/arbiter.c` are a specification, not a C
//! implementation detail. Same structures, same field names, same order of
//! decisions, no allocation, no clock, no I/O, no `unsafe`.
//!
//! It is confronted with the C arbiter tick by tick by
//! `ports/rust/difftest/run_difftest.py`, over the same JSON-lines protocol that
//! `host/difftest/bridge.c` already speaks (so the C side is the untouched
//! reference binary `build/relais-bridge`).
//!
//! Requirement IDs anchored here: MAN-02, MAN-03, MAN-04, MAN-05, MAN-06,
//! MAN-10, REL-05, REL-06, DET-01, DET-03, DET-09, REP-05, DEG-01, DEG-02,
//! DEG-05, SEC-04, SEC-06, ACT-02, ACT-03, ACT-05, I1, I6.
#![cfg_attr(not(feature = "std"), no_std)]
#![forbid(unsafe_code)]
#![deny(missing_docs)]

pub mod arbiter;
pub mod params;
pub mod step;
pub mod types;

#[cfg(feature = "std")]
pub mod json;

pub use arbiter::{Arb, ArbOut};
pub use params::Params;

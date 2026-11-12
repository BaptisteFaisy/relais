//! Protocol parameters, mirroring `core/src/params.c` (spec 6.3). Only the
//! fields the arbiter reads are here; the control-law and slow-motion fields
//! belong to the node side, which this port does not reimplement.

use crate::types::TESTAMENT_MAX;

/// The parameter block of spec 6.3.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Params {
    /// T, the tick period in ms (CMD-01).
    pub period_ms: u32,
    /// p, consecutive misses tolerated (MAN-04).
    pub patience: u8,
    /// N, horizon of the will in ticks (ACT-02).
    pub testament_len: u8,
    /// Matching ticks before a second is certified (REP-05).
    pub certify_ticks: u8,
    /// STATUT silence after which a node deposes itself (MAN-09).
    pub bail_ticks: u8,
    /// SEC-06: demandes accepted per window.
    pub request_burst: u8,
    /// Ticks in that window.
    pub request_window: u16,
    /// Order/shadow agreement tolerance, full-scale fraction (DET-09).
    pub epsilon: f32,
    /// Continuity tolerance at handover, full-scale fraction (REL-03).
    pub delta: f32,
    /// DEG-08: widened band for a dissimilar second; 0 disables it.
    pub dissimilar_epsilon: f32,
    /// DEG-05: ticks without redundancy before a stop request.
    pub degraded_ticks: u32,
    /// ACT-05 command bound.
    pub max_amplitude: f32,
    /// ACT-05 bound on the change per tick.
    pub max_slew: f32,
}

impl Default for Params {
    /// The documented defaults, identical to `relais_params_default()`.
    fn default() -> Self {
        Self {
            period_ms: 5,
            patience: 1,
            testament_len: TESTAMENT_MAX as u8,
            certify_ticks: 3,
            bail_ticks: 4,
            request_burst: 4,
            request_window: 4,
            epsilon: 0.005,
            delta: 0.02,
            dissimilar_epsilon: 0.0,
            degraded_ticks: 6000,
            max_amplitude: 1.0,
            max_slew: 0.25,
        }
    }
}

impl Params {
    /// The consistency rule of spec 6.3: the will must cover the handover even
    /// when three demandes are lost.
    #[must_use]
    pub fn valid(&self) -> bool {
        self.period_ms >= 1
            && self.period_ms <= 50
            && self.patience <= 3
            && self.testament_len >= 1
            && self.testament_len <= TESTAMENT_MAX as u8
            && self.testament_len >= self.patience + 2 + 3
            && self.certify_ticks >= 1
            && self.certify_ticks <= TESTAMENT_MAX as u8
            && self.bail_ticks >= 2
            && self.epsilon > 0.0
            && self.delta > 0.0
            && self.delta <= 0.5
            && self.dissimilar_epsilon >= 0.0
            && self.dissimilar_epsilon < 1.0
    }

    /// Step of one tick in seconds: the protocol is tick based (CMD-07), the
    /// plant model is not.
    #[must_use]
    pub fn step_s(&self) -> f64 {
        f64::from(self.period_ms) / 1000.0
    }
}

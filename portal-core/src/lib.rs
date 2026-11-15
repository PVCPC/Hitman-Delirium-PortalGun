//! Delirium's Portal 2 portal code, shared with the Hitman plugin: the pair maths, Portal 2's placement rules and
//! shot trace, the air funnel, finding the user's own Portal 2 / GTA IV installs and reading Portal 2's VPKs.
//! Metres, Z up (Delirium's and Hitman's axes). The C ABI is in `ffi` (header: `include/delirium_portal.h`).
use glam::*;

pub mod ffi;
pub mod funnel;
pub mod games;
pub mod place;
pub mod vpk;

/// Half the opening (Portal 2's 64 x 112 units).
pub const HALF: Vec2 = Vec2::new(32.0 * 0.0254, 56.0 * 0.0254);
/// The part of the oval inside the burning band: the hole things go through.
pub const OPEN: f32 = 0.83;

/// A portal: centre, rotation (local +Z out of the surface, +Y its up).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Frame {
    pub at: Vec3,
    pub rot: Quat,
}

impl Frame {
    pub fn new(at: Vec3, normal: Vec3, up_hint: Vec3) -> Option<Frame> {
        let n = normal.try_normalize()?;
        let up = (up_hint - n * up_hint.dot(n)).try_normalize()?;
        Some(Frame { at, rot: Quat::from_mat3(&Mat3::from_cols(up.cross(n), up, n)).normalize() })
    }
    pub fn local(&self, p: Vec3) -> Vec3 {
        self.rot.inverse() * (p - self.at)
    }
    pub fn normal(&self) -> Vec3 {
        self.rot * Vec3::Z
    }
}

/// In `a`, out of `b`: glued back to back (180 degrees about local Y). The turn, and where a point goes.
pub fn through(a: &Frame, b: &Frame) -> (Quat, impl Fn(Vec3) -> Vec3) {
    const FLIP: Quat = Quat::from_xyzw(0.0, 1.0, 0.0, 0.0);
    let (a, b) = (*a, *b);
    ((b.rot * FLIP * a.rot.inverse()).normalize(), move |p| b.at + b.rot * (FLIP * a.local(p)))
}

/// Rectangle portals instead of ovals (the Playroom's Shape; `dp_set_rect`).
pub static RECT: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Inside the hole (oval, or rectangle with `RECT`), in a portal's own axes, for an opening of half size `half`.
pub fn in_hole(l: Vec3, half: Vec2) -> bool {
    let q = (l.truncate() / (half * OPEN)).abs();

    if RECT.load(std::sync::atomic::Ordering::Relaxed) { q.max_element() < 1.0 } else { q.length_squared() < 1.0 }
}

/// Through an open portal: `p` (its local z a frame ago `last`) crossed the plane inside the opening.
pub fn crossed(f: &Frame, last: f32, p: Vec3, half: Vec2) -> bool {
    let l = f.local(p);
    last >= 0.0 && l.z < 0.0 && l.z > -1.0 && in_hole(l, half)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn through_walls_keeps_speed_and_turns_it() {
        // Blue on a wall facing +X, orange on the floor facing up.
        let a = Frame::new(Vec3::new(0.0, 0.0, 1.5), Vec3::X, Vec3::Z).unwrap();
        let b = Frame::new(Vec3::new(10.0, 0.0, 0.0), Vec3::Z, Vec3::X).unwrap();
        let (turn, map) = through(&a, &b);
        // Going into blue (moving -X) comes out of orange moving up, same speed.
        let v = turn * Vec3::new(-5.0, 0.0, 0.0);
        assert!((v - Vec3::new(0.0, 0.0, 5.0)).length() < 1e-4, "{v}");
        // Blue's centre maps to orange's centre.
        assert!((map(a.at) - b.at).length() < 1e-4);
        // Crossing: in front last frame, just behind and inside now.
        assert!(crossed(&a, 0.1, Vec3::new(-0.05, 0.0, 1.5), HALF));
        assert!(!crossed(&a, 0.1, Vec3::new(-0.05, 3.0, 1.5), HALF));
    }
}

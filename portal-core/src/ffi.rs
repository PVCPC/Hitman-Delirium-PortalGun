//! The C ABI the Hitman plugin calls (header: `include/delirium_portal.h`). Vectors are float[3], rotations
//! quaternions x, y, z, w. The world is the plugin's: placement asks it for rays through `DpRay`.
use std::ffi::{c_char, c_void, CStr};
use std::path::Path;

use glam::*;

use crate::games::{self, Game};
use crate::{place, Frame};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DpFrame {
    pub at: [f32; 3],
    pub rot: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DpHit {
    pub point: [f32; 3],
    /// Facing the ray's start.
    pub normal: [f32; 3],
    pub distance: f32,
}

/// Nearest solid hit along `origin + t * dir`, `0 <= t <= max` (`dir` unit length): true and `hit` filled, or false.
/// Rays should miss characters, props and the holes the portals cut.
pub type DpRay = extern "C" fn(ctx: *mut c_void, origin: *const [f32; 3], dir: *const [f32; 3], max: f32, hit: *mut DpHit) -> bool;

impl From<DpFrame> for Frame {
    fn from(f: DpFrame) -> Frame {
        Frame { at: Vec3::from(f.at), rot: Quat::from_array(f.rot).normalize() }
    }
}
impl From<Frame> for DpFrame {
    fn from(f: Frame) -> DpFrame {
        DpFrame { at: f.at.into(), rot: f.rot.into() }
    }
}

struct CWorld(DpRay, *mut c_void);

impl place::World for CWorld {
    fn ray(&self, origin: Vec3, dir: Vec3, max: f32) -> Option<place::Hit> {
        let mut h = DpHit::default();
        (self.0)(self.1, &origin.to_array(), &dir.to_array(), max, &mut h)
            .then(|| place::Hit { point: Vec3::from(h.point), normal: Vec3::from(h.normal), distance: h.distance })
    }
}

static HALF: std::sync::Mutex<Vec2> = std::sync::Mutex::new(crate::HALF);

fn half() -> Vec2 {
    *HALF.lock().unwrap()
}

unsafe fn v3(p: *const [f32; 3]) -> Vec3 {
    Vec3::from(*p)
}

/// The opening's half size (m); Portal 2's to begin with.
#[no_mangle]
pub extern "C" fn dp_set_half(w: f32, h: f32) {
    *HALF.lock().unwrap() = Vec2::new(w, h).clamp(Vec2::splat(0.2), Vec2::splat(5.0));
}

/// A shot from `origin` along `dir` for `range` m, placed by Portal 2's rules. `other`: the other open portal (or
/// null). `linked`: both portals [blue, orange] when a shot may pass through them (or null). Returns 0 placed
/// (`out` filled, 1 cm off the surface), 1 hit nothing, 2 doesn't fit, 3 overlaps the other. `hit` (may be null)
/// gets where the shot ended, for the bad-surface sparks.
#[no_mangle]
pub unsafe extern "C" fn dp_shoot(ray: DpRay, ctx: *mut c_void, origin: *const [f32; 3], dir: *const [f32; 3], range: f32,
    other: *const DpFrame, linked: *const DpFrame, out: *mut DpFrame, hit: *mut DpHit) -> i32 {
    let w = CWorld(ray, ctx);
    let windows: Vec<place::Window> = if linked.is_null() {
        vec![]
    } else {
        let (a, b): (Frame, Frame) = ((*linked).into(), (*linked.add(1)).into());
        vec![place::Window { frame: a, exit: b }, place::Window { frame: b, exit: a }]
    };
    let others: Vec<Frame> = other.as_ref().map(|o| (*o).into()).into_iter().collect();
    let trace = place::trace_shot(&w, v3(origin), v3(dir), range, half(), &windows);
    if let (Some(h), Some(out)) = (trace.hit, hit.as_mut()) {
        *out = DpHit { point: h.point.into(), normal: h.normal.into(), distance: h.distance };
    }
    let Some(h) = trace.hit else { return 1 };
    match place::place(&w, h, trace.dir, half(), &others) {
        Ok(f) => {
            *out = Frame { at: f.at + f.normal() * 0.01, ..f }.into();
            0
        }
        Err(place::Fail::NoHit) => 1,
        Err(place::Fail::DoesNotFit) => 2,
        Err(place::Fail::Overlap) => 3,
    }
}

/// Something at `pos` turned `rot` moving `vel` (each may be null) put through `from` and out of `to`.
#[no_mangle]
pub unsafe extern "C" fn dp_through(from: *const DpFrame, to: *const DpFrame, pos: *mut [f32; 3], vel: *mut [f32; 3], rot: *mut [f32; 4]) {
    let (turn, map) = crate::through(&(*from).into(), &(*to).into());
    if let Some(p) = pos.as_mut() {
        *p = map(Vec3::from(*p)).into();
    }
    if let Some(v) = vel.as_mut() {
        *v = (turn * Vec3::from(*v)).into();
    }
    if let Some(r) = rot.as_mut() {
        *r = (turn * Quat::from_array(*r)).normalize().into();
    }
}

/// How far `p` is out in front of the portal (m; below 0 behind it). Keep it each frame for `dp_crossed`.
#[no_mangle]
pub unsafe extern "C" fn dp_local_z(f: *const DpFrame, p: *const [f32; 3]) -> f32 {
    Frame::from(*f).local(v3(p)).z
}

/// `p` (`last` = its `dp_local_z` a frame ago) went in through the opening this frame.
#[no_mangle]
pub unsafe extern "C" fn dp_crossed(f: *const DpFrame, last: f32, p: *const [f32; 3]) -> bool {
    crate::crossed(&(*f).into(), last, v3(p), half())
}

/// Portal 2's air funnel: the velocity a falling body (centre `centre`, velocity `vel`) should have after `dt`
/// toward a linked floor or ceiling portal among `portals[n]`; `vel` unchanged when none catches it.
#[no_mangle]
pub unsafe extern "C" fn dp_funnel(portals: *const DpFrame, n: usize, gravity: f32, centre: *const [f32; 3], vel: *mut [f32; 3], dt: f32) {
    let ps: Vec<Frame> = std::slice::from_raw_parts(portals, n).iter().map(|&f| f.into()).collect();
    *vel = crate::funnel::steer(&ps, gravity, v3(centre), Vec3::from(*vel), dt).into();
}

/// Out of a floor or ceiling portal near enough straight up or down: the sideways speed to take off (else 0).
#[no_mangle]
pub unsafe extern "C" fn dp_exit_gimme(exit: *const DpFrame, vel: *const [f32; 3], out: *mut [f32; 3]) {
    *out = crate::funnel::exit_gimme(&(*exit).into(), v3(vel)).into();
}

unsafe fn str_arg<'a>(s: *const c_char) -> Option<&'a str> {
    (!s.is_null()).then(|| CStr::from_ptr(s).to_str().ok()).flatten()
}

/// The user's own install of `game` (0 Portal 2, 1 GTA IV): saved before in `config_dir` or by Delirium, else found
/// in their Steam libraries (and saved). Writes it to `out` (NUL-ended, `cap` bytes); its length, 0 when not found.
#[no_mangle]
pub unsafe extern "C" fn dp_find_game(game: i32, config_dir: *const c_char, out: *mut c_char, cap: usize) -> usize {
    let (Some(g), Some(dir)) = (Game::from_i32(game), str_arg(config_dir)) else { return 0 };
    let Some(path) = games::find(g, Path::new(dir)) else { return 0 };
    if path.len() + 1 > cap {
        return 0;
    }
    std::ptr::copy_nonoverlapping(path.as_ptr(), out as *mut u8, path.len());
    *out.add(path.len()) = 0;
    path.len()
}

/// A folder the user gave for `game`: saved to `config_dir` and true if it really is that game.
#[no_mangle]
pub unsafe extern "C" fn dp_set_game(game: i32, config_dir: *const c_char, path: *const c_char) -> bool {
    match (Game::from_i32(game), str_arg(config_dir), str_arg(path)) {
        (Some(g), Some(d), Some(p)) => games::save(g, Path::new(d), p),
        _ => false,
    }
}

/// One file out of Portal 2's VPKs (e.g. "models/weapons/w_portalgun.mdl"): its bytes in `*data` / `*len`
/// (free with `dp_free`), or false.
#[no_mangle]
pub unsafe extern "C" fn dp_vpk_read(portal2: *const c_char, path: *const c_char, data: *mut *mut u8, len: *mut usize) -> bool {
    let (Some(game), Some(p)) = (str_arg(portal2), str_arg(path)) else { return false };
    let Some(mut files) = crate::vpk::vpk(game, &[p]) else { return false };
    let b = files.remove(0).into_boxed_slice();
    *len = b.len();
    *data = Box::into_raw(b) as *mut u8;
    true
}

#[no_mangle]
pub unsafe extern "C" fn dp_free(data: *mut u8, len: usize) {
    if !data.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(data, len)));
    }
}

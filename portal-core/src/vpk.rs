//! Reading files out of the user's own Portal 2 VPKs (from Delirium's portal mode).
pub fn vpk(game: &str, want: &[&str]) -> Option<Vec<Vec<u8>>> {
    let dir = format!("{game}/portal2/pak01_dir.vpk");
    let d = std::fs::read(&dir).ok()?;
    let u32_at = |o: usize| d.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()));
    let head = if u32_at(4)? == 2 { 28 } else { 12 };
    let data_at = head + u32_at(8)? as usize;
    let mut p = head;
    let mut found = std::collections::HashMap::new();
    let cs = |p: &mut usize| -> Option<String> {
        let e = *p + d[*p..].iter().position(|&c| c == 0)?;
        let s = String::from_utf8_lossy(&d[*p..e]).into_owned();
        *p = e + 1;
        Some(s)
    };
    loop {
        let ext = cs(&mut p)?;
        if ext.is_empty() {
            break;
        }
        loop {
            let path = cs(&mut p)?;
            if path.is_empty() {
                break;
            }
            loop {
                let name = cs(&mut p)?;
                if name.is_empty() {
                    break;
                }
                let full = format!("{path}/{name}.{ext}");
                let (pre, arc) = (u16::from_le_bytes([d[p + 4], d[p + 5]]) as usize, u16::from_le_bytes([d[p + 6], d[p + 7]]));
                let (off, len) = (u32_at(p + 8)? as u64, u32_at(p + 12)? as usize);
                if want.contains(&full.as_str()) {
                    found.insert(full, (d[p + 18..p + 18 + pre].to_vec(), arc, off, len));
                }
                p += 18 + pre;
            }
        }
    }
    want.iter().map(|w| {
        let (mut out, arc, off, len) = found.remove(*w)?;
        if len > 0 {
            use std::io::{Read, Seek};
            let (f, at) = if arc == 0x7fff { (dir.clone(), off + data_at as u64) } else { (format!("{game}/portal2/pak01_{arc:03}.vpk"), off) };
            let mut f = std::fs::File::open(f).ok()?;
            f.seek(std::io::SeekFrom::Start(at)).ok()?;
            let mut b = vec![0; len];
            f.read_exact(&mut b).ok()?;
            out.extend(b);
        }
        Some(out)
    }).collect()
}


//! Finding the user's own Portal 2 and GTA IV: a folder saved before (ours, or Delirium's own for GTA IV), else
//! every Steam library (Steam's libraryfolders.vdf, as Delirium's portal mode does). Nothing of theirs is shipped.
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Game {
    Portal2,
    Gta4,
}

impl Game {
    pub fn from_i32(i: i32) -> Option<Game> {
        match i {
            0 => Some(Game::Portal2),
            1 => Some(Game::Gta4),
            _ => None,
        }
    }
    fn file(self) -> &'static str {
        match self {
            Game::Portal2 => "portal2_path.txt",
            Game::Gta4 => "game_path.txt",
        }
    }
    /// Its folder under steamapps/common.
    fn steam_dir(self) -> &'static str {
        match self {
            Game::Portal2 => "Portal 2",
            Game::Gta4 => "Grand Theft Auto IV/GTAIV",
        }
    }
    pub fn is_install(self, dir: &str) -> bool {
        let d = Path::new(dir);
        match self {
            Game::Portal2 => d.join("portal2/pak01_dir.vpk").is_file(),
            Game::Gta4 => d.join("GTAIV.exe").is_file() && d.join("pc").is_dir(),
        }
    }
}

/// Delirium's own saved folders (%APPDATA%/.delirium): set up Delirium once and this finds the same games.
fn delirium(game: Game) -> Option<PathBuf> {
    std::env::var_os("APPDATA").map(|d| Path::new(&d).join(".delirium").join(game.file()))
}

fn saved(game: Game, config_dir: &Path) -> Option<String> {
    [Some(config_dir.join(game.file())), delirium(game)].into_iter().flatten()
        .filter_map(|p| std::fs::read_to_string(p).ok()).map(|t| t.trim().to_string()).find(|g| game.is_install(g))
}

fn steam(game: Game) -> Option<String> {
    let roots = ["C:/Program Files (x86)/Steam", "C:/Program Files/Steam"];
    let vdf: String = roots.iter().filter_map(|s| std::fs::read_to_string(format!("{s}/steamapps/libraryfolders.vdf")).ok()).collect();
    let libs = vdf.lines().filter_map(|l| l.trim().strip_prefix("\"path\"")).map(|p| p.trim().trim_matches('"').replace("\\\\", "/"));
    roots.iter().map(|s| s.to_string()).chain(libs).map(|l| format!("{l}/steamapps/common/{}", game.steam_dir())).find(|g| game.is_install(g))
}

/// The game's folder, saving it to `config_dir` when it was found anew.
pub fn find(game: Game, config_dir: &Path) -> Option<String> {
    if let Some(g) = saved(game, config_dir) {
        return Some(g);
    }
    let g = steam(game)?;
    let _ = save(game, config_dir, &g);
    Some(g)
}

/// A folder the user gave (pasted or picked): kept if it really is that game.
pub fn save(game: Game, config_dir: &Path, dir: &str) -> bool {
    let dir = dir.trim().trim_matches('"').replace('\\', "/");
    game.is_install(&dir) && std::fs::create_dir_all(config_dir).is_ok() && std::fs::write(config_dir.join(game.file()), dir).is_ok()
}

#[cfg(test)]
mod tests {
    /// Needs Portal 2 installed: `cargo test -- --ignored`.
    #[test]
    #[ignore]
    fn finds_portal2_and_reads_the_gun() {
        let dir = std::env::temp_dir().join("dp-games-test");
        let p2 = super::find(super::Game::Portal2, &dir).expect("Portal 2 not found");
        let mdl = crate::vpk::vpk(&p2, &["models/weapons/w_portalgun.mdl"]).expect("gun not in the VPKs");
        assert_eq!(&mdl[0][..4], b"IDST");
        println!("portal2 {p2}, gta4 {:?}", super::find(super::Game::Gta4, &dir));
    }
}

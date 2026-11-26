use std::{env, ffi::OsString, path::PathBuf, process::{Command, ExitCode}};
use std::os::unix::process::CommandExt;

const HELP: &str = "Kawari P0.3a\nUsage: vtuber-studio [--fps 30|60|120|144|0] [--size WIDTHxHEIGHT] [--offscreen] [--broadcast] [--no-preview] [--broadcast-size WIDTHxHEIGHT] [--broadcast-fps 30|60|120|144] [--parameter-input] [--parameter-map FILE] [--stats] [--demo] [--sample | PATH.model3.json]\n--no-preview implies --broadcast; stop with Ctrl+C. Requires a graphical session at startup.\nBroadcast renders at --broadcast-size (default 1920x1080), independently of --size.\n0 = no extra FPS limit (presentation may still limit FPS).\nControls: Up/Down select parameter; Left/Right adjust; R release; P list; WASD move; +/- zoom; Home reset; B background; F2 demo; F3 stats; Esc close.";

#[derive(Debug, PartialEq)]
struct Options { model: Option<PathBuf>, fps: u32, size: Option<(u32,u32)>, offscreen: bool, stats: bool, demo: bool, broadcast: bool, no_preview: bool, broadcast_size: Option<(u32,u32)>, broadcast_fps: u32, parameter_input: bool, parameter_map: Option<PathBuf> }
impl Default for Options { fn default() -> Self { Self { model:None, fps:60, size:None, offscreen:false, stats:false, demo:false, broadcast:false, no_preview:false, broadcast_size:None, broadcast_fps:60, parameter_input:false, parameter_map:None } } }
fn parse_size(s: &str) -> Option<(u32,u32)> {
    let (w,h)=s.split_once('x')?;
    let (w,h)=(w.parse::<u32>().ok()?,h.parse::<u32>().ok()?);
    ((320..=7680).contains(&w) && (320..=7680).contains(&h)).then_some((w,h))
}
fn parse(args: impl IntoIterator<Item=OsString>) -> Result<Option<Options>, String> {
    let mut args = args.into_iter();
    let mut opts = Options::default();
    let mut selected = false;
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--help" | "-h") => return Ok(None),
            Some("--fps") => {
                let value = args.next().ok_or("--fps needs a value")?;
                opts.fps = value.to_str().and_then(|v| v.parse().ok()).filter(|v| matches!(*v, 0 | 30 | 60 | 120 | 144)).ok_or("Invalid FPS (0, 30, 60, 120, 144)")?;
            }
            Some("--size") => { let value=args.next().ok_or("--size needs WIDTHxHEIGHT")?; opts.size=Some(value.to_str().and_then(parse_size).ok_or("Invalid size (320..7680 per dimension)")?); }
            Some("--offscreen") => opts.offscreen=true,
            Some("--broadcast") => { opts.broadcast=true; opts.offscreen=true; }
            Some("--no-preview") => { opts.no_preview=true; opts.broadcast=true; opts.offscreen=true; }
            Some("--broadcast-size") => { let value=args.next().ok_or("--broadcast-size needs WIDTHxHEIGHT")?; opts.broadcast_size=Some(value.to_str().and_then(parse_size).ok_or("Invalid broadcast size")?); }
            Some("--broadcast-fps") => { let value=args.next().ok_or("--broadcast-fps needs a value")?; opts.broadcast_fps=value.to_str().and_then(|v|v.parse().ok()).filter(|v|matches!(*v,30|60|120|144)).ok_or("Invalid broadcast FPS")?; }
            Some("--parameter-input") => opts.parameter_input=true,
            Some("--parameter-map") => { opts.parameter_map=Some(PathBuf::from(args.next().ok_or("--parameter-map needs FILE")?)); opts.parameter_input=true; }
            Some("--stats") => opts.stats=true,
            Some("--demo") => opts.demo=true,
            Some("--sample") if !selected => { selected = true; }
            Some(s) if s.starts_with('-') => return Err(format!("Unknown or duplicate option: {s}")),
            _ if !selected => { opts.model = Some(PathBuf::from(arg)); selected = true; }
            _ => return Err("Specify only one model or --sample".into()),
        }
    }
    Ok(Some(opts))
}
fn run() -> Result<(), String> {
    let Some(opts) = parse(env::args_os().skip(1))? else { println!("{HELP}"); return Ok(()); };
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    if opts.broadcast && env::var_os("STUDIO_VIEWER_BIN").is_none() && std::fs::read_to_string(root.join("build/studio-broadcast-enabled")).map(|s|s.trim()!="1").unwrap_or(true) { return Err("Build with ./scripts/build.sh --with-broadcast first".into()); }
    let backend = env::var_os("STUDIO_VIEWER_BIN").map(PathBuf::from).unwrap_or_else(|| root.join("build/native/bin/Demo/Demo"));
    let backend = backend.canonicalize().map_err(|e| format!("Native viewer not built: {} ({e}). Run scripts/build.sh first.", backend.display()))?;
    let mut cmd = Command::new(&backend);
    cmd.current_dir(backend.parent().ok_or("Invalid viewer directory")?).env("STUDIO_FPS", opts.fps.to_string()).env_remove("STUDIO_MODEL");
    cmd.env("STUDIO_OFFSCREEN", if opts.offscreen {"1"} else {"0"})
       .env("STUDIO_STATS", if opts.stats {"1"} else {"0"})
       .env("STUDIO_DEMO", if opts.demo {"1"} else {"0"})
       .env_remove("STUDIO_WIDTH").env_remove("STUDIO_HEIGHT")
       .env("STUDIO_BROADCAST", if opts.broadcast {"1"} else {"0"})
       .env("STUDIO_NO_PREVIEW",if opts.no_preview {"1"} else {"0"})
       .env("STUDIO_BROADCAST_FPS",opts.broadcast_fps.to_string())
       .env_remove("STUDIO_BROADCAST_WIDTH").env_remove("STUDIO_BROADCAST_HEIGHT");
    cmd.env("STUDIO_PARAMETER_INPUT",if opts.parameter_input {"1"} else {"0"}).env_remove("STUDIO_PARAMETER_MAP");
    if let Some(path)=opts.parameter_map { let path=path.canonicalize().map_err(|e|format!("Parameter map: {e}"))?; if !path.is_file() {return Err("Parameter map must be a file".into());} cmd.env("STUDIO_PARAMETER_MAP",path); }
    if let Some((w,h))=opts.broadcast_size { cmd.env("STUDIO_BROADCAST_WIDTH",w.to_string()).env("STUDIO_BROADCAST_HEIGHT",h.to_string()); }
    if let Some((w,h))=opts.size { cmd.env("STUDIO_WIDTH",w.to_string()).env("STUDIO_HEIGHT",h.to_string()); }
    if let Some(model) = opts.model {
        let model = model.canonicalize().map_err(|e| format!("Model {}: {e}", model.display()))?;
        if !model.is_file() || !model.file_name().and_then(|s| s.to_str()).is_some_and(|s| s.ends_with(".model3.json")) {
            return Err("Expected a regular .model3.json file (UTF-8 filename)".into());
        }
        // Fail early for broken manifests instead of passing missing files to Cubism.
        let check = Command::new("python3").arg(root.join("scripts/check_model.py")).arg(&model).status().map_err(|e| format!("Model validation failed to start: {e}"))?;
        if !check.success() { return Err("Model validation failed".into()); }
        cmd.env("STUDIO_MODEL", model);
    }
    // Replace the launcher process: no resident Rust wrapper or duplicate frame copies.
    Err(format!("Could not launch Vulkan viewer: {}", cmd.exec()))
}
fn main() -> ExitCode {
    match run() { Ok(()) => ExitCode::SUCCESS, Err(e) => { eprintln!("{e}\n{HELP}"); ExitCode::FAILURE } }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn p(a: &[&str]) -> Result<Option<Options>, String> { parse(a.iter().map(|s| OsString::from(*s))) }
    #[test] fn defaults() { assert_eq!(p(&[]).unwrap(), Some(Options::default())); }
    #[test] fn model_and_fps() { assert_eq!(p(&["--fps","120","my model.model3.json"]).unwrap(), Some(Options{model:Some("my model.model3.json".into()),fps:120,..Options::default()})); }
    #[test] fn viewer_flags() { assert_eq!(p(&["--size","1920x1080","--offscreen","--stats","--demo","--sample"]).unwrap(),Some(Options{size:Some((1920,1080)),offscreen:true,stats:true,demo:true,..Options::default()})); }
    #[test] fn broadcast_flags() { assert_eq!(p(&["--broadcast","--broadcast-size","1280x720","--broadcast-fps","30"]).unwrap(),Some(Options{offscreen:true,broadcast:true,broadcast_size:Some((1280,720)),broadcast_fps:30,..Options::default()})); assert!(p(&["--broadcast-fps","0"]).is_err()); }
    #[test] fn no_preview() { assert_eq!(p(&["--no-preview"]).unwrap(),Some(Options{no_preview:true,broadcast:true,offscreen:true,..Options::default()})); }
    #[test] fn parameter_flags() { assert_eq!(p(&["--parameter-map","map.txt"]).unwrap(),Some(Options{parameter_input:true,parameter_map:Some("map.txt".into()),..Options::default()})); assert!(p(&["--parameter-map"]).is_err()); }
    #[test] fn bad_size() { for s in ["0x1080","1920x","1920X1080","99999x500","a","400x400x400"] { assert!(p(&["--size",s]).is_err()); } }
    #[test] fn invalid() { for a in [&["--fps"][..], &["--fps","10"], &["--sample","x"], &["x","--sample"], &["--bad"]] { assert!(p(a).is_err()); } }
}

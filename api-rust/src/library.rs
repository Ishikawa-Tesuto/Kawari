//! Original model paths only. No model assets are copied by the library.
use super::{err, model_name, read_json, validate, R};
use serde_json::json;
use std::{collections::BTreeSet, env, fs, io::{self, Write}, path::{Path, PathBuf}};

fn safe_path(p: &Path) -> bool {
    p.to_str().is_some_and(|s| !s.is_empty() && !s.chars().any(|c| c.is_control()))
}
pub(super) fn original(p: &Path) -> R<PathBuf> {
    let p = p.canonicalize()?;
    if !safe_path(&p) { return Err(err("Model path must be UTF-8 without control characters")); }
    model_name(&p)?;
    if p.parent().unwrap().join("studio-profile.json").exists() { return Err(err("Select the original model, not a generated runtime profile")); }
    validate(&p, false)?;
    Ok(p)
}
pub(super) fn directory() -> R<PathBuf> {
    let base = env::var_os("XDG_DATA_HOME").filter(|v| !v.is_empty()).map(PathBuf::from)
        .or_else(|| env::var_os("HOME").map(|v| PathBuf::from(v).join(".local/share")))
        .ok_or_else(|| err("Missing HOME"))?;
    Ok(base.join("kawari"))
}
#[derive(Default)]
pub(super) struct Library { pub last: Option<PathBuf>, models: BTreeSet<PathBuf>, roots: BTreeSet<PathBuf> }
fn paths(v: &serde_json::Value) -> R<BTreeSet<PathBuf>> {
    let a = v.as_array().ok_or_else(|| err("Invalid library path list"))?;
    a.iter().map(|x| x.as_str().map(PathBuf::from).filter(|p| safe_path(p) && p.is_absolute()).ok_or_else(|| err("Invalid library path"))).collect()
}
impl Library {
    pub fn load() -> R<Self> {
        let file = directory()?.join("library.json");
        if !file.exists() { return Ok(Self::default()); }
        let v = read_json(&file)?;
        if v["version"] != 1 { return Err(err("Unsupported library version")); }
        let last = match &v["lastModel"] { serde_json::Value::Null => None, x => Some(PathBuf::from(x.as_str().ok_or_else(|| err("Invalid last model"))?)) };
        Ok(Self { last, models: paths(&v["models"])?, roots: paths(&v["roots"])? })
    }
    pub fn add_root(&mut self, p: &Path) -> R<()> {
        let p = p.canonicalize()?;
        if !p.is_dir() || !safe_path(&p) { return Err(err("Invalid library directory")); }
        self.roots.insert(p); Ok(())
    }
    pub fn refresh(&mut self, current: &Path) {
        self.models.insert(current.to_owned());
        // Discover the usual Steam/VTS layout, not the entire home directory.
        if let Some(p) = current.ancestors().find(|p| p.file_name().and_then(|s| s.to_str()) == Some("Live2DModels")) { self.roots.insert(p.to_owned()); }
        let mut budget = 20000;
        for root in &self.roots { scan(root, 4, &mut budget, &mut self.models); }
        self.models.retain(|p| p.is_file() && safe_path(p));
    }
    pub fn save_ready(&mut self, current: &Path) -> R<()> {
        self.last = Some(current.to_owned()); self.models.insert(current.to_owned());
        let dir = directory()?; fs::create_dir_all(&dir)?;
        let mut f = tempfile::NamedTempFile::new_in(&dir)?;
        serde_json::to_writer_pretty(&mut f, &json!({"version":1,"lastModel":self.last,"models":self.models,"roots":self.roots}))?;
        f.write_all(b"\n")?; f.as_file().sync_all()?; f.persist(dir.join("library.json"))?; Ok(())
    }
    pub fn publish(&self, dir: &Path) -> R<()> {
        let mut f = tempfile::NamedTempFile::new_in(dir)?;
        for p in &self.models { if safe_path(p) { writeln!(f,"{}",p.display())?; } }
        f.persist(dir.join("models.txt"))?; Ok(())
    }
}
fn scan(dir: &Path, depth: usize, budget: &mut usize, models: &mut BTreeSet<PathBuf>) {
    if dir.join("studio-profile.json").exists() { return; }
    let Ok(entries) = fs::read_dir(dir) else { return; };
    for e in entries.flatten() {
        if *budget == 0 { return; } *budget -= 1;
        let Ok(t) = e.file_type() else { continue; }; let p = e.path();
        if t.is_dir() && depth > 0 { scan(&p, depth-1, budget, models); }
        else if t.is_file() && model_name(&p).is_ok() && safe_path(&p) { if let Ok(p) = original(&p) { models.insert(p); } }
    }
}
pub(super) fn take_request(dir: &Path) -> R<Option<PathBuf>> {
    let p = dir.join("select.txt");
    let text = match fs::read_to_string(&p) { Ok(s)=>s, Err(e) if e.kind()==io::ErrorKind::NotFound=>return Ok(None), Err(e)=>return Err(e.into()) };
    fs::remove_file(&p)?;
    if text.len()>8192 || !safe_path(Path::new(&text)) || !Path::new(&text).is_absolute() { return Err(err("Enter an absolute .model3.json path")); }
    Ok(Some(PathBuf::from(text)))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn scan_and_validate_originals() {
        let t=tempfile::tempdir().unwrap(); let root=t.path();
        let m=root.join("One");fs::create_dir(&m).unwrap();
        fs::write(m.join("a.moc3"),b"test").unwrap();fs::write(m.join("a.png"),b"test").unwrap();
        let model=m.join("One.model3.json");fs::write(&model,r#"{"Version":3,"FileReferences":{"Moc":"a.moc3","Textures":["a.png"]}}"#).unwrap();
        let bad=root.join("Bad.model3.json");fs::write(bad,"{}").unwrap();
        let mut l=Library::default();l.add_root(root).unwrap();l.refresh(&model);
        assert_eq!(l.models.len(),1);assert!(l.models.contains(&model.canonicalize().unwrap()));
        let ipc=tempfile::tempdir().unwrap();l.publish(ipc.path()).unwrap();assert_eq!(fs::read_to_string(ipc.path().join("models.txt")).unwrap().lines().count(),1);
        fs::write(m.join("studio-profile.json"),"{}").unwrap();assert!(original(&model).is_err());
    }
    #[test]
    fn mailbox_consumes_once_and_rejects_bad_paths(){
        let t=tempfile::tempdir().unwrap();let p=t.path().join("select.txt");
        assert!(take_request(t.path()).unwrap().is_none());
        fs::write(&p,"/path with spaces/Test.model3.json").unwrap();assert_eq!(take_request(t.path()).unwrap().unwrap(),PathBuf::from("/path with spaces/Test.model3.json"));
        assert!(take_request(t.path()).unwrap().is_none());fs::write(&p,"../Test.model3.json").unwrap();assert!(take_request(t.path()).is_err());assert!(!p.exists());
    }
}

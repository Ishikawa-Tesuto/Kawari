//! Exact-identity grants. Legacy CLI grants remain supported, but an explicit
//! block always wins. Managed tokens are global, not model-scoped.
use super::*;
#[derive(Clone,Default)]
pub(super) struct Manager {
    pub path: Option<PathBuf>,
    pub grants: BTreeMap<Id,Option<String>>,
    pub pending: BTreeMap<Id,Instant>,
    pub sessions: BTreeMap<usize,Id>,
}
impl Manager {
    pub fn load(path:Option<PathBuf>)->AnyResult<Self>{
        let mut s=Self{path,..Self::default()};
        let Some(p)=&s.path else{return Ok(s)};
        let f=match fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW|libc::O_NONBLOCK).open(p){Ok(f)=>f,Err(e) if e.kind()==io::ErrorKind::NotFound=>return Ok(s),Err(e)=>return Err(e.into())};
        let m=f.metadata()?;if !m.is_file()||m.uid()!=unsafe{libc::getuid()}||m.mode()&0o077!=0||m.len()>65536{return Err(err("Expected private plugin store (0600)"))}
        let v:V=serde_json::from_reader(f)?;let rows=v["plugins"].as_array().ok_or_else(||err("Invalid plugin store"))?;
        if v["schema"]!="kawari-plugins-1"||rows.len()>128{return Err(err("Invalid plugin store schema"))}
        for row in rows {let id=identity(row).map_err(|e|err(e.1))?;let token=match &row["token"]{V::Null=>None,V::String(t) if t.len()==64&&t.bytes().all(|b|b.is_ascii_hexdigit())=>Some(t.clone()),_=>return Err(err("Invalid plugin token"))};if s.grants.insert(id,token).is_some(){return Err(err("Duplicate plugin identity"))}}
        Ok(s)
    }
    pub fn set(&mut self,id:Id,allow:bool)->AnyResult<()> {self.set_with_token(id,allow,None)}
    pub fn set_with_token(&mut self,id:Id,allow:bool,reuse:Option<String>)->AnyResult<()> {
        let p=self.path.as_ref().ok_or_else(||err("Plugin manager unavailable"))?;
        let mut next=self.grants.clone();if next.len()>=128&&!next.contains_key(&id){return Err(err("Plugin limit reached"))}
        let preserve=allow&&reuse.is_some();
        next.insert(id.clone(),if allow{Some(match reuse{Some(t)=>t,None=>random()?})}else{None});
        write_json(p,&json!({"schema":"kawari-plugins-1","plugins":next.iter().map(|((n,d),t)|json!({"pluginName":n,"pluginDeveloper":d,"token":t})).collect::<Vec<_>>()}))?;
        self.grants=next;self.pending.remove(&id);if !preserve{self.sessions.retain(|_,v|v!=&id);}Ok(())
    }
    pub fn queue(&mut self,id:Id){self.prune();if self.pending.len()<32{self.pending.entry(id).or_insert(Instant::now()+Duration::from_secs(60));}}
    pub fn prune(&mut self){let now=Instant::now();self.pending.retain(|_,t|*t>now);}
    pub fn rows(&self,legacy:&BTreeMap<Id,String>)->Vec<(Id,&'static str,usize)>{
        let ids:BTreeSet<_>=self.grants.keys().chain(legacy.keys()).chain(self.pending.keys()).cloned().collect();
        ids.into_iter().map(|id|{let state=match self.grants.get(&id){Some(Some(_))=>"Allowed",Some(None)=>"Blocked",None if legacy.contains_key(&id)=>"CLI allowed",None=>"Pending"};let count=self.sessions.values().filter(|x|*x==&id).count();(id,state,count)}).collect()
    }
}
#[cfg(test)] mod tests{use super::*;#[test]fn persisted_exact_grant_and_revoke(){let d=tempfile::tempdir().unwrap();let p=d.path().join("plugins.json");let id=("Plugin".into(),"Developer".into());let mut m=Manager::load(Some(p.clone())).unwrap();m.queue(id.clone());assert_eq!(m.rows(&BTreeMap::new())[0].1,"Pending");m.set(id.clone(),true).unwrap();let old=m.grants[&id].clone();assert_eq!(Manager::load(Some(p.clone())).unwrap().grants[&id],old);assert_eq!(fs::metadata(&p).unwrap().mode()&0o777,0o600);m.set(id.clone(),false).unwrap();assert_eq!(Manager::load(Some(p)).unwrap().grants[&id],None);m.set(id.clone(),true).unwrap();assert_ne!(m.grants[&id],old);}}

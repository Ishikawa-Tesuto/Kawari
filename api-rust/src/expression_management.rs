//! Model-local management metadata; never edits the original model or VTS file.
use super::*;

#[derive(Clone,Default)]
pub(super) struct Management {
    removed: BTreeSet<String>,
    removed_hotkeys: BTreeSet<String>,
    custom: BTreeMap<String,(String,String)>, // id -> (label, expression file)
    rules: BTreeMap<String,(String,bool)>, // expression file -> (exclusive group, protected)
}
fn path(b:&Bridge)->AnyResult<PathBuf>{
    let p=b.args.state.as_ref().ok_or_else(||err("No writable profile"))?.with_extension("expression-management.json");
    outside_model(&p,&b.args.model)?;
    if [&b.args.state,&b.args.tokens,&b.args.plugin_store].into_iter().flatten().any(|x|x==&p){return Err(err("Management path collision"))}
    Ok(p)
}
fn label(s:&str)->bool{s.len()<=160&&!s.chars().any(char::is_control)&&!s.contains("##")}
fn id_ok(s:&str)->bool{!s.is_empty()&&s.len()<=512&&!s.chars().any(char::is_control)}
impl Management {
    pub(super) fn removed(&self,name:&str)->bool{self.removed.contains(name)}
    pub(super) fn group(&self,name:&str)->&str{self.rules.get(name).map_or("",|x|x.0.as_str())}
    pub(super) fn protected(&self,name:&str)->bool{self.rules.get(name).is_some_and(|x|x.1)}
}
pub(super) fn read(b:&Bridge)->AnyResult<Management>{
    if b.args.state.is_none(){return Ok(Management::default())}
    let p=path(b)?;if !p.exists(){return Ok(Management::default())}
    let v=read_json(&p)?;
    if v["schema"]!="kawari-expression-management-1"||v["modelID"]!=b.modelid{return Err(err("Invalid expression management profile"))}
    let m=Management{removed:serde_json::from_value(v["removed"].clone())?,removed_hotkeys:serde_json::from_value(v["removedHotkeys"].clone())?,custom:serde_json::from_value(v["customHotkeys"].clone())?,rules:serde_json::from_value(v["rules"].clone())?};
    validate(&m)?;
    Ok(m)
}
fn validate(m:&Management)->AnyResult<()>{
    if m.removed.len()>1024||m.removed_hotkeys.len()>1024||m.custom.len()>128||m.rules.len()>64||
       m.removed.iter().any(|n|!expression_editor::valid_file(n))||m.removed_hotkeys.iter().any(|id|!id_ok(id))||
       m.custom.iter().any(|(id,(n,f))|!id.starts_with("kawari-")||!id_ok(id)||n.is_empty()||!label(n)||!expression_editor::valid_file(f))||
       m.rules.iter().any(|(f,(g,_))|!expression_editor::valid_file(f)||!label(g)) {return Err(err("Invalid expression management entries"))}
    Ok(())
}
fn persist(b:&mut Bridge,m:Management)->AnyResult<()>{
    validate(&m)?;
    write_json(&path(b)?,&json!({"schema":"kawari-expression-management-1","modelID":b.modelid,"removed":m.removed,"removedHotkeys":m.removed_hotkeys,"customHotkeys":m.custom,"rules":m.rules}))?;
    b.expressions.management=m;apply(b);Ok(())
}
pub(super) fn apply(b:&mut Bridge){
    let e=&mut b.expressions;let m=&e.management;
    e.assets.retain(|n,_|!m.removed(n));
    let assets=&e.assets;
    e.hotkeys.retain(|h|!m.removed_hotkeys.contains(&h.id)&&!m.custom.contains_key(&h.id)&&assets.contains_key(&h.file));
    for(id,(name,file))in &m.custom{if e.assets.contains_key(file){e.hotkeys.push(Hotkey{id:id.clone(),name:name.clone(),file:file.clone(),hold:0.,screen:-1,source_binding:Candidate::Empty});}}
}
fn filename(b:&Bridge,key:&str)->AnyResult<String>{b.expressions.assets.iter().find(|(_,a)|a.key==key).map(|(n,_)|n.clone()).ok_or_else(||err("Unknown expression"))}
pub(super) fn configure(b:&mut Bridge,key:&str,group:&str,protected:&str)->AnyResult<()>{
    let n=filename(b,key)?;if b.expressions.active(&n){return Err(err("Deactivate the expression before changing its group"))}
    let group=group.trim();if !label(group){return Err(err("Invalid group name"))}
    let protected=match protected{"1"=>true,"0"=>false,_=>return Err(err("Invalid protection flag"))};
    let mut m=b.expressions.management.clone();if group.is_empty()&&!protected{m.rules.remove(&n);}else{m.rules.insert(n,(group.to_owned(),protected));}persist(b,m)
}
pub(super) fn new_hotkey(b:&mut Bridge,key:&str,name:&str)->AnyResult<()>{
    let file=filename(b,key)?;let name=name.trim();if name.is_empty()||!label(name)||b.expressions.hotkeys.iter().any(|h|h.name.to_lowercase()==name.to_lowercase()){return Err(err("Invalid or duplicate hotkey name"))}
    let mut m=b.expressions.management.clone();let id=format!("kawari-{}",&random()?[..32]);m.custom.insert(id,(name.into(),file));persist(b,m)
}
pub(super) fn delete_hotkey(b:&mut Bridge,key:&str)->AnyResult<()>{
    let id=b.expressions.hotkeys.iter().find(|h|hash(&h.id)==key).map(|h|h.id.clone()).ok_or_else(||err("Unknown hotkey"))?;
    let mut m=b.expressions.management.clone();m.custom.remove(&id);m.removed_hotkeys.insert(id);persist(b,m)
}
pub(super) fn delete_expression(b:&mut Bridge,key:&str)->AnyResult<()>{
    let n=filename(b,key)?;if b.expressions.active(&n){return Err(err("Deactivate the expression before deleting"))}
    let mut m=b.expressions.management.clone();m.removed.insert(n.clone());m.rules.remove(&n);m.custom.retain(|_,(_,f)|f.as_str()!=n.as_str());
    // A tombstone prevents an old Twitch binding from activating a new, unrelated expression.
    persist(b,m)?;b.expressions.active.remove(&n);b.expressions.order.retain(|f|f!=&n);b.save_state()
}
pub(super) fn deactivate_all(b:&mut Bridge)->AnyResult<()>{
    let names:Vec<_>=b.expressions.order.iter().filter(|n|b.expressions.active(n)&&!b.expressions.management.protected(n)).cloned().collect();
    for n in names{b.activate(&n,false,None,0.).map_err(|e|err(e.1))?;}b.save_state()
}
pub(super) fn remaining_ms(e:&Expressions,n:&str,now:Instant)->Option<u64>{
    let d=e.active.get(n).copied().flatten()?;
    if d<=now{return None}Some(d.saturating_duration_since(now).as_millis().max(1) as u64)
}

// Linux CLOCK_MONOTONIC is shared by the API and UI processes, unlike wall time.
pub(super) fn monotonic_ms()->Option<u64>{
    let mut t=libc::timespec{tv_sec:0,tv_nsec:0};
    if unsafe{libc::clock_gettime(libc::CLOCK_MONOTONIC,&mut t)}!=0{return None}
    Some((t.tv_sec as u64)*1000+(t.tv_nsec as u64)/1_000_000)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::DirBuilderExt;
    #[test]
    fn management_groups_protection_timers_and_restart(){
        let tmp=tempfile::tempdir().unwrap();let root=tmp.path();let original=root.join("model");fs::create_dir(&original).unwrap();
        let model=original.join("Test.model3.json");let a="A.exp3.json";let c="C.exp3.json";let d="D.exp3.json";
        write_json(&model,&json!({"FileReferences":{"Expressions":[{"File":a},{"File":c},{"File":d}]}})).unwrap();
        for n in [a,c,d]{write_json(&original.join(n),&json!({"Parameters":[{"Id":"ParamA","Value":1,"Blend":"Overwrite"}]})).unwrap();}
        let bytes=fs::read(original.join(c)).unwrap();
        let ipc=root.join("ipc");fs::DirBuilder::new().mode(0o700).create(&ipc).unwrap();
        let sock=ipc.join("input.sock");let rx=UnixDatagram::bind(&sock).unwrap();rx.set_read_timeout(Some(Duration::from_secs(1))).unwrap();
        let args=Args{plugin_store:None,control:None,sock,model:Some(model),port:8001,allowed:BTreeSet::new(),state:Some(root.join("state.json")),tokens:None,restore:false};
        let mut b=Bridge::new(args.clone()).unwrap();b.snap.params.insert("ParamA".into(),parameter("ParamA",-10.,10.,0.,0.,None));
        let (ak,ck,dk)=(hash(a),hash(c),hash(d));
        configure(&mut b,&ak,"Hair","1").unwrap();configure(&mut b,&ck,"Hair","0").unwrap();configure(&mut b,&dk,"","1").unwrap();
        new_hotkey(&mut b,&ak,"Toggle A").unwrap();new_hotkey(&mut b,&ck,"Toggle C").unwrap();assert!(new_hotkey(&mut b,&ck,"toggle a").is_err());
        let hk=hash(&b.expressions.hotkeys.iter().find(|h|h.file==a).unwrap().id);
        let receive=||{let mut buf=[0u8;1024];let n=rx.recv(&mut buf).unwrap();String::from_utf8(buf[..n].to_vec()).unwrap()};
        b.activate(a,true,None,0.).unwrap();assert!(receive().starts_with(format!("expr on {ak} ").as_str()));
        b.activate(d,true,None,0.).unwrap();receive();
        b.activate(c,true,None,60.).unwrap();assert!(receive().starts_with(format!("expr on {ck} ").as_str()));assert_eq!(receive(),format!("expr off {ak}"));assert!(!b.expressions.active(a));assert!(b.expressions.active(c));assert!(b.expressions.active(d));
        let old=b.expressions.active[c].unwrap();assert!(remaining_ms(&b.expressions,c,old-Duration::from_secs(30)).unwrap()>=29999);
        b.activate(c,true,None,120.).unwrap();receive();assert!(b.expressions.active[c].unwrap()>old);assert!(remaining_ms(&b.expressions,c,old+Duration::from_secs(90)).is_none());
        assert!(configure(&mut b,&ck,"Other","0").is_err());assert!(delete_expression(&mut b,&ck).is_err());
        deactivate_all(&mut b).unwrap();assert_eq!(receive(),format!("expr off {ck}"));assert!(b.expressions.active(d));assert!(remaining_ms(&b.expressions,c,Instant::now()).is_none());
        delete_hotkey(&mut b,&hk).unwrap();delete_expression(&mut b,&ck).unwrap();
        assert!(expression_editor::save(&mut b,&["save-expression","new","C","0","0","ParamA","O","1"]).is_err());
        assert_eq!(fs::read(original.join(c)).unwrap(),bytes);
        let mut next=Bridge::new(args).unwrap();assert!(!next.expressions.assets.contains_key(c));assert!(next.expressions.hotkeys.is_empty());assert_eq!(next.expressions.management.group(a),"Hair");assert!(next.expressions.management.protected(d));
        next.snap.params.insert("ParamA".into(),parameter("ParamA",-10.,10.,0.,0.,None));new_hotkey(&mut next,&ak,"New A").unwrap();let id=next.expressions.hotkeys[0].id.clone();let h=next.expressions.hotkeys[0].clone();next.trigger_hotkey(&h).unwrap();receive();assert!(next.expressions.active(a));assert!(id.starts_with("kawari-"));
    }
}

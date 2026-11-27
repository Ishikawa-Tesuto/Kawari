mod obs;
mod plugins;
mod global_shortcuts;
mod hotkey_import;
mod expression_editor;
mod expression_management;
use hotkey_import::{Candidate,shortcut_encode};
use serde_json::{json, Value as V};
use sha2::{Digest,Sha256};
use subtle::ConstantTimeEq;
use std::{collections::{BTreeMap, BTreeSet, HashMap}, fs, io::{self, Read, Write}, net::{TcpListener,TcpStream,UdpSocket}, os::unix::{fs::{MetadataExt,FileTypeExt,OpenOptionsExt},net::UnixDatagram}, path::{Path,PathBuf}, sync::{Arc,Mutex,atomic::{AtomicUsize,Ordering}}, thread, time::{Duration,Instant,SystemTime,UNIX_EPOCH}};
use tungstenite::{accept_hdr_with_config,handshake::server::{Request,Response,ErrorResponse},protocol::WebSocketConfig,Message};

type AnyResult<T> = Result<T,Box<dyn std::error::Error>>;
type Id = (String,String);
type Params = BTreeMap<String,V>;
type Owners = HashMap<String,(usize,Instant)>;
const API:&str="VTubeStudioPublicAPI";
const LIMIT:usize=16384;
fn err(s:impl Into<String>)->Box<dyn std::error::Error>{io::Error::new(io::ErrorKind::InvalidData,s.into()).into()}
fn hex(bytes:&[u8])->String{bytes.iter().map(|b|format!("{b:02x}")).collect()}
fn hash(s:&str)->String{hex(&Sha256::digest(s.as_bytes()))[..32].to_owned()}
fn random()->AnyResult<String>{let mut b=[0u8;32];fs::File::open("/dev/urandom")?.read_exact(&mut b)?;Ok(hex(&b))}
fn stamp()->u64{SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis() as u64}
fn envelope(kind:&str,data:V,rid:&str)->V{json!({"apiName":API,"apiVersion":"1.0","timestamp":stamp(),"requestID":rid,"messageType":kind,"data":data})}
fn read_json(p:&Path)->AnyResult<V>{if fs::metadata(p)?.len()>8*1024*1024{return Err(err("JSON too large"))} Ok(serde_json::from_reader(fs::File::open(p)?)?)}
fn write_json(p:&Path,v:&V)->AnyResult<()> {let parent=p.parent().filter(|p|!p.as_os_str().is_empty()).unwrap_or(Path::new("."));fs::create_dir_all(parent)?;let mut f=tempfile::NamedTempFile::new_in(parent)?;serde_json::to_writer_pretty(&mut f,v)?;f.write_all(b"\n")?;f.as_file().sync_all()?;f.persist(p)?;Ok(())}
fn strv<'a>(v:&'a V,k:&str)->&'a str{v[k].as_str().unwrap_or("")}
fn arr<'a>(v:&'a V,k:&str)->&'a [V]{v[k].as_array().map(Vec::as_slice).unwrap_or(&[])}
fn safe_id(s:&str)->bool{!s.is_empty()&&s.len()<=128&&!s.chars().any(|c|c.is_whitespace()||c.is_control())}
fn num(v:&V)->Option<f64>{v.as_f64().filter(|x|x.is_finite()&&x.abs()<=1e6)}
fn optional(v:&V,k:&str,default:f64,lo:f64,hi:f64)->Option<f64>{let x=if v.get(k).is_some(){num(&v[k])?}else{default};(lo<=x&&x<=hi).then_some(x)}
fn parameter(n:&str,a:f64,b:f64,c:f64,v:f64,owner:Option<&str>)->V{let mut p=json!({"name":n,"min":a,"max":b,"defaultValue":c,"value":v});if let Some(o)=owner{p["addedBy"]=json!(o);}p}
fn builtins()->Vec<(&'static str,f64,f64,f64,Option<&'static str>)>{vec![
    ("FaceAngleX",-30.0,30.0,0.0,Some("ParamAngleX")),
    ("FaceAngleY",-30.0,30.0,0.0,Some("ParamAngleY")),
    ("FaceAngleZ",-30.0,30.0,0.0,Some("ParamAngleZ")),
    ("EyeOpenLeft",0.0,1.0,1.0,Some("ParamEyeLOpen")),
    ("EyeOpenRight",0.0,1.0,1.0,Some("ParamEyeROpen")),
    ("EyeLeftX",-1.0,1.0,0.0,Some("ParamEyeBallX")),
    ("EyeLeftY",-1.0,1.0,0.0,Some("ParamEyeBallY")),
    ("EyeRightX",-1.0,1.0,0.0,None),
    ("EyeRightY",-1.0,1.0,0.0,None),
    ("MouthOpen",0.0,1.0,0.0,Some("ParamMouthOpenY")),
    ("MouthSmile",-1.0,1.0,0.0,Some("ParamMouthForm")),
    ("BrowLeftY",-1.0,1.0,0.0,Some("ParamBrowLY")),
    ("BrowRightY",-1.0,1.0,0.0,Some("ParamBrowRY")),
    ("FacePositionX",-10.0,10.0,0.0,None),
    ("FacePositionY",-10.0,10.0,0.0,None),
    ("FacePositionZ",-10.0,10.0,0.0,None),
    ("CheekPuff",0.0,1.0,0.0,Some("ParamCheek")),
    ("TongueOut",0.0,1.0,0.0,None),
    ("MouthX",-1.0,1.0,0.0,None),
    ("Brows",-1.0,1.0,0.0,None),
    ("MousePositionX",-1.0,1.0,0.0,None),
    ("MousePositionY",-1.0,1.0,0.0,None),
    ("VoiceVolume",0.0,1.0,0.0,None),
    ("VoiceFrequency",0.0,1.0,0.0,None),
    ("VoiceVolumePlusMouthOpen",0.0,1.0,0.0,None),
    ("VoiceFrequencyPlusMouthSmile",0.0,1.0,0.0,None),
    ("VoiceA",0.0,1.0,0.0,None),
    ("VoiceI",0.0,1.0,0.0,None),
    ("VoiceU",0.0,1.0,0.0,None),
    ("VoiceE",0.0,1.0,0.0,None),
    ("VoiceO",0.0,1.0,0.0,None),
    ("VoiceSilence",0.0,1.0,0.0,None),
    ("FaceAngry",0.0,1.0,0.0,None),
]}
#[derive(Default)]
struct Snapshot{params:Params,maps:BTreeMap<String,Vec<(String,f64,f64)>>,mtime:Option<SystemTime>}
impl Snapshot{
 fn refresh(&mut self,path:&Path){
  let read=||->AnyResult<Option<Self>>{let mt=fs::metadata(path)?.modified()?;if mt.elapsed().unwrap_or_default()>Duration::from_secs(3){return Err(err("stale snapshot"))}if self.mtime==Some(mt){return Ok(None)}let mut s=Self::default();for line in fs::read_to_string(path)?.lines(){let p:Vec<_>=line.split('\t').collect();match p.as_slice(){["P",n,a,b,c,v] if safe_id(n)=>{let (a,b,c,v):(f64,f64,f64,f64)=(a.parse()?,b.parse()?,c.parse()?,v.parse()?);if [a,b,c,v].iter().all(|x|x.is_finite()){s.params.insert(n.to_string(),parameter(n,a,b,c,v,None));}},["M",n,t,a,b,_,_] if safe_id(n)&&safe_id(t)=>{let(a,b):(f64,f64)=(a.parse()?,b.parse()?);if a.is_finite()&&b.is_finite(){s.maps.entry(n.to_string()).or_default().push((t.to_string(),a,b));}},_=>{}}}if s.params.is_empty(){return Err(err("empty snapshot"))}s.mtime=Some(mt);Ok(Some(s))};
  match read(){Ok(Some(s))=>*self=s,Ok(None)=>{},Err(_)=>*self=Self::default()}
 }
}
#[derive(Clone)]struct Asset{key:String,terms:Vec<(String,&'static str,f64)>,items:Vec<(String,bool)>,position:Option<expression_editor::ModelPosition>,fade:f64,fade_out:f64}
#[derive(Clone)]struct Hotkey{id:String,name:String,file:String,hold:f64,screen:i64,source_binding:Candidate}
#[derive(Default)]struct Expressions{management:expression_management::Management,assets:BTreeMap<String,Asset>,hotkeys:Vec<Hotkey>,saved:Vec<String>,active:BTreeMap<String,Option<Instant>>,order:Vec<String>}
impl Expressions{
 fn load(model:&Option<PathBuf>,manifest:&V)->Self{let mut e=Self::default();let Some(model)=model else{return e};let root=model.parent().unwrap();let cfg=model.with_file_name(format!("{}.vtube.json",model.file_name().unwrap().to_string_lossy().trim_end_matches(".model3.json")));let data=if cfg.exists(){match read_json(&cfg){Ok(v)=>v,Err(x)=>{eprintln!("[expressions] {x}");V::Null}}}else{V::Null};let mut files=BTreeSet::new();for x in arr(manifest,"Expressions"){if let Some(f)=x["File"].as_str(){files.insert(f.to_owned());}}for x in arr(&data,"Hotkeys"){if x["Action"]=="ToggleExpression"{if let Some(f)=x["File"].as_str(){files.insert(f.to_owned());}}}for x in arr(&data,"SavedActiveExpressions"){if let Some(f)=x.as_str(){e.saved.push(f.to_owned());files.insert(f.to_owned());}}
 for name in files{if e.assets.len()>=64{break}let load=||->AnyResult<Asset>{let p=root.join(&name).canonicalize()?;if !p.starts_with(root)||!name.ends_with(".exp3.json")||!p.is_file()||fs::metadata(&p)?.len()>1024*1024{return Err(err("unsafe expression file"))}let v=read_json(&p)?;expression_editor::parse_asset(&name,&v)};match load(){Ok(a)=>{e.assets.insert(name,a);},Err(x)=>eprintln!("[expressions] {name}: {x}")}}
 for h in arr(&data,"Hotkeys"){let f=strv(h,"File");if h["Action"]!="ToggleExpression"||!e.assets.contains_key(f)||h["IsActive"]==false||h["DeactivateAfterKeyUp"]==true{continue}let hold=if h["DeactivateAfterSeconds"]==true{let Some(v)=optional(h,"DeactivateAfterSecondsAmount",0.,0.001,86400.)else{continue};v}else{0.};let(Some(id),Some(name))=(h["HotkeyID"].as_str(),h["Name"].as_str())else{continue};e.hotkeys.push(Hotkey{id:id.into(),name:name.into(),file:f.into(),hold,screen:h["Triggers"]["ScreenButton"].as_i64().unwrap_or(-1),source_binding:vts_candidate(h)});}
 e }
 fn active(&self,name:&str)->bool{self.active.get(name).is_some_and(|x|x.is_none_or(|d|d>Instant::now()))}
 fn state(&self,name:&str,details:bool)->Vec<V>{self.assets.iter().filter(|(f,_)|name.is_empty()||f.as_str()==name).map(|(f,a)|{let active=self.active(f);let deadline=self.active.get(f).copied().flatten();let timed=active&&deadline.is_some();json!({"name":f.trim_end_matches(".exp3.json"),"file":f,"active":active,"deactivateWhenKeyIsLetGo":false,"autoDeactivateAfterSeconds":timed,"secondsRemaining":deadline.map(|d|d.saturating_duration_since(Instant::now()).as_secs_f64()).unwrap_or(0.),"usedInHotkeys":if details{self.hotkeys.iter().filter(|h|&h.file==f).map(|h|json!({"name":h.name,"id":h.id})).collect::<Vec<_>>()}else{vec![]},"parameters":if details{a.terms.iter().map(|(n,_,v)|json!({"name":n,"value":v})).collect::<Vec<_>>()}else{vec![]}})}).collect()}
}
#[derive(Debug)]struct ApiError(i32,String);
fn ae(code:i32,msg:impl Into<String>)->ApiError{ApiError(code,msg.into())}
fn bad(msg:impl Into<String>)->ApiError{ae(400,msg)}
type ApiResult<T>=Result<T,ApiError>;
fn numeric(v:&V)->ApiResult<f64>{num(v).ok_or_else(||bad("Invalid numeric value"))}
fn identity(d:&V)->ApiResult<Id>{let n=strv(d,"pluginName");let dev=strv(d,"pluginDeveloper");if [n,dev].iter().all(|s|(3..=32).contains(&s.chars().count())&&!s.chars().any(char::is_control)){Ok((n.into(),dev.into()))}else{Err(ae(50,"Invalid plugin identity"))}}
#[derive(Clone)]
struct Args{plugin_store:Option<PathBuf>,control:Option<PathBuf>,sock:PathBuf,model:Option<PathBuf>,port:u16,allowed:BTreeSet<String>,state:Option<PathBuf>,tokens:Option<PathBuf>,restore:bool}
impl Args{fn parse()->AnyResult<Self>{let mut a=Self{plugin_store:None,control:None,sock:PathBuf::new(),model:None,port:8001,allowed:BTreeSet::new(),state:None,tokens:None,restore:false};let mut it=std::env::args().skip(1);while let Some(k)=it.next(){if k=="--restore-expressions"{a.restore=true;continue}let v=it.next().ok_or_else(||err(format!("Missing value for {k}")))?;match k.as_str(){"--plugin-store"=>a.plugin_store=Some(v.into()),"--control-dir"=>a.control=Some(PathBuf::from(v).canonicalize()?),"--socket"=>a.sock=v.into(),"--model"=>a.model=Some(PathBuf::from(v).canonicalize()?),"--port"=>a.port=v.parse()?,"--allow-plugin"=>{a.allowed.insert(v.to_lowercase());},"--state-file"=>a.state=Some(v.into()),"--token-file"=>a.tokens=Some(v.into()),_=>return Err(err(format!("Unknown option {k}")))}}if a.sock.as_os_str().is_empty()||(a.allowed.is_empty()&&a.plugin_store.is_none())||a.port==0{return Err(err("--socket, --plugin-store or --allow-plugin, and valid port required"))}Ok(a)}}
fn validate_socket(p:&Path)->AnyResult<()>{let st=fs::symlink_metadata(p)?;let parent=fs::metadata(p.parent().ok_or_else(||err("socket parent missing"))?)?;let uid=unsafe{libc::getuid()};if !st.file_type().is_socket()||st.uid()!=uid||parent.uid()!=uid||parent.mode()&0o077!=0{return Err(err("Expected private user-owned parameter socket"))}Ok(())}
fn outside_model(p:&Path,model:&Option<PathBuf>)->AnyResult<()>{if let Some(m)=model{let parent=p.parent().filter(|p|!p.as_os_str().is_empty()).unwrap_or(Path::new("."));fs::create_dir_all(parent)?;let path=if p.exists(){p.canonicalize()?}else{parent.canonicalize()?.join(p.file_name().ok_or_else(||err("invalid file name"))?)};if path.starts_with(m.parent().unwrap()){return Err(err("State/token file must be outside original model directory"))}}Ok(())}
fn load_tokens(path:&Path,allowed:&BTreeSet<String>)->AnyResult<BTreeMap<Id,String>>{let mut out=BTreeMap::new();let f=match fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW|libc::O_NONBLOCK).open(path){Ok(f)=>f,Err(e) if e.kind()==io::ErrorKind::NotFound=>return Ok(out),Err(e)=>return Err(e.into())};let st=f.metadata()?;if !st.is_file()||st.uid()!=unsafe{libc::getuid()}||st.mode()&0o077!=0||st.len()>65536{return Err(err("Token file must be private (0600)"))}let v:V=serde_json::from_reader(f)?;let rows=v["tokens"].as_array().ok_or_else(||err("invalid token file"))?;if v["schema"]!="studio-api-tokens-1"||rows.len()>64{return Err(err("invalid token schema"))}for row in rows{let n=strv(row,"name");let d=strv(row,"developer");let t=strv(row,"token");if ![n,d].iter().all(|s|(3..=32).contains(&s.chars().count()))||t.len()!=64||!t.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b)){return Err(err("invalid stored token"))}if allowed.contains(&n.to_lowercase()){out.insert((n.into(),d.into()),t.into());}}Ok(out)}
fn save_tokens(path:&Path,tokens:&BTreeMap<Id,String>)->AnyResult<()>{write_json(path,&json!({"schema":"studio-api-tokens-1","tokens":tokens.iter().map(|((n,d),t)|json!({"name":n,"developer":d,"token":t})).collect::<Vec<_>>()}))}
struct Bridge{plugins:plugins::Manager,paused:bool,args:Args,sender:UnixDatagram,snap:Snapshot,modelname:String,modelid:String,manifest:V,expressions:Expressions,tokens:BTreeMap<Id,String>,custom:BTreeMap<String,(Id,V)>,values:HashMap<String,(f64,Instant)>,owners:Owners,route_owners:Owners,face:HashMap<usize,Instant>}
impl Bridge{
 fn new(args:Args)->AnyResult<Self>{validate_socket(&args.sock)?;for p in [&args.state,&args.tokens,&args.plugin_store].into_iter().flatten(){outside_model(p,&args.model)?;}if args.state.is_some()&&args.state==args.tokens{return Err(err("state/token collision"))}let name=args.model.as_ref().map(|p|p.file_name().unwrap().to_string_lossy().trim_end_matches(".model3.json").to_owned()).unwrap_or("Haru".into());let id=hash(&args.model.as_ref().map(|p|p.to_string_lossy().into_owned()).unwrap_or("sample:Haru".into()));let manifest=match &args.model{Some(p)=>read_json(p)?["FileReferences"].clone(),None=>V::Null};let expressions=Expressions::load(&args.model,&manifest);let tokens=match &args.tokens{Some(p)=>load_tokens(p,&args.allowed)?,None=>BTreeMap::new()};let sender=UnixDatagram::unbound()?;sender.set_nonblocking(true)?;let plugins=plugins::Manager::load(args.plugin_store.clone())?;let mut b=Self{plugins,paused:false,args,sender,snap:Snapshot::default(),modelname:name,modelid:id,manifest,expressions,tokens,custom:BTreeMap::new(),values:HashMap::new(),owners:HashMap::new(),route_owners:HashMap::new(),face:HashMap::new()};b.expressions.management=expression_management::read(&b)?;if let Err(e)=expression_editor::load(&mut b){eprintln!("[expression-editor] {e}");}expression_management::apply(&mut b);Ok(b)}
 // The listener, session identities and plugin-defined inputs outlive the model.
 fn switch_model(&mut self,request:&V)->AnyResult<()> {
  if !self.paused{return Err(err("Model switch requires paused API"))}
  let path=|key:&str|->AnyResult<PathBuf>{let text=request[key].as_str().filter(|s|!s.is_empty()).ok_or_else(||err(format!("Missing {key}")))?;let p=PathBuf::from(text);if !p.is_absolute(){return Err(err(format!("Expected absolute {key}")))}Ok(p)};
  let mut args=self.args.clone();args.sock=path("socket")?;if args.sock==self.args.sock{return Err(err("Cannot reuse old model socket"))}args.model=Some(path("model")?.canonicalize()?);args.state=Some(path("state")?);args.tokens=Some(path("tokens")?);
  let mut next=Self::new(args)?;next.refresh();if next.snap.params.is_empty(){return Err(err("New model snapshot not ready"))}
  // Current server tokens win: authenticated sessions must stay valid. Preserve
  // other allowed identities already present in the destination profile.
  for (id,token) in &self.tokens{next.tokens.insert(id.clone(),token.clone());}
  if next.tokens.len()>64{return Err(err("Merged token limit exceeded"))}
  next.custom=self.custom.clone();next.plugins=self.plugins.clone();next.restore()?;
  if let Some(p)=&next.args.tokens{save_tokens(p,&next.tokens)?;}
  // Do not replay old tracking values or route ownership into a different rig.
  *self=next;Ok(())
 }
 fn refresh(&mut self){if self.paused{return}self.snap.refresh(&self.args.sock.parent().unwrap().join("parameters.tsv"));}
 fn send(&self,p:&str)->ApiResult<()>{if p.len()>LIMIT{return Err(bad("IPC packet too large"))}self.sender.send_to(p.as_bytes(),&self.args.sock).map_err(|e|bad(format!("IPC unavailable: {e}")))?;if p.starts_with("expr ")&&matches!(std::env::var("KAWARI_EXPRESSION_DEBUG").as_deref(),Ok("1")){eprintln!("[expression IPC] {p}");}Ok(())}
 fn activate(&mut self,name:&str,on:bool,fade:Option<f64>,hold:f64)->ApiResult<()>{
  self.set_expression(name,on,fade,hold,true)
 }
 fn set_expression(&mut self,name:&str,on:bool,fade:Option<f64>,hold:f64,move_model:bool)->ApiResult<()>{
  self.activate_raw(name,on,fade,hold,move_model)?;
  if on&&self.expressions.active(name){
   let group=self.expressions.management.group(name);
   if !group.is_empty(){
    let others:Vec<_>=self.expressions.order.iter()
     .filter(|n|n.as_str()!=name&&self.expressions.active(n)&&self.expressions.management.group(n)==group)
     .cloned().collect();
    for other in others{self.activate_raw(&other,false,None,0.,false)?;}
   }
  }
  Ok(())
 }
 fn activate_raw(&mut self,name:&str,on:bool,fade:Option<f64>,hold:f64,move_model:bool)->ApiResult<()>{
  let asset=self.expressions.assets.get(name).ok_or_else(||bad("Unknown expression"))?;
  if on{
   let effects=!asset.terms.is_empty()||!asset.items.is_empty();
   // A placement-only expression runs on each call and has no active state to restore.
   if !effects&&!move_model{return Ok(())}
   if asset.terms.iter().any(|(id,_,_)|!self.snap.params.contains_key(id)){return Err(bad("Missing rig parameters"))}
   let fi=(fade.unwrap_or(asset.fade)*1000.).round() as u64;
   let fo=(asset.fade_out*1000.).round() as u64;
   let ttl=if hold>0.{((hold*1000.).round() as u64).max(1)}else{0};
   let mut packet=if fi==fo{format!("expr on {} {fi} {ttl}",asset.key)}else{format!("expr on2 {} {fi} {ttl} {fo}",asset.key)};
   for(id,blend,value)in &asset.terms{packet.push_str(&format!(" {id} {blend} {value}"));}
   for(id,show)in &asset.items{packet.push_str(&format!(" {id} I {}",u8::from(*show)));}
   if move_model{if let Some(position)=asset.position{packet.push_str(&position.packet());}}
   self.send(&packet)?;
   if effects{
    self.expressions.order.retain(|entry|entry!=name);
    self.expressions.order.push(name.into());
    self.expressions.active.insert(name.into(),(ttl>0).then(||Instant::now()+Duration::from_millis(ttl)));
   }
  }else{
   let packet=match fade{
    Some(v)=>format!("expr off2 {} {}",asset.key,(v*1000.).round() as u64),
    None=>format!("expr off {}",asset.key),
   };
   self.send(&packet)?;
   self.expressions.active.remove(name);
   self.expressions.order.retain(|entry|entry!=name);
  }
  Ok(())
 }
 // Hotkeys use the expression's authored transitions, not the imported VTS shortcut fade.
 fn trigger_hotkey(&mut self,h:&Hotkey)->ApiResult<()>{let on=!self.expressions.active(&h.file);self.activate(&h.file,on,None,h.hold)}
 fn save_state(&self)->AnyResult<()>{if let Some(p)=&self.args.state{let names=self.expressions.order.iter().filter(|n|self.expressions.active.get(*n).is_some_and(|d|d.is_none())).collect::<Vec<_>>();write_json(p,&json!({"schema":"studio-model-state-1","modelID":self.modelid,"activeExpressions":names}))?;}Ok(())}
 fn restore(&mut self)->AnyResult<()>{let saved=if let Some(p)=self.args.state.as_ref().filter(|p|p.exists()){let v=read_json(p)?;if v["schema"]!="studio-model-state-1"||v["modelID"]!=self.modelid{return Err(err("State file belongs to another model"))}let a=v["activeExpressions"].as_array().ok_or_else(||err("Invalid state"))?;if a.len()>64||a.iter().any(|x|!x.is_string()){return Err(err("Invalid state entries"))}Some(a.iter().map(|x|x.as_str().unwrap().to_owned()).collect::<Vec<_>>())}else{None};if self.expressions.assets.is_empty(){return Ok(())}self.refresh();if self.snap.params.is_empty(){return Err(err("Wait until model snapshot ready"))}self.send("expr reset").map_err(|e|err(e.1))?;let initial=saved.is_none();let names=saved.unwrap_or_else(||if self.args.restore{self.expressions.saved.clone()}else{vec![]});for n in names{if let Err(e)=self.set_expression(&n,true,None,0.,false){eprintln!("[expressions] {n}: {}",e.1);}}if initial{self.save_state()?;}Ok(())}
 fn inputs(&self,now:Instant)->Params{let mut out=Params::new();for(n,a,b,c,_)in builtins(){out.insert(n.into(),parameter(n,a,b,c,c,Some("VTube Studio")));}for(n,ms)in &self.snap.maps{let lo=ms.iter().map(|(_,a,b)|a.min(*b)).fold(f64::INFINITY,f64::min);let hi=ms.iter().map(|(_,a,b)|a.max(*b)).fold(f64::NEG_INFINITY,f64::max);if let Some(p)=out.get_mut(n){if ms.iter().any(|(t,_,_)|t!=n){p["min"]=json!(lo.min(p["min"].as_f64().unwrap()));p["max"]=json!(hi.max(p["max"].as_f64().unwrap()));}}else{let c=0f64.clamp(lo,hi);out.insert(n.clone(),parameter(n,lo,hi,c,c,Some("Studio")));}}for(n,(_,p))in &self.custom{out.insert(n.clone(),p.clone());}for(n,(v,expiry))in &self.values{if *expiry>now{if let Some(p)=out.get_mut(n){p["value"]=json!(v);}}}out}
 fn route(&self,n:&str)->Option<String>{if self.snap.maps.contains_key(n){return Some(n.into())}let t=builtins().into_iter().find(|x|x.0==n)?.4?;self.snap.maps.get(t).filter(|ms|ms.iter().any(|(x,_,_)|x==t)).map(|_|t.into())}
 fn info(&self)->V{let loaded=!self.paused&&!self.snap.params.is_empty();json!({"modelLoaded":loaded,"modelName":if loaded{self.modelname.as_str()}else{""},"modelID":if loaded{self.modelid.as_str()}else{""}})}
 fn request(&mut self,msg:V,s:&mut Session)->V{let rid=msg["requestID"].as_str().filter(|x|!x.is_empty()&&x.len()<=64).unwrap_or("invalid-request");let kind=strv(&msg,"messageType");let empty=json!({});let d=msg.get("data").filter(|x|!x.is_null()).unwrap_or(&empty);let result=if msg["apiName"]!=API||msg["apiVersion"]!="1.0"{Err(ae(3,"Unsupported API envelope"))}else if kind.is_empty()||!d.is_object(){Err(bad("Invalid request"))}else{self.dispatch(kind,d,s)};match result{Ok(v)=>envelope(&format!("{}Response",kind.trim_end_matches("Request")),v,rid),Err(e)=>envelope("APIError",json!({"errorID":e.0,"message":e.1}),rid)}}
 fn credential(&self,id:&Id)->Option<&String>{match self.plugins.grants.get(id){Some(t)=>t.as_ref(),None if self.args.allowed.contains(&id.0.to_lowercase())=>self.tokens.get(id),None=>None}}
 fn permitted(&self,id:&Id)->bool{match self.plugins.grants.get(id){Some(t)=>t.is_some(),None=>self.args.allowed.contains(&id.0.to_lowercase())}}
 fn session_valid(&self,s:&Session)->bool{s.identity.as_ref().and_then(|id|self.credential(id)).is_some_and(|t|bool::from(t.as_bytes().ct_eq(s.token.as_bytes())))}
 fn dispatch(&mut self,k:&str,d:&V,s:&mut Session)->ApiResult<V>{
  if !self.session_valid(s){s.identity=None;s.token.clear();self.plugins.sessions.remove(&s.id);}
  match k{
   "APIStateRequest"=>return Ok(json!({"active":true,"vTubeStudioVersion":"0.4.0","currentSessionAuthenticated":s.identity.is_some()})),
   "AuthenticationTokenRequest"=>{let id=identity(d)?;if !self.permitted(&id){return Err(ae(50,"Plugin not approved"))}let token=if let Some(t)=self.credential(&id){t.clone()}else{if self.tokens.len()>=64{return Err(bad("Token limit reached"))}let t=random().map_err(|_|bad("Token generation failed"))?;let mut proposed=self.tokens.clone();proposed.insert(id,t.clone());if let Some(p)=&self.args.tokens{save_tokens(p,&proposed).map_err(|_|bad("Could not persist token"))?;}self.tokens=proposed;t};return Ok(json!({"authenticationToken":token}))},
   "AuthenticationRequest"=>{let id=identity(d)?;let token=strv(d,"authenticationToken");let ok=self.credential(&id).is_some_and(|t|bool::from(t.as_bytes().ct_eq(token.as_bytes())));self.plugins.sessions.remove(&s.id);if ok{self.plugins.sessions.insert(s.id,id.clone());}s.identity=ok.then_some(id);s.token=if ok{token.into()}else{String::new()};return Ok(json!({"authenticated":ok,"reason":if ok{"Token valid"}else{"Invalid token; request a new token"}}))},_=>{}}
  let id=s.identity.clone().ok_or_else(||ae(8,"Request requires authentication"))?;self.refresh();let now=Instant::now();self.owners.retain(|_,(_,t)|*t>now);self.route_owners.retain(|_,(_,t)|*t>now);self.values.retain(|_,(_,t)|*t>now);self.face.retain(|_,t|*t>now);let inputs=self.inputs(now);let mut info=self.info();let loaded=info["modelLoaded"]==true;
  match k{
   "CurrentModelRequest"=>{info["vtsModelName"]=json!("");info["vtsModelIconName"]=json!("");info["live2DModelName"]=json!(if loaded{format!("{}.model3.json",self.modelname)}else{String::new()});info["numberOfLive2DParameters"]=json!(if loaded{self.snap.params.len()}else{0});info["hasPhysicsFile"]=json!(loaded&&!strv(&self.manifest,"Physics").is_empty());info["numberOfTextures"]=json!(if loaded{arr(&self.manifest,"Textures").len()}else{0});Ok(info)},
   "Live2DParameterListRequest"=>{info["parameters"]=json!(if loaded{self.snap.params.values().collect::<Vec<_>>()}else{vec![]});Ok(info)},
   "InputParameterListRequest"=>{info["defaultParameters"]=json!(inputs.iter().filter(|(n,_)|!self.custom.contains_key(*n)).map(|(_,p)|p).collect::<Vec<_>>());info["customParameters"]=json!(self.custom.keys().filter_map(|n|inputs.get(n)).collect::<Vec<_>>());Ok(info)},
   "ParameterValueRequest"=>inputs.get(strv(d,"name")).cloned().ok_or_else(||ae(453,"Parameter not found")),
   "FaceFoundRequest"=>Ok(json!({"found":!self.face.is_empty()})),
   "ParameterCreationRequest"=>{let n=strv(d,"parameterName");let(a,b,c)=(numeric(&d["min"])?,numeric(&d["max"])?,numeric(&d["defaultValue"])?);if !(4..=32).contains(&n.len())||!n.bytes().all(|b|b.is_ascii_alphanumeric())||a>=b||c<a||c>b{return Err(bad("Invalid custom parameter definition"))}if builtins().iter().any(|x|x.0==n)||self.custom.get(n).is_some_and(|(o,_)|o!=&id){return Err(bad("Parameter already owned"))}if !self.custom.contains_key(n)&&(self.custom.len()>=300||self.custom.values().filter(|(o,_)|o==&id).count()>=100){return Err(bad("Custom parameter limit reached"))}self.custom.insert(n.into(),(id.clone(),parameter(n,a,b,c,c,Some(&id.0))));Ok(json!({"parameterName":n}))},
   "ParameterDeletionRequest"=>{let n=strv(d,"parameterName");if !self.custom.get(n).is_some_and(|(o,_)|o==&id){return Err(bad("Parameter not owned"))}self.custom.remove(n);self.values.remove(n);self.owners.remove(n);Ok(json!({"parameterName":n}))},
   "InjectParameterDataRequest"=>{if !loaded&&!self.paused{return Err(bad("Model IPC not ready"))}if d.get("mode").is_some()&&d["mode"]!="set"{return Err(bad("Only set mode supported"))}let values=arr(d,"parameterValues");if values.is_empty()||values.len()>300{return Err(bad("Invalid parameterValues"))}let mut pending=Vec::new();let mut names=BTreeSet::new();let mut routes=Vec::new();let mut targets=BTreeSet::new();for x in values{let n=strv(x,"id");let v=numeric(&x["value"])?;let w=if x.get("weight").is_some(){numeric(&x["weight"])?}else{1.};if w!=1.{return Err(bad("Only weight=1 supported"))}if !inputs.contains_key(n)||!names.insert(n){return Err(ae(453,"Unknown or duplicate input parameter"))}if self.owners.get(n).is_some_and(|(o,_)|*o!=s.id){return Err(bad("Parameter controlled by another session"))}pending.push((n.to_owned(),v));if let Some(r)=self.route(n).filter(|_|!self.paused){if !targets.insert(r.clone())||self.route_owners.get(&r).is_some_and(|(o,_)|*o!=s.id){return Err(bad("Conflicting model mapping"))}routes.push((r,v));}}if routes.len()>256{return Err(bad("Too many mapped inputs"))}if self.paused{return Ok(json!({}))}if !routes.is_empty(){let mut packet="set plugin 1000".to_owned();for(r,v)in &routes{packet.push_str(&format!(" {r} {v}"));}self.send(&packet)?;}let expiry=now+Duration::from_secs(1);for(r,_)in routes{self.route_owners.insert(r,(s.id,expiry));}for(n,v)in pending{self.values.insert(n.clone(),(v,expiry));self.owners.insert(n,(s.id,expiry));}if d["faceFound"]==true{self.face.insert(s.id,expiry);}else if d["faceFound"]==false{self.face.remove(&s.id);}Ok(json!({}))},
   "HotkeysInCurrentModelRequest"=>{let mid=strv(d,"modelID");if !strv(d,"live2DItemFileName").is_empty()||(!mid.is_empty()&&mid!=self.modelid){return Err(bad("Only current model supported"))}info["availableHotkeys"]=json!(if loaded{self.expressions.hotkeys.iter().map(|h|json!({"name":h.name,"type":"ToggleExpression","description":"Toggle expression","file":h.file,"hotkeyID":h.id,"keyCombination":[],"onScreenButtonID":h.screen})).collect::<Vec<_>>()}else{vec![]});Ok(info)},
   "ExpressionStateRequest"=>{let n=strv(d,"expressionFile");if loaded&&!n.is_empty()&&!self.expressions.assets.contains_key(n){return Err(bad("Unknown expression"))}info["expressions"]=json!(if loaded{self.expressions.state(n,d["details"]==true)}else{vec![]});Ok(info)},
   "ExpressionActivationRequest"=>{if !loaded{return Err(bad("Model unavailable"))}let n=strv(d,"expressionFile");let on=d["active"].as_bool().ok_or_else(||bad("Invalid active"))?;let fade=if d.get("fadeTime").is_some(){Some(numeric(&d["fadeTime"])?.clamp(0.,2.))}else{None};self.activate(n,on,fade,0.)?;if let Err(e)=self.save_state(){eprintln!("[state] {e}");}Ok(json!({}))},
   "KawariRewardExpressionRequest"=>{if !loaded||strv(d,"modelID")!=self.modelid{return Err(bad("Reward is for another model"))}let n=strv(d,"expressionFile");let mode=strv(d,"mode");let hold=optional(d,"duration",0.,0.,300.).ok_or_else(||bad("Invalid reward duration"))?;let on=match mode{"on"=>true,"off"=>false,"toggle"=>!self.expressions.active(n),"timed" if hold>0.=>true,_=>return Err(bad("Invalid reward action"))};self.activate(n,on,None,if mode=="timed"{hold}else{0.})?;self.save_state().map_err(|e|bad(e.to_string()))?;Ok(json!({}))},
   "HotkeyTriggerRequest"=>{if !loaded||!strv(d,"itemInstanceID").is_empty(){return Err(bad("Invalid hotkey request"))}let key=strv(d,"hotkeyID");let h=self.expressions.hotkeys.iter().find(|h|h.id==key||h.name.to_lowercase()==key.to_lowercase()).cloned().ok_or_else(||bad("Unknown hotkey"))?;self.trigger_hotkey(&h)?;if let Err(e)=self.save_state(){eprintln!("[state] {e}");}Ok(json!({"hotkeyID":h.id}))},
   _=>Err(ae(7,format!("Unsupported request: {k}")))
  }
 }
}
struct Session{id:usize,identity:Option<Id>,token:String}
struct Slot(Arc<AtomicUsize>);impl Drop for Slot{fn drop(&mut self){self.0.fetch_sub(1,Ordering::Relaxed);}}
// Wait without holding the model lock, so the UI and other clients keep running.
fn client_request(bridge:&Arc<Mutex<Bridge>>,msg:V,session:&mut Session)->Option<V>{
 let candidate=if msg["apiName"]==API&&msg["apiVersion"]=="1.0"&&msg["messageType"]=="AuthenticationTokenRequest"{identity(&msg["data"]).ok()}else{None};
 if let Some(id)=candidate{let deadline=Instant::now()+Duration::from_secs(60);loop{
  {let mut b=bridge.lock().ok()?;if b.plugins.path.is_none()||b.permitted(&id)||b.plugins.grants.contains_key(&id)||Instant::now()>=deadline{break}b.plugins.queue(id.clone());}
  thread::sleep(Duration::from_millis(100));
 }}
 Some(bridge.lock().ok()?.request(msg,session))
}
fn client(stream:TcpStream,bridge:Arc<Mutex<Bridge>>,id:usize,_slot:Slot){
 let _=stream.set_read_timeout(Some(Duration::from_secs(5)));let _=stream.set_write_timeout(Some(Duration::from_secs(5)));let _=stream.set_nodelay(true);
 let config=WebSocketConfig::default().read_buffer_size(16*1024).write_buffer_size(16*1024).max_message_size(Some(1024*1024)).max_frame_size(Some(1024*1024)).max_write_buffer_size(2*1024*1024);
 let Ok(mut ws)=accept_hdr_with_config(stream,|req:&Request,res:Response|->Result<Response,ErrorResponse>{if req.headers().contains_key("origin"){let mut r=ErrorResponse::new(Some("Browser origins not allowed".into()));*r.status_mut()=tungstenite::http::StatusCode::FORBIDDEN;Err(r)}else{Ok(res)}},Some(config))else{return};
 let mut session=Session{id,identity:None,token:String::new()};loop{match ws.read(){Ok(Message::Text(text))=>{let msg=serde_json::from_str(text.as_str()).unwrap_or(V::Null);let Some(reply)=client_request(&bridge,msg,&mut session)else{break};if ws.send(Message::Text(reply.to_string().into())).is_err(){break}},Ok(Message::Binary(bytes))=>{let msg=serde_json::from_slice(&bytes).unwrap_or(V::Null);let Some(reply)=client_request(&bridge,msg,&mut session)else{break};if ws.send(Message::Text(reply.to_string().into())).is_err(){break}},Ok(Message::Close(_))=>{let _=ws.flush();break},Ok(_)=>{if ws.flush().is_err(){break}},Err(tungstenite::Error::Io(e)) if matches!(e.kind(),io::ErrorKind::WouldBlock|io::ErrorKind::TimedOut)=>continue,Err(_)=>break}}
 if let Ok(mut b)=bridge.lock(){b.face.remove(&id);b.plugins.sessions.remove(&id);}
}
// Supervisor-only control mailbox (private 0700 directory). One atomic
// request at a time; acknowledgments use the existing child stdout channel.
fn poll_control(dir:&Path,bridge:&Arc<Mutex<Bridge>>,global:&mut global_shortcuts::Runtime)->AnyResult<()> {
 let p=dir.join("api-control.json");let request=match fs::symlink_metadata(&p){
  Ok(m)=>{if !m.is_file()||m.uid()!=unsafe{libc::getuid()}||m.len()>32768{return Err(err("Unsafe API control file"))}let r=read_json(&p)?;fs::remove_file(&p)?;r},
  Err(e)if e.kind()==io::ErrorKind::NotFound=>return Ok(()),Err(e)=>return Err(e.into())};
 let action=strv(&request,"action");let result=(||->AnyResult<()>{let mut b=bridge.lock().map_err(|_|err("API context lock poisoned"))?;match action{
  "pause"=>{if b.paused{return Err(err("Already paused"))}b.paused=true;global.stop();Ok(())},
  "resume"=>{if !b.paused{return Err(err("Not paused"))}validate_socket(&b.args.sock)?;b.paused=false;b.refresh();Ok(())},
  "switch"=>b.switch_model(&request),_=>Err(err("Unknown API control action"))}})();
 match result{Ok(())=>println!("[api-control] ok {action}"),Err(e)=>println!("[api-control] error {action}: {e}")};io::stdout().flush()?;Ok(())
}
// Local UI mailbox lives beside the private, model-scoped parameter socket.
// Each request uses a stable asset/hotkey hash, never a display name or path.
fn ui_text(s:&str)->String {s.chars().map(|c|if c.is_control(){' '}else{c}).collect()}
fn ui_binding_path(b:&Bridge)->Option<PathBuf>{b.args.state.as_ref().map(|p|p.with_extension("hotkeys.json"))}
fn vts_candidate(h:&V)->Candidate {
 let t=&h["Triggers"];if t.is_null(){return Candidate::Empty}if !t.is_object(){return Candidate::Unsupported}
 let mut keys=Vec::new();for field in ["Trigger1","Trigger2","Trigger3"]{match t.get(field){None=>{},Some(v)=>{let Some(key)=v.as_str()else{return Candidate::Unsupported};keys.push(key);}}}hotkey_import::candidate(&keys)
}
fn shortcut_normalize(v:u32)->Option<u32>{
 if v==0{return Some(0)}
 if v<128{let k=v&15;if !(1..=8).contains(&k){return None}return shortcut_encode(293+k,(if v&16!=0{2}else{0})|(if v&32!=0{1}else{0})|(if v&64!=0{4}else{0}));}
 if v& !0x11fff!=0||v&0x10000==0{return None}
 shortcut_encode(v&511,(v>>9)&15).filter(|x|*x==v)
}
fn ui_bindings(b:&Bridge)->AnyResult<BTreeMap<String,u32>>{
 let Some(p)=ui_binding_path(b)else{return Ok(BTreeMap::new())};
 if !p.exists(){return Ok(BTreeMap::new())}let v=read_json(&p)?;
 if (v["schema"]!="kawari-hotkeys-1"&&v["schema"]!="kawari-hotkeys-2")||v["modelID"]!=b.modelid{return Err(err("Invalid or foreign hotkey profile"))}
 let old:BTreeMap<String,u32>=serde_json::from_value(v["bindings"].clone())?;let mut result=BTreeMap::new();
 for(k,x)in old{let x=shortcut_normalize(x).ok_or_else(||err("Invalid saved shortcut"))?;if x!=0{result.insert(k,x);}}
 Ok(result)
}
fn ui_globals(b:&Bridge)->AnyResult<BTreeSet<String>>{
 let Some(p)=ui_binding_path(b)else{return Ok(BTreeSet::new())};if !p.exists(){return Ok(BTreeSet::new())}let v=read_json(&p)?;
 if v["modelID"]!=b.modelid{return Err(err("Invalid or foreign hotkey profile"))}
 if v.get("global").is_none(){return Ok(BTreeSet::new())}Ok(serde_json::from_value(v["global"].clone())?)
}
// Publish every API input, not just the inputs currently routed to Live2D.
// Values without a recent injection are explicitly marked inactive.
fn input_ui_catalog(b:&Bridge,now:Instant)->String {
 let mut out=String::from("kawari-inputs-1\n");
 let builtin=builtins();
 for (name,p) in b.inputs(now){
  if !safe_id(&name)||name.contains('#'){continue}
  let (Some(lo),Some(hi),Some(default),Some(value))=(p["min"].as_f64(),p["max"].as_f64(),p["defaultValue"].as_f64(),p["value"].as_f64())else{continue};
  if ![lo,hi,default,value].iter().all(|v|v.is_finite())||lo>=hi{continue}
  let kind=if b.custom.contains_key(&name){"plugin"}else if builtin.iter().any(|x|x.0==name.as_str()){"builtin"}else{"mapping"};
  let live=b.values.get(&name).is_some_and(|(_,expiry)|*expiry>now);
  let routed=b.route(&name).is_some();
  out.push_str(&format!("I\t{name}\t{kind}\t{}\t{lo}\t{hi}\t{default}\t{value}\t{}\t{}\n",ui_text(strv(&p,"addedBy")),live as u8,routed as u8));
 }
 out
}
fn plugin_ui(b:&mut Bridge,dir:&Path)->AnyResult<()> {
 if b.plugins.path.is_none(){return Ok(())}b.plugins.prune();
 let req=dir.join("plugins-request.tsv");
 if req.exists(){let result=(||->AnyResult<()>{let m=fs::symlink_metadata(&req)?;if !m.is_file()||m.uid()!=unsafe{libc::getuid()}||m.len()>256{return Err(err("Unsafe plugin request"))}let text=fs::read_to_string(&req)?;let (action,key)=text.trim_end().split_once('\t').ok_or_else(||err("Invalid plugin request"))?;
 let (id,_,_)=b.plugins.rows(&b.tokens).into_iter().find(|(id,_,_)|plugin_key(id)==key).ok_or_else(||err("Unknown plugin"))?;
 let allow=match action{"allow"=>true,"block"=>false,_=>return Err(err("Invalid plugin action"))};
 let sessions:BTreeSet<_>=b.plugins.sessions.iter().filter(|(_,v)|*v==&id).map(|(k,_)|*k).collect();
 let reuse=if allow&&!b.plugins.grants.contains_key(&id){b.tokens.get(&id).cloned()}else{None};
 if reuse.is_some(){b.plugins.set_with_token(id.clone(),true,reuse)?;return Ok(())}
 b.plugins.set(id.clone(),allow)?;
 // Release ownership immediately; the renderer's existing TTL expires last samples.
 let names:Vec<_>=b.owners.iter().filter(|(_,v)|sessions.contains(&v.0)).map(|(k,_)|k.clone()).collect();for n in names{b.values.remove(&n);b.owners.remove(&n);}b.route_owners.retain(|_,v|!sessions.contains(&v.0));b.face.retain(|k,_|!sessions.contains(k));
 b.custom.retain(|_,(owner,_)|&*owner!=&id);Ok(())})();let _=fs::remove_file(&req);let mut f=tempfile::NamedTempFile::new_in(dir)?;writeln!(f,"{}",match result{Ok(())=>"OK".into(),Err(e)=>ui_text(&e.to_string())})?;f.persist(dir.join("plugins-result.txt"))?;}
 let mut out=String::from("kawari-plugins-1\n");for (id,state,count) in b.plugins.rows(&b.tokens){out.push_str(&format!("{}\t{}\t{}\t{}\t{}\n",plugin_key(&id),state,count,ui_text(&id.0),ui_text(&id.1)));}
 let path=dir.join("plugins-ui.tsv");if fs::read_to_string(&path).ok().as_deref()!=Some(out.as_str()){let mut f=tempfile::NamedTempFile::new_in(dir)?;f.write_all(out.as_bytes())?;f.persist(path)?;}Ok(())
}
fn plugin_key(id:&Id)->String{hash(&format!("{}:{}:{}",id.0.len(),id.0,id.1))}
fn ui_poll(bridge:&Arc<Mutex<Bridge>>,last:&mut String,last_inputs:&mut String,global:&mut global_shortcuts::Runtime)->AnyResult<()> {
 let mut b=bridge.lock().map_err(|_|err("API context lock poisoned"))?;
 if b.paused{global.stop();return Ok(())}
 let dir=b.args.sock.parent().ok_or_else(||err("Missing socket directory"))?.to_owned();
 plugin_ui(&mut b,&dir)?;
 let req=dir.join("expression-request.tsv");let state=dir.join("expression-ui.tsv");
 b.refresh();let input_state=dir.join("input-ui.tsv");let catalog=input_ui_catalog(&b,Instant::now());
 if *last_inputs!=catalog||!input_state.exists(){let mut f=tempfile::NamedTempFile::new_in(&dir)?;f.write_all(catalog.as_bytes())?;f.persist(input_state)?;*last_inputs=catalog;}
 let saved_bindings=ui_bindings(&b);let mut bindings=saved_bindings.as_ref().cloned().unwrap_or_default();let mut status=String::new();let mut import_status=None;let saved_globals=ui_globals(&b);let mut globals=saved_globals.as_ref().cloned().unwrap_or_default();
 let known_keys:BTreeSet<_>=b.expressions.hotkeys.iter().map(|h|hash(&h.id)).collect();bindings.retain(|k,_|known_keys.contains(k));globals.retain(|k|known_keys.contains(k));
 if req.exists(){
  let result=(||->AnyResult<()> {
   let m=fs::symlink_metadata(&req)?;if !m.is_file()||m.uid()!=unsafe{libc::getuid()}||m.len()>LIMIT as u64{return Err(err("Unsafe UI request"))}
   let text=fs::read_to_string(&req)?;let fields:Vec<_>=text.trim_end_matches('\n').split('\t').collect();b.refresh();
   match fields.as_slice(){
    ["configure-expression",key,group,protected]=>expression_management::configure(&mut b,key,group,protected)?,
    ["new-hotkey",key,name]=>expression_management::new_hotkey(&mut b,key,name)?,
    ["delete-hotkey",key]=>expression_management::delete_hotkey(&mut b,key)?,
    ["delete-expression",key]=>expression_management::delete_expression(&mut b,key)?,
    ["deactivate-all","model"]=>expression_management::deactivate_all(&mut b)?,
    ["save-expression",..]=>{expression_editor::save(&mut b,&fields)?;},
    ["toggle",key]=>{let name=b.expressions.assets.iter().find(|(_,a)|a.key==*key).map(|(n,_)|n.clone()).ok_or_else(||err("Unknown expression"))?;let on=!b.expressions.active(&name);b.activate(&name,on,None,0.).map_err(|e|err(e.1))?;b.save_state()?;},
    ["trigger",key]=>{let h=b.expressions.hotkeys.iter().find(|h|hash(&h.id)==*key).cloned().ok_or_else(||err("Unknown hotkey"))?;b.trigger_hotkey(&h).map_err(|e|err(e.1))?;b.save_state()?;},
    ["bind",key,value]=>{if let Err(e)=&saved_bindings{return Err(err(e.to_string()))}if !b.expressions.hotkeys.iter().any(|h|hash(&h.id)==*key){return Err(err("Unknown hotkey"))}let v=shortcut_normalize(value.parse::<u32>()?).ok_or_else(||err("Invalid shortcut key"))?;
     if v!=0&&bindings.iter().any(|(k,x)|k!=*key&&*x==v){return Err(err("Shortcut already assigned"))}
     if let Err(e)=&saved_globals{return Err(err(e.to_string()))}if globals.contains(*key)&&v!=0{return Err(err("Disable Global before changing the shortcut"))}let mut next=bindings.clone();if v==0{next.remove(*key);globals.remove(*key);}else{next.insert((*key).to_owned(),v);}
     let p=ui_binding_path(&b).ok_or_else(||err("No writable profile"))?;outside_model(&p,&b.args.model)?;write_json(&p,&json!({"schema":"kawari-hotkeys-2","modelID":b.modelid,"bindings":next,"global":globals}))?;bindings=next;},
    ["import-vts","model"]=>{
     if let Err(e)=&saved_bindings{return Err(err(e.to_string()))}if let Err(e)=&saved_globals{return Err(err(e.to_string()))}
     let p=ui_binding_path(&b).ok_or_else(||err("No writable profile"))?;outside_model(&p,&b.args.model)?;
     let items:Vec<_>=b.expressions.hotkeys.iter().map(|h|(hash(&h.id),h.source_binding)).collect();
     let mut next=bindings.clone();let report=hotkey_import::merge(&mut next,&items);
     if report.added>0{write_json(&p,&json!({"schema":"kawari-hotkeys-2","modelID":b.modelid,"bindings":next,"global":globals}))?;bindings=next;}
     import_status=Some(report.summary());
    },
    ["global",key,value]=>{if let Err(e)=&saved_bindings{return Err(err(e.to_string()))}if let Err(e)=&saved_globals{return Err(err(e.to_string()))}if !b.expressions.hotkeys.iter().any(|h|hash(&h.id)==*key){return Err(err("Unknown hotkey"))}let mut next=globals.clone();match *value{"1"=>{if !bindings.contains_key(*key){return Err(err("Assign a shortcut first"))}next.insert((*key).to_owned());},"0"=>{next.remove(*key);},_=>return Err(err("Invalid global scope"))}let p=ui_binding_path(&b).ok_or_else(||err("No writable profile"))?;outside_model(&p,&b.args.model)?;write_json(&p,&json!({"schema":"kawari-hotkeys-2","modelID":b.modelid,"bindings":bindings,"global":next}))?;globals=next;},
    _=>return Err(err("Invalid UI request"))}Ok(())
  })();let _=fs::remove_file(&req);status=match result{Ok(())=>import_status.unwrap_or_else(||"OK".into()),Err(e)=>ui_text(&e.to_string())};
 }
 let desired:Vec<_>=if saved_bindings.is_ok()&&saved_globals.is_ok(){b.expressions.hotkeys.iter().filter_map(|h|{let key=hash(&h.id);let chord=*bindings.get(&key)?;if !globals.contains(&key){return None}Some(global_shortcuts::Binding{id:hash(&format!("{}:{key}:{chord}",b.modelid)),key,chord,label:format!("{}: {}",ui_text(&b.modelname),ui_text(&h.name))})}).collect()}else{vec![]};
 global.update(desired);for key in global.poll(){b.refresh();if let Some(h)=b.expressions.hotkeys.iter().find(|h|hash(&h.id)==key).cloned(){if let Err(e)=b.trigger_hotkey(&h){eprintln!("[global-shortcuts] {}",e.1);}else if let Err(e)=b.save_state(){eprintln!("[global-shortcuts] {e}");}}}
 let mut out=format!("M\t{}\n",b.modelid);if global_shortcuts::local_fallback(){out.push_str("L\t1\n");}if !global.status().is_empty(){out.push_str(&format!("G\t{}\n",ui_text(global.status())));}for (n,a) in &b.expressions.assets{out.push_str(&format!("E\t{}\t{}\t{}\n",a.key,b.expressions.active(n) as u8,ui_text(n)));}
 let now=Instant::now();let sample=expression_management::monotonic_ms();for(n,a)in &b.expressions.assets{let g=b.expressions.management.group(n);let p=b.expressions.management.protected(n);out.push_str(&format!("X\t{}\t{}\t{}\n",a.key,p as u8,ui_text(g)));if !b.snap.params.is_empty(){if let (Some(ms),Some(sample))=(expression_management::remaining_ms(&b.expressions,n,now),sample){out.push_str(&format!("Q\t{}\t{}\t{}\n",ui_text(n),ms,sample));}}}
 out.push_str(&expression_editor::details(&b));
 for h in &b.expressions.hotkeys{let k=hash(&h.id);out.push_str(&format!("H\t{}\t{}\t{}\t{}\n",k,bindings.get(&k).copied().unwrap_or(0),b.expressions.active(&h.file) as u8,ui_text(&h.name)));if globals.contains(&k){out.push_str(&format!("S\t{k}\n"));}}
 if *last!=out||!state.exists(){let mut f=tempfile::NamedTempFile::new_in(&dir)?;f.write_all(out.as_bytes())?;f.persist(state)?;*last=out;}
 if !status.is_empty(){let mut f=tempfile::NamedTempFile::new_in(&dir)?;writeln!(f,"{status}")?;f.persist(dir.join("expression-result.txt"))?;}Ok(())
}
fn run()->AnyResult<()> {let args=Args::parse()?;let _plugin_lock=if let Some(p)=&args.plugin_store{use std::os::fd::AsRawFd;let parent=p.parent().filter(|x|!x.as_os_str().is_empty()).unwrap_or(Path::new("."));fs::create_dir_all(parent)?;let f=fs::OpenOptions::new().read(true).write(true).create(true).mode(0o600).custom_flags(libc::O_NOFOLLOW|libc::O_NONBLOCK).open(p.with_extension("lock"))?;let m=f.metadata()?;if !m.is_file()||m.uid()!=unsafe{libc::getuid()}||m.mode()&0o077!=0{return Err(err("Expected private plugin lock"))}if unsafe{libc::flock(f.as_raw_fd(),libc::LOCK_EX|libc::LOCK_NB)}!=0{return Err(err("Plugin store is already in use"))}Some(f)}else{None};let control=args.control.clone();if let Some(d)=&control{let m=fs::symlink_metadata(d)?;if !m.is_dir()||m.uid()!=unsafe{libc::getuid()}||m.mode()&0o077!=0{return Err(err("Expected private API control directory"))}}let listener=TcpListener::bind(("127.0.0.1",args.port))?;listener.set_nonblocking(true)?;let mut bridge=Bridge::new(args)?;bridge.restore()?;if let Some(p)=&bridge.args.tokens{if p.exists(){save_tokens(p,&bridge.tokens)?;}}let port=bridge.args.port;let sock=bridge.args.sock.clone();let instance=random()?[..32].to_string();let broadcast=envelope("VTubeStudioAPIStateBroadcast",json!({"active":true,"port":port,"instanceID":instance,"windowTitle":"Kawari"}),"VTubeStudioAPIStateBroadcast");println!("[expressions] {} files / {} supported hotkeys",bridge.expressions.assets.len(),bridge.expressions.hotkeys.len());println!("[vts-api] Listening on ws://127.0.0.1:{port}; model {}; Rust backend",bridge.modelname);io::stdout().flush()?;let bridge=Arc::new(Mutex::new(bridge));let udp=UdpSocket::bind(("127.0.0.1",0))?;let count=Arc::new(AtomicUsize::new(0));let mut id=0usize;let mut next=Instant::now();let mut next_ui=Instant::now();let mut ui_last=String::new();let mut input_last=String::new();let mut global=global_shortcuts::Runtime::default();let mut obs=bridge.lock().unwrap().args.plugin_store.as_ref().map(|p|obs::Runtime::new(p.with_file_name("obs.json")));while control.as_ref().map_or_else(||sock.exists(),|d|d.is_dir()){if let Some(d)=&control{poll_control(d,&bridge,&mut global)?;}if let Some(obs)=&mut obs{if let Err(e)=obs.poll(&bridge){eprintln!("[obs] {e}");}}if Instant::now()>=next_ui{if let Err(e)=ui_poll(&bridge,&mut ui_last,&mut input_last,&mut global){eprintln!("[expression-ui] {e}");}next_ui=Instant::now()+Duration::from_millis(200);}if Instant::now()>=next{let mut msg=broadcast.clone();msg["timestamp"]=json!(stamp());let _=udp.send_to(msg.to_string().as_bytes(),("127.0.0.1",47779));next=Instant::now()+Duration::from_secs(2);}match listener.accept(){Ok((stream,_))=>{if count.load(Ordering::Relaxed)>=16{continue}count.fetch_add(1,Ordering::Relaxed);id=id.wrapping_add(1);let b=bridge.clone();let slot=Slot(count.clone());thread::spawn(move||client(stream,b,id,slot));},Err(e)if e.kind()==io::ErrorKind::WouldBlock=>thread::sleep(Duration::from_millis(10)),Err(e)=>return Err(e.into())}}Ok(())}
fn main(){if std::env::args().nth(1).as_deref()==Some("--help"){println!("studio-vts-api --socket PATH [--plugin-store PATH | --allow-plugin NAME] [--model PATH] [--control-dir DIR] (persistent API protocol 1; Kawari expression UI protocol 3; plugin manager protocol 1)");return}if let Err(e)=run(){eprintln!("[vts-api] {e}");std::process::exit(1);}}
#[cfg(test)]mod tests{use super::*;#[test]fn stable_hash(){assert_eq!(hash("abc"),"ba7816bf8f01cfea414140de5dae2223");}#[test]fn rejects_invalid_numbers(){assert!(num(&json!(true)).is_none());assert!(num(&json!(1000001)).is_none());}#[test]fn reversed_mapping(){let mut s=Snapshot::default();let d=tempfile::tempdir().unwrap();let p=d.path().join("parameters.tsv");fs::write(&p,"P\tParamMouthPucker\t0\t2\t0\t0\nM\tMouthPucker\tParamMouthPucker\t0\t-0.15\t0\t2\n").unwrap();s.refresh(&p);assert_eq!(s.maps["MouthPucker"][0].2,-0.15);}}

#[cfg(test)]mod shortcut_tests{use super::*;#[test]fn migration_and_ranges(){assert_eq!(shortcut_normalize(17),shortcut_encode(294,2));assert_eq!(shortcut_normalize(0),Some(0));assert_eq!(shortcut_encode(85,3),Some(0x10655));assert_ne!(shortcut_encode(53,0),shortcut_encode(325,0));assert_eq!(shortcut_encode(256,0),None);assert_eq!(shortcut_encode(340,0),None);assert_eq!(shortcut_normalize(127),None);assert_eq!(shortcut_normalize(0xffffffff),None);for k in 0..512{for m in 0..16{if let Some(v)=shortcut_encode(k,m){assert_eq!(shortcut_normalize(v),Some(v));}}}}}

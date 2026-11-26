use std::{io::{BufRead,BufReader},process::{Child,Command,Stdio},sync::mpsc::{self,Receiver},thread};

#[derive(Clone,Debug,PartialEq,Eq)]
pub struct Binding {pub id:String,pub key:String,pub chord:u32,pub label:String}
#[derive(Default)]
pub struct Runtime {desired:Vec<Binding>,child:Option<Child>,rx:Option<Receiver<String>>,status:String}
fn wayland_session()->bool{match std::env::var_os("XDG_SESSION_TYPE"){Some(s)=>s=="wayland",None=>std::env::var_os("WAYLAND_DISPLAY").is_some()}}
pub fn local_fallback()->bool{let setting=std::env::var("KAWARI_GLOBAL_SHORTCUTS");setting.as_deref()==Ok("0")||(wayland_session()&&setting.as_deref()!=Ok("1"))}
impl Runtime {
    pub fn stop(&mut self){self.rx=None;if let Some(mut c)=self.child.take(){let _=c.kill();let _=c.wait();}self.desired.clear();self.status.clear();}
    pub fn update(&mut self,desired:Vec<Binding>){
        if desired==self.desired{return}self.stop();self.desired=desired;
        if self.desired.is_empty(){return}
        if std::env::var("KAWARI_GLOBAL_SHORTCUTS").as_deref()==Ok("0"){self.status="Global shortcuts disabled for this run".into();return}
        if local_fallback(){self.status="Wayland global shortcuts deferred; shortcuts are local".into();return}
        let result=(||->std::io::Result<Child>{let helper=std::env::current_exe()?.with_file_name("kawari-global-shortcuts");let mut cmd=Command::new(helper);for b in &self.desired{cmd.arg(&b.id).arg(b.chord.to_string()).arg(&b.label);}cmd.stdout(Stdio::piped()).stderr(Stdio::inherit()).spawn()})();
        match result {Ok(mut child)=>{let stdout=child.stdout.take().unwrap();let (tx,rx)=mpsc::sync_channel(128);thread::spawn(move||{for line in BufReader::new(stdout).lines(){let Ok(line)=line else{break};if line.len()<=2048{match tx.try_send(line){Ok(())|Err(mpsc::TrySendError::Full(_))=>{},Err(mpsc::TrySendError::Disconnected(_))=>break}}}});self.child=Some(child);self.rx=Some(rx);self.status="Registering global shortcuts...".into();},Err(e)=>self.status=format!("Global shortcut helper unavailable: {e}; run scripts/build-global-shortcuts.sh")}
    }
    pub fn poll(&mut self)->Vec<String>{let mut events=Vec::new();
        if let Some(rx)=&self.rx{for line in rx.try_iter().take(128){if let Some(s)=line.strip_prefix("STATUS "){self.status=s.chars().map(|c|if c.is_control(){' '}else{c}).collect();eprintln!("[global-shortcuts] {}",self.status);}else if let Some(id)=line.strip_prefix("EVENT "){if let Some(b)=self.desired.iter().find(|b|b.id==id){events.push(b.key.clone());}}}}
        if let Some(c)=&mut self.child{if let Ok(Some(s))=c.try_wait(){self.status=format!("{} (helper exited: {s})",self.status);self.child=None;self.rx=None;events.clear();}}
        events
    }
    pub fn status(&self)->&str{&self.status}
}
impl Drop for Runtime{fn drop(&mut self){self.stop();}}

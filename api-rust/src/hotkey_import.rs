//! Convert VTS RestrictedRawKey names to Kawari's GLFW chord format.
use std::collections::{BTreeMap,BTreeSet};

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum Candidate { Empty, Unsupported, Chord(u32) }

pub fn shortcut_encode(k:u32,m:u32)->Option<u32>{
    let key=matches!(k,32|39|44..=57|59|61|65..=93|96|161|162|256..=269|280..=284|290..=314|320..=336|348);
    if key&&m<=15&&!(k==256&&m==0){Some(0x10000|(m<<9)|k)}else{None}
}

fn digit(s:&str)->Option<u32>{let b=s.as_bytes();if b.len()==1&&b[0].is_ascii_digit(){Some(u32::from(b[0]-b'0'))}else{None}}
fn key(s:&str)->Option<u32>{
    if s.len()==1&&s.as_bytes()[0].is_ascii_uppercase(){return Some(u32::from(s.as_bytes()[0]))}
    if let Some(n)=s.strip_prefix("Numpad").and_then(digit){return Some(320+n)}
    if let Some(n)=s.strip_prefix('N').and_then(digit){return Some(48+n)}
    if let Some(n)=s.strip_prefix('F').and_then(|x|x.parse::<u32>().ok()).filter(|n|(1..=24).contains(n)){return Some(289+n)}
    Some(match s{
        "Tab"=>258,"CapsLock"=>280,"Escape"=>256,"Space"=>32,
        "Left"=>263,"Up"=>265,"Right"=>262,"Down"=>264,
        "Print"=>283,"Delete"=>261,"PageUp"=>266,"PageDown"=>267,
        "Multiply"=>332,"Add"=>334,"Subtract"=>333,"Decimal"=>330,"Divide"=>331,
        "NumLock"=>282,"ScrollLock"=>281,_=>return None
    })
}

pub fn candidate(names:&[&str])->Candidate{
    let mut modifiers=0;let mut primary=None;let mut any=false;
    for &name in names{
        if name.is_empty(){continue}any=true;
        let m=match name{"LeftShift"|"RightShift"=>1,"LeftControl"|"RightControl"=>2,"Alt"=>4,"LeftWindows"|"RightWindows"=>8,_=>0};
        if m!=0{if modifiers&m!=0{return Candidate::Unsupported}modifiers|=m;continue}
        let Some(k)=key(name)else{return Candidate::Unsupported};
        if primary.replace(k).is_some(){return Candidate::Unsupported}
    }
    if !any{return Candidate::Empty}
    match primary.and_then(|k|shortcut_encode(k,modifiers)){Some(c)=>Candidate::Chord(c),None=>Candidate::Unsupported}
}

#[derive(Default,Debug,PartialEq,Eq)]
pub struct Report {pub added:usize,pub existing:usize,pub conflict:usize,pub unsupported:usize,pub empty:usize}
impl Report {pub fn summary(&self)->String{format!("Imported: {} | Existing: {} | Conflicts: {} | Unsupported: {} | No key: {}",self.added,self.existing,self.conflict,self.unsupported,self.empty)}}

// Only explicit imports fill empty bindings. Ambiguous source chords/IDs are skipped.
// Existing bindings always win; no global registration is imported.
pub fn merge(bindings:&mut BTreeMap<String,u32>,items:&[(String,Candidate)])->Report{
    let mut result=Report::default();let mut chord_counts=BTreeMap::new();let mut id_counts=BTreeMap::new();
    for (id,c) in items{*id_counts.entry(id.clone()).or_insert(0usize)+=1;if let Candidate::Chord(v)=c{*chord_counts.entry(*v).or_insert(0usize)+=1;}}
    let mut used:BTreeSet<u32>=bindings.values().copied().collect();
    for (id,c) in items{
        if bindings.contains_key(id){result.existing+=1;continue}
        match c {
            Candidate::Empty=>result.empty+=1,
            Candidate::Unsupported=>result.unsupported+=1,
            Candidate::Chord(v)=>{
                if id_counts[id]>1||chord_counts[v]>1||used.contains(v){result.conflict+=1;continue}
                bindings.insert(id.clone(),*v);used.insert(*v);result.added+=1;
            }
        }
    }result
}

#[cfg(test)]mod tests{
    use super::*;
    #[test]fn exact_key_conversion(){
        for n in 0..10{assert_eq!(candidate(&[&format!("Numpad{n}")]),Candidate::Chord(0x10000+320+n));assert_eq!(candidate(&[&format!("N{n}")]),Candidate::Chord(0x10000+48+n));}
        for n in 1..25{assert_eq!(candidate(&[&format!("F{n}")]),Candidate::Chord(0x10000+289+n));}
        assert_eq!(candidate(&["U","LeftControl","RightShift"]),Candidate::Chord(0x10000+3*512+85));
        for (name,k) in [("Multiply",332),("Subtract",333),("Add",334),("Divide",331),("Decimal",330),("PageUp",266)]{assert_eq!(candidate(&[name]),Candidate::Chord(0x10000+k));}
        assert_eq!(candidate(&["LeftWindows","A"]),Candidate::Chord(0x10000+8*512+65));
        for a in [vec!["A","B"],vec!["LeftMouseButton"],vec!["Escape"],vec!["LeftControl"],vec!["LeftShift","RightShift","U"],vec!["Mystery","A"]]{assert_eq!(candidate(&a),Candidate::Unsupported);}
        assert_eq!(candidate(&["","",""]),Candidate::Empty);
    }
    #[test]fn merge_preserves_and_skips(){
        use Candidate::*;let mut b=BTreeMap::from([("existing".into(),10)]);
        let items=vec![("existing".into(),Chord(11)),("occupied".into(),Chord(10)),("dup1".into(),Chord(12)),("dup2".into(),Chord(12)),("new".into(),Chord(13)),("empty".into(),Empty),("bad".into(),Unsupported),("same-id".into(),Chord(14)),("same-id".into(),Chord(15))];
        assert_eq!(merge(&mut b,&items),Report{added:1,existing:1,conflict:5,unsupported:1,empty:1});assert_eq!(b,BTreeMap::from([("existing".into(),10),("new".into(),13)]));
        assert_eq!(merge(&mut b,&items).added,0);
    }
}

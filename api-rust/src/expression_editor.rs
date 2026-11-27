//! Private profile overlays. Original model files are never written.
use super::*;

fn item_id_ok(id:&str)->bool{id.len()==12&&id.starts_with("asset-")&&id.as_bytes()[6..].iter().all(u8::is_ascii_alphanumeric)}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct ModelPosition {
    x: f64,
    y: f64,
    speed: f64,
    scale: Option<f64>,
}
impl ModelPosition {
    fn parse(value: &V) -> AnyResult<Self> {
        let field = |key, lo, hi| {
            num(&value[key]).filter(|n| (lo..=hi).contains(n))
                .ok_or_else(|| err(format!("Invalid model position {key}")))
        };
        Ok(Self {
            x: field("X", -10_000., 10_000.)?,
            y: field("Y", -10_000., 10_000.)?,
            speed: field("Speed", 0.05, 20.)?,
            scale: value.get("Scale").map(|_| field("Scale", 0.2, 5.)).transpose()?,
        })
    }
    pub fn packet(self) -> String {
        match self.scale {
            Some(scale) => format!(" position V {:.8} {:.8} {:.8} {:.8}", self.x, self.y, self.speed, scale),
            None => format!(" position P {:.8} {:.8} {:.8}", self.x, self.y, self.speed),
        }
    }
}

pub(super) fn parse_asset(name:&str,v:&V)->AnyResult<Asset>{
    if v.get("Type").is_some()&&v["Type"]!="Live2D Expression"{return Err(err("Invalid expression type"))}
    let mut terms=vec![];let mut seen=BTreeSet::new();
    for x in arr(v,"Parameters"){
        let n=strv(x,"Id");let b=match x["Blend"].as_str().unwrap_or("Add"){"Add"=>"A","Multiply"=>"M","Overwrite"=>"O",_=>return Err(err("Invalid blend"))};
        if !safe_id(n)||!seen.insert(n){return Err(err("Invalid parameter ID"))}
        terms.push((n.to_owned(),b,num(&x["Value"]).ok_or_else(||err("Invalid value"))?));
    }
    let mut items=vec![];let mut seen_items=BTreeSet::new();
    if let Some(value)=v.get("KawariItems"){
        let rows=value.as_array().ok_or_else(||err("Invalid item actions"))?;
        if rows.len()>32{return Err(err("Too many item actions"))}
        for row in rows{let id=strv(row,"Id");let show=row["Visible"].as_bool().ok_or_else(||err("Invalid item visibility"))?;
            if !item_id_ok(id)||!seen_items.insert(id){return Err(err("Invalid or duplicate item ID"))}items.push((id.to_owned(),show));}
    }
    let position=v.get("KawariPosition").map(ModelPosition::parse).transpose()?;
    if (terms.is_empty()&&items.is_empty()&&position.is_none())||terms.len()>256{return Err(err("Select parameters, items or a model position"))}
    let fade=optional(v,"FadeInTime",0.5,0.,10.).ok_or_else(||err("Invalid fade in"))?;
    let fade_out=optional(v,"FadeOutTime",fade,0.,10.).ok_or_else(||err("Invalid fade out"))?;
    Ok(Asset{key:hash(name),terms,items,position,fade,fade_out})
}
fn path(b:&Bridge)->AnyResult<PathBuf>{
    let p=b.args.state.as_ref().ok_or_else(||err("No writable profile"))?.with_extension("expressions.json");
    outside_model(&p,&b.args.model)?;
    if [&b.args.state,&b.args.tokens,&b.args.plugin_store].into_iter().flatten().any(|x|x==&p){return Err(err("Expression profile path collision"))}
    Ok(p)
}
fn catalog(b:&Bridge)->AnyResult<BTreeMap<String,V>>{
    let p=path(b)?;if !p.exists(){return Ok(BTreeMap::new())}
    let v=read_json(&p)?;
    if v["schema"]!="kawari-expressions-1"||v["modelID"]!=b.modelid{return Err(err("Invalid expression profile"))}
    let entries=v["expressions"].as_object().ok_or_else(||err("Invalid expression entries"))?;
    if entries.len()>1024{return Err(err("Too many expression entries"))}
    let mut out=BTreeMap::new();for (n,v) in entries{if !valid_file(n){return Err(err("Invalid expression filename"))}parse_asset(n,v)?;out.insert(n.clone(),v.clone());}Ok(out)
}
pub(super) fn valid_file(n:&str)->bool{n.ends_with(".exp3.json")&&n.len()<=512&&!n.chars().any(char::is_control)&&!n.starts_with('/')&&!n.split('/').any(|x|x==".."||x=="."||x.is_empty())&&!n.contains('\\')}
fn new_file(name:&str)->AnyResult<String>{
    let n=name.trim();if n.is_empty()||n.len()>160||n=="."||n==".."||n.contains('/')||n.contains('\\')||n.chars().any(char::is_control)||n.contains("##"){return Err(err("Invalid expression name"))}
    Ok(if n.ends_with(".exp3.json"){n.into()}else{format!("{n}.exp3.json")})
}
pub(super) fn load(b:&mut Bridge)->AnyResult<()>{
    if b.args.state.is_none(){return Ok(())}
    let mut next=b.expressions.assets.clone();
    next.retain(|n,_|!b.expressions.management.removed(n));
    for(n,v)in catalog(b)?{if !b.expressions.management.removed(&n){next.insert(n.clone(),parse_asset(&n,&v)?);}}
    if next.len()>64{return Err(err("Expression limit exceeded"))}
    b.expressions.assets=next;Ok(())
}
pub(super) fn details(b:&Bridge)->String{
    let mut out=String::new();for a in b.expressions.assets.values(){
        out.push_str(&format!("D\t{}\t{}\t{}\n",a.key,a.fade,a.fade_out));
        for(id,blend,val)in &a.terms{out.push_str(&format!("T\t{}\t{}\t{}\t{}\n",a.key,id,blend,val));}
        for(id,show)in &a.items{out.push_str(&format!("I\t{}\t{}\t{}\n",a.key,id,u8::from(*show)));}
        if let Some(p)=a.position {
            out.push_str(&format!("P\t{}\t{}\t{}\t{}",a.key,p.x,p.y,p.speed));
            if let Some(scale)=p.scale {out.push_str(&format!("\t{scale}"));}
            out.push('\n');
        }
    }out
}
pub(super) fn save(b:&mut Bridge,fields:&[&str])->AnyResult<()>{
    // save-expression <new|hash> <name> <fadeIn> <fadeOut> <id> <blend> <value> ...
    if fields.len()<8||(fields.len()-5)%3!=0||(fields.len()-5)/3>289{return Err(err("Invalid expression request"))}
    let name=if fields[1]=="new"{
        let n=new_file(fields[2])?;if b.expressions.assets.contains_key(&n)||b.expressions.management.removed(&n){return Err(err("Expression name already exists or was retired"))}if b.expressions.assets.len()>=64{return Err(err("Expression limit reached"))}n
    }else{
        let n=b.expressions.assets.iter().find(|(_,a)|a.key==fields[1]).map(|(n,_)|n.clone()).ok_or_else(||err("Unknown expression"))?;
        if b.expressions.active(&n){return Err(err("Deactivate the expression before editing"))}n
    };
    if !valid_file(&name){return Err(err("Invalid expression filename"))}
    let mut terms=vec![];let mut items=vec![];let mut position=None;
    for row in fields[5..].chunks_exact(3){
        let id=row[0];if row[1]=="I"{let show=match row[2]{"1"=>true,"0"=>false,_=>return Err(err("Invalid item visibility"))};items.push(json!({"Id":id,"Visible":show}));continue;}
        if matches!(row[1],"P"|"V") {
            if id!="position"||position.is_some(){return Err(err("Invalid or duplicate model position"))}
            let mut values=row[2].split_ascii_whitespace().map(|s|s.parse::<f64>().ok().filter(|n|n.is_finite()));
            let (Some(Some(x)),Some(Some(y)),Some(Some(speed)))=(values.next(),values.next(),values.next())
                else{return Err(err("Invalid model position"))};
            let mut view=json!({"X":x,"Y":y,"Speed":speed});
            if row[1]=="V" {
                let Some(Some(scale))=values.next() else{return Err(err("Invalid model scale"))};
                view["Scale"]=json!(scale);
            }
            if values.next().is_some(){return Err(err("Invalid model position"))}
            position=Some(view);
            continue;
        }
        if !b.snap.params.contains_key(id){return Err(err("Missing rig parameter"))}
        let blend=match row[1]{"A"=>"Add","M"=>"Multiply","O"=>"Overwrite",_=>return Err(err("Invalid blend"))};
        let value=row[2].parse::<f64>()?;if !value.is_finite()||value.abs()>1e6{return Err(err("Invalid value"))}
        terms.push(json!({"Id":id,"Blend":blend,"Value":value}));
    }
    let fade=fields[3].parse::<f64>()?;let fade_out=fields[4].parse::<f64>()?;
    let mut v=json!({"Type":"Live2D Expression","FadeInTime":fade,"FadeOutTime":fade_out,"Parameters":terms,"KawariItems":items});
    if let Some(p)=position {v["KawariPosition"]=p;}
    let a=parse_asset(&name,&v)?;
    // The native datagram must fit even for a maximally sized draft.
    if a.terms.iter().map(|(n,_,v)|n.len()+v.to_string().len()+4).sum::<usize>()+a.items.len()*17+a.position.map_or(0,|p|p.packet().len())+100>LIMIT{return Err(err("Expression too large for renderer"))}
    let mut entries=catalog(b)?;entries.insert(name.clone(),v);if entries.len()>1024{return Err(err("Expression archive limit reached"))}
    let data=json!({"schema":"kawari-expressions-1","modelID":b.modelid,"expressions":entries});
    if serde_json::to_vec_pretty(&data)?.len()>8*1024*1024-1{return Err(err("Expression archive size limit reached"))}
    write_json(&path(b)?,&data)?;
    b.expressions.assets.insert(name,a);Ok(())
}

#[cfg(test)]
mod tests {
    use std::os::unix::fs::DirBuilderExt;
    use super::*;
    #[test]
    fn scaled_position_expression() {
        let mut model=json!({"KawariPosition":{"X":12,"Y":-0.25,"Speed":0.5,"Scale":1.75}});
        let asset=parse_asset("Move.exp3.json",&model).unwrap();
        assert_eq!(asset.position.unwrap().packet()," position V 12.00000000 -0.25000000 0.50000000 1.75000000");
        model["KawariPosition"]["Scale"]=json!(0.0);
        assert!(parse_asset("Move.exp3.json",&model).is_err());
    }

    #[test]
    fn position_only_expression() {
        let asset = parse_asset("Chat.exp3.json", &json!({
            "Type": "Live2D Expression",
            "KawariPosition": {"X": -0.75, "Y": 0.25, "Speed": 0.5}
        })).unwrap();
        assert!(asset.terms.is_empty() && asset.items.is_empty());
        assert_eq!(asset.position.unwrap().packet(), " position P -0.75000000 0.25000000 0.50000000");
        let old = parse_asset("Smile.exp3.json", &json!({"Parameters":[{"Id":"Smile","Value":1}]})).unwrap();
        assert!(old.position.is_none());
        assert!(parse_asset("Invalid.exp3.json", &json!({"KawariPosition":{"X":0,"Y":0,"Speed":0}})).is_err());
    }

    #[test]
    fn valid_names_and_blends(){
        assert_eq!(new_file("Улыбка").unwrap(),"Улыбка.exp3.json");
        assert!(new_file("../outside").is_err());assert!(new_file("x\ty").is_err());
        let v=json!({"Type":"Live2D Expression","FadeInTime":0.2,"FadeOutTime":0.7,"Parameters":[{"Id":"ParamA","Blend":"Add","Value":2},{"Id":"ParamB","Blend":"Multiply","Value":1.5}]});
        let a=parse_asset("Test.exp3.json",&v).unwrap();assert_eq!(a.fade,0.2);assert_eq!(a.fade_out,0.7);assert_eq!(a.terms.len(),2);
        let mut bad=v.clone();bad["Parameters"][1]["Id"]=json!("ParamA");assert!(parse_asset("Test.exp3.json",&bad).is_err());
        bad=v.clone();bad["FadeOutTime"]=json!(11);assert!(parse_asset("Test.exp3.json",&bad).is_err());
    }
    #[test]
    fn save_and_restore_profile_overlay(){
        let root=tempfile::tempdir().unwrap();let model_dir=root.path().join("model");fs::create_dir(&model_dir).unwrap();
        let model=model_dir.join("Test.model3.json");write_json(&model,&json!({"FileReferences":{"Expressions":[{"File":"Original.exp3.json"}]}})).unwrap();
        let original=model_dir.join("Original.exp3.json");write_json(&original,&json!({"Parameters":[{"Id":"ParamA","Value":1,"Blend":"Overwrite"}]})).unwrap();let bytes=fs::read(&original).unwrap();
        let ipc=root.path().join("ipc");fs::DirBuilder::new().mode(0o700).create(&ipc).unwrap();
        let sock=ipc.join("input.sock");let _rx=UnixDatagram::bind(&sock).unwrap();
        let args=Args{plugin_store:None,control:None,sock,model:Some(model),port:8001,allowed:BTreeSet::new(),state:Some(root.path().join("state.json")),tokens:None,restore:false};
        let mut b=Bridge::new(args.clone()).unwrap();b.snap.params.insert("ParamA".into(),parameter("ParamA",-10.,10.,0.,0.,None));
        save(&mut b,&["save-expression","new","Улыбка","0.2","0.7","ParamA","O","3"]).unwrap();
        let name="Улыбка.exp3.json";assert!(b.expressions.assets.contains_key(name));
        let key=b.expressions.assets["Original.exp3.json"].key.clone();
        save(&mut b,&["save-expression",&key,"","0.3","0.4","ParamA","A","4"]).unwrap();
        assert!(save(&mut b,&["save-expression","new","Улыбка","0.2","0.7","ParamA","O","3"]).is_err());
        assert!(save(&mut b,&["save-expression","new","Invalid","0.2","0.7","Missing","O","3"]).is_err());
        b.expressions.active.insert(name.into(),None);let key=b.expressions.assets[name].key.clone();
        assert!(save(&mut b,&["save-expression",&key,"","0","0","ParamA","O","4"]).is_err());
        save(&mut b,&["save-expression","new","Item only","0.2","0.7","asset-aB12cD","I","1"]).unwrap();
        save(&mut b,&["save-expression","new","Combined","0.2","0.7","ParamA","O","3","asset-aB12cD","I","0"]).unwrap();
        assert!(save(&mut b,&["save-expression","new","Bad","0","0","asset-aB12cD","I","2"]).is_err());
        assert!(save(&mut b,&["save-expression","new","Bad","0","0","asset-aB12cD","I","1","asset-aB12cD","I","0"]).is_err());
        let b=Bridge::new(args).unwrap();assert_eq!(b.expressions.assets["Item only.exp3.json"].items,vec![("asset-aB12cD".to_owned(),true)]);assert_eq!(b.expressions.assets["Combined.exp3.json"].items,vec![("asset-aB12cD".to_owned(),false)]);assert_eq!(b.expressions.assets[name].fade_out,0.7);assert_eq!(b.expressions.assets["Original.exp3.json"].terms[0].2,4.0);assert_eq!(fs::read(original).unwrap(),bytes);
    }
    #[test]
    fn edited_fades_reach_renderer_from_all_activation_paths(){
        let root=tempfile::tempdir().unwrap();let model_dir=root.path().join("model");fs::create_dir(&model_dir).unwrap();
        let model=model_dir.join("Test.model3.json");let name="Test.exp3.json";
        write_json(&model,&json!({"FileReferences":{"Expressions":[{"File":name}]}})).unwrap();
        write_json(&model_dir.join(name),&json!({"Parameters":[{"Id":"ParamA","Value":10,"Blend":"Overwrite"}]})).unwrap();
        write_json(&model_dir.join("Test.vtube.json"),&json!({"Hotkeys":[{"Action":"ToggleExpression","File":name,"Name":"Test","HotkeyID":"test-key","FadeSecondsAmount":0.1}]})).unwrap();
        let ipc=root.path().join("ipc");fs::DirBuilder::new().mode(0o700).create(&ipc).unwrap();
        let sock=ipc.join("input.sock");let rx=UnixDatagram::bind(&sock).unwrap();rx.set_read_timeout(Some(Duration::from_secs(1))).unwrap();
        fs::write(ipc.join("parameters.tsv"),"P\tParamA\t0\t10\t0\t0\n").unwrap();
        let args=Args{plugin_store:None,control:None,sock,model:Some(model),port:8001,allowed:BTreeSet::from(["tester".into()]),state:Some(root.path().join("state.json")),tokens:None,restore:false};
        let mut b=Bridge::new(args.clone()).unwrap();b.refresh();let key=b.expressions.assets[name].key.clone();
        save(&mut b,&["save-expression",&key,"","3","0.5","ParamA","O","10"]).unwrap();
        let mut b=Bridge::new(args).unwrap();b.refresh();
        let packet=||{let mut buf=[0u8;16384];let n=rx.recv(&mut buf).unwrap();String::from_utf8(buf[..n].to_vec()).unwrap()};
        let expected=format!("expr on2 {key} 3000 0 500 ParamA O 10");let off=format!("expr off {key}");
        b.activate(name,true,None,0.).unwrap();assert_eq!(packet(),expected);b.activate(name,false,None,0.).unwrap();assert_eq!(packet(),off);
        let h=b.expressions.hotkeys[0].clone();b.trigger_hotkey(&h).unwrap();assert_eq!(packet(),expected);b.trigger_hotkey(&h).unwrap();assert_eq!(packet(),off);
        let id=("Tester".to_owned(),"Test Dev".to_owned());b.tokens.insert(id.clone(),"test-token".into());let mut session=Session{id:1,identity:Some(id),token:"test-token".into()};
        b.dispatch("ExpressionActivationRequest",&json!({"expressionFile":name,"active":true}),&mut session).unwrap();assert_eq!(packet(),expected);
        b.dispatch("ExpressionActivationRequest",&json!({"expressionFile":name,"active":false}),&mut session).unwrap();assert_eq!(packet(),off);
        b.dispatch("HotkeyTriggerRequest",&json!({"hotkeyID":"test-key"}),&mut session).unwrap();assert_eq!(packet(),expected);
        b.dispatch("ExpressionActivationRequest",&json!({"expressionFile":name,"active":false,"fadeTime":0.2}),&mut session).unwrap();assert_eq!(packet(),format!("expr off2 {key} 200"));
        b.dispatch("ExpressionActivationRequest",&json!({"expressionFile":name,"active":true,"fadeTime":1}),&mut session).unwrap();assert_eq!(packet(),format!("expr on2 {key} 1000 0 500 ParamA O 10"));
        let modelid=b.modelid.clone();b.dispatch("KawariRewardExpressionRequest",&json!({"expressionFile":name,"modelID":modelid,"mode":"toggle"}),&mut session).unwrap();assert_eq!(packet(),off);
        b.dispatch("KawariRewardExpressionRequest",&json!({"expressionFile":name,"modelID":modelid,"mode":"timed","duration":8}),&mut session).unwrap();assert_eq!(packet(),format!("expr on2 {key} 3000 8000 500 ParamA O 10"));
    }

}

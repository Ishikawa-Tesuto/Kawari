//! OBS program scene tracking and playback of saved expression positions.
use super::{
    err,
    expression_editor::{valid_file, ModelPosition},
    outside_model, write_json, AnyResult, Bridge,
};
use data_encoding::BASE64;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs::{self, OpenOptions},
    io::{self, Read, Write},
    net::{Ipv4Addr, SocketAddr, TcpStream},
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};
use tungstenite::{client::client_with_config, protocol::WebSocketConfig, Message};

const MAX_DELAY: u64 = 30_000;
const POLL: Duration = Duration::from_millis(25);
const UI_POLL: Duration = Duration::from_millis(150);
const SCENE_LOOKUP_TIMEOUT: Duration = Duration::from_millis(500);
const CONNECT_TIMEOUT: Duration = Duration::from_secs(2);

#[derive(Clone)]
struct Settings {
    enabled: bool,
    port: u16,
    password: String,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            enabled: false,
            port: 4455,
            password: String::new(),
        }
    }
}
impl Settings {
    fn load(path: &Path) -> AnyResult<Self> {
        let Some(text) = read_private(path, 4096)? else {
            return Ok(Self::default());
        };
        let data: Value = serde_json::from_str(&text)?;
        let port = data["port"]
            .as_u64()
            .filter(|n| (1..=65535).contains(n))
            .ok_or_else(|| err("Invalid OBS port"))?;
        let password = data["password"]
            .as_str()
            .filter(|s| safe_text(s, 512))
            .ok_or_else(|| err("Invalid OBS password"))?;
        let enabled = data["enabled"]
            .as_bool()
            .ok_or_else(|| err("Invalid OBS setting"))?;
        Ok(Self {
            enabled,
            port: port as u16,
            password: password.to_owned(),
        })
    }
    fn save(&self, path: &Path) -> AnyResult<()> {
        write_json(
            path,
            &json!({"enabled":self.enabled,"port":self.port,"password":self.password}),
        )
    }
}

#[derive(Clone, Default)]
struct SceneState {
    status: String,
    scenes: BTreeSet<String>,
    program: Option<(String, Instant)>,
    lookup: Option<(String, Instant)>,
    transition_id: u64,
}
impl SceneState {
    fn update(&mut self, message: &Value, now: Instant) -> Option<String> {
        let data = &message["d"];
        match message["op"].as_u64() {
            Some(7)
                if data["requestId"] == "kawari-scenes"
                    && data["requestStatus"]["result"] == true =>
            {
                let snapshot = &data["responseData"];
                self.set_scenes(&snapshot["scenes"]);
                if self.program.is_none() && !self.resolving(now) {
                    self.set_program(&snapshot["currentProgramSceneName"], now);
                }
            }
            Some(7)
                if self.resolving(now)
                    && self
                        .lookup
                        .as_ref()
                        .is_some_and(|(id, _)| data["requestId"] == id.as_str())
                    && data["requestStatus"]["result"] == true =>
            {
                let scene = &data["responseData"];
                let name = scene
                    .get("sceneName")
                    .unwrap_or(&scene["currentProgramSceneName"]);
                if name.as_str().is_some_and(scene_name)
                    && self.program.as_ref().map(|(name, _)| name.as_str()) != name.as_str()
                {
                    let (_, started) = self.lookup.take().unwrap();
                    self.set_program(name, started);
                }
            }
            Some(5) if data["eventType"] == "SceneTransitionStarted" => {
                self.transition_id = self.transition_id.wrapping_add(1);
                let id = format!("kawari-program-{}", self.transition_id);
                self.lookup = Some((id.clone(), now));
                return Some(id);
            }
            Some(5) if data["eventType"] == "SceneListChanged" => {
                self.set_scenes(&data["eventData"]["scenes"])
            }
            Some(5) if data["eventType"] == "SceneNameChanged" => {
                let event = &data["eventData"];
                if let Some(old) = event["oldSceneName"].as_str() {
                    self.scenes.remove(old);
                    let name = &event["sceneName"];
                    if let Some(name) = name.as_str().filter(|name| scene_name(name)) {
                        self.scenes.insert(name.to_owned());
                    }
                    if self.program.as_ref().is_some_and(|(name, _)| name == old) {
                        self.set_program(name, now);
                    }
                }
            }
            Some(5) if data["eventType"] == "CurrentProgramSceneChanged" => {
                let started = self
                    .lookup
                    .take()
                    .map(|(_, started)| started)
                    .filter(|started| {
                        now.saturating_duration_since(*started) < SCENE_LOOKUP_TIMEOUT
                    })
                    .unwrap_or(now);
                self.set_program(&data["eventData"]["sceneName"], started);
            }
            _ => {}
        }
        None
    }
    fn resolving(&self, now: Instant) -> bool {
        self.lookup.as_ref().is_some_and(|(_, started)| {
            now.saturating_duration_since(*started) < SCENE_LOOKUP_TIMEOUT
        })
    }
    fn set_program(&mut self, value: &Value, now: Instant) {
        let next = value.as_str().filter(|name| scene_name(name));
        if self.program.as_ref().map(|(name, _)| name.as_str()) != next {
            self.program = next.map(|name| (name.to_owned(), now));
        }
    }
    fn set_scenes(&mut self, value: &Value) {
        self.scenes = value
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|s| s["sceneName"].as_str())
            .filter(|name| scene_name(name))
            .take(512)
            .map(str::to_owned)
            .collect();
    }
}

fn safe_text(value: &str, limit: usize) -> bool {
    value.len() <= limit && !value.chars().any(char::is_control)
}
fn scene_name(name: &str) -> bool {
    !name.is_empty() && safe_text(name, 256)
}

fn read_private(path: &Path, limit: u64) -> AnyResult<Option<String>> {
    let file = match OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(path)
    {
        Ok(file) => file,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error.into()),
    };
    let meta = file.metadata()?;
    if !meta.is_file()
        || meta.uid() != unsafe { libc::getuid() }
        || meta.mode() & 0o077 != 0
        || meta.len() > limit
    {
        return Err(err("Expected a private OBS file"));
    }
    let mut text = String::new();
    file.take(limit + 1).read_to_string(&mut text)?;
    if text.len() as u64 > limit {
        return Err(err("OBS file too large"));
    }
    Ok(Some(text))
}

fn authentication(password: &str, salt: &str, challenge: &str) -> String {
    let secret = BASE64.encode(&Sha256::digest(format!("{password}{salt}")));
    BASE64.encode(&Sha256::digest(format!("{secret}{challenge}")))
}

struct Connection {
    state: Arc<Mutex<SceneState>>,
    stop: Arc<AtomicBool>,
}
impl Connection {
    fn start(settings: Settings) -> Self {
        let state = Arc::new(Mutex::new(SceneState::default()));
        let stop = Arc::new(AtomicBool::new(false));
        let (worker_state, worker_stop) = (state.clone(), stop.clone());
        thread::spawn(move || {
            while !worker_stop.load(Ordering::Relaxed) {
                *worker_state.lock().unwrap() = SceneState {
                    status: "Connecting to OBS".into(),
                    ..SceneState::default()
                };
                if let Err(error) = listen(&settings, &worker_state, &worker_stop) {
                    *worker_state.lock().unwrap() = SceneState {
                        status: format!("OBS: {error}"),
                        ..SceneState::default()
                    };
                }
                for _ in 0..20 {
                    if worker_stop.load(Ordering::Relaxed) {
                        return;
                    }
                    thread::sleep(Duration::from_millis(100));
                }
            }
        });
        Self { state, stop }
    }
}
impl Drop for Connection {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

fn listen(settings: &Settings, state: &Mutex<SceneState>, stop: &AtomicBool) -> AnyResult<()> {
    let address = SocketAddr::from((Ipv4Addr::LOCALHOST, settings.port));
    let stream = TcpStream::connect_timeout(&address, CONNECT_TIMEOUT)?;
    stream.set_read_timeout(Some(CONNECT_TIMEOUT))?;
    stream.set_write_timeout(Some(CONNECT_TIMEOUT))?;
    let config = WebSocketConfig::default()
        .max_message_size(Some(1024 * 1024))
        .max_frame_size(Some(1024 * 1024));
    let (mut ws, _) = client_with_config(format!("ws://{address}"), stream, Some(config))
        .map_err(|_| err("WebSocket handshake failed"))?;
    let hello: Value = serde_json::from_str(ws.read()?.to_text()?)?;
    if hello["op"] != 0 || hello["d"]["rpcVersion"].as_u64().unwrap_or(0) < 1 {
        return Err(err("OBS WebSocket 5 is required"));
    }
    let mut identify = json!({"rpcVersion":1,"eventSubscriptions":(1 << 2) | (1 << 4)});
    if let Some(auth) = hello["d"].get("authentication") {
        if settings.password.is_empty() {
            return Err(err("Enter the OBS WebSocket password"));
        }
        let salt = auth["salt"]
            .as_str()
            .ok_or_else(|| err("Invalid OBS authentication challenge"))?;
        let challenge = auth["challenge"]
            .as_str()
            .ok_or_else(|| err("Invalid OBS authentication challenge"))?;
        identify["authentication"] = json!(authentication(&settings.password, salt, challenge));
    }
    ws.send(Message::Text(
        json!({"op":1,"d":identify}).to_string().into(),
    ))?;
    let identified = ws
        .read()
        .map_err(|_| err("Identification failed. Check the OBS password."))?;
    if identified
        .to_text()
        .ok()
        .and_then(|text| serde_json::from_str::<Value>(text).ok())
        .is_none_or(|v| v["op"] != 2)
    {
        return Err(err("Identification failed. Check the OBS password."));
    }
    if stop.load(Ordering::Relaxed) {
        return Ok(());
    }
    state.lock().unwrap().status = "Connected to OBS".into();
    ws.send(Message::Text(
        json!({"op":6,"d":{"requestType":"GetSceneList","requestId":"kawari-scenes"}})
            .to_string()
            .into(),
    ))?;
    ws.get_mut()
        .set_read_timeout(Some(Duration::from_millis(150)))?;
    while !stop.load(Ordering::Relaxed) {
        match ws.read() {
            Ok(Message::Text(text)) => {
                if let Ok(message) = serde_json::from_str(&text) {
                    let query = state.lock().unwrap().update(&message, Instant::now());
                    if let Some(id) = query {
                        ws.send(Message::Text(
                            json!({"op":6,"d":{"requestType":"GetCurrentProgramScene","requestId":id}})
                                .to_string().into(),
                        ))?;
                    }
                }
            }
            Ok(Message::Close(_)) => return Err(err("Disconnected. Reconnecting.")),
            Ok(Message::Ping(_)) => ws.flush()?,
            Ok(_) => {}
            Err(tungstenite::Error::Io(error))
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) => {}
            Err(_) => return Err(err("Disconnected. Reconnecting.")),
        }
    }
    Ok(())
}

#[derive(Clone)]
struct Rule {
    expression: String,
    delay: Duration,
}
fn rules_from(path: &Path) -> AnyResult<BTreeMap<String, Rule>> {
    let Some(text) = read_private(path, 512 * 1024)? else {
        return Ok(BTreeMap::new());
    };
    let data: Value = serde_json::from_str(&text)?;
    let records = data
        .as_object()
        .filter(|m| m.len() <= 128)
        .ok_or_else(|| err("Invalid OBS scene rules"))?;
    records
        .iter()
        .map(|(scene, rule)| {
            let expression = rule["expression"]
                .as_str()
                .filter(|n| valid_file(n))
                .ok_or_else(|| err("Invalid OBS expression"))?;
            let delay = rule["delayMs"]
                .as_u64()
                .filter(|n| *n <= MAX_DELAY)
                .ok_or_else(|| err("Invalid OBS delay"))?;
            if !scene_name(scene) {
                return Err(err("Invalid OBS scene"));
            }
            Ok((
                scene.clone(),
                Rule {
                    expression: expression.to_owned(),
                    delay: Duration::from_millis(delay),
                },
            ))
        })
        .collect()
}
fn save_rules(path: &Path, rules: &BTreeMap<String, Rule>) -> AnyResult<()> {
    let data: serde_json::Map<String, Value> = rules
        .iter()
        .map(|(scene, r)| {
            (
                scene.clone(),
                json!({"expression":r.expression,"delayMs":r.delay.as_millis() as u64}),
            )
        })
        .collect();
    write_json(path, &Value::Object(data))
}

#[derive(Default)]
struct Schedule {
    observed: Option<(String, Instant, Option<(Duration, ModelPosition)>)>,
    pending: Option<(Instant, Option<ModelPosition>)>,
}
impl Schedule {
    fn observe(
        &mut self,
        program: Option<(String, Instant)>,
        resolve: impl FnOnce(&str) -> Option<(Duration, ModelPosition)>,
    ) {
        let observed = program.map(|(scene, changed)| {
            let binding = resolve(&scene);
            (scene, changed, binding)
        });
        if self.observed == observed {
            return;
        }
        self.pending = observed
            .as_ref()
            .map(|(_, changed, binding)| match binding {
                Some((delay, position)) => (*changed + *delay, Some(*position)),
                None => (*changed, None),
            });
        self.observed = observed;
    }
    fn ready(&self, now: Instant) -> Option<Option<ModelPosition>> {
        self.pending
            .filter(|(due, _)| *due <= now)
            .map(|(_, target)| target)
    }
}

pub(super) struct Runtime {
    file: PathBuf,
    settings: Settings,
    connection: Option<Connection>,
    socket: PathBuf,
    rules_path: Option<PathBuf>,
    rules: BTreeMap<String, Rule>,
    schedule: Schedule,
    applied: bool,
    published: String,
    notice: String,
    next: Instant,
    next_ui: Instant,
}
impl Runtime {
    pub fn new(file: PathBuf) -> Self {
        let (settings, notice) = match Settings::load(&file) {
            Ok(settings) => (settings, String::new()),
            Err(_) => (
                Settings::default(),
                "Could not read saved OBS settings".into(),
            ),
        };
        let connection = settings
            .enabled
            .then(|| Connection::start(settings.clone()));
        Self {
            file,
            settings,
            connection,
            socket: PathBuf::new(),
            rules_path: None,
            rules: BTreeMap::new(),
            schedule: Schedule::default(),
            applied: false,
            published: String::new(),
            notice,
            next: Instant::now(),
            next_ui: Instant::now(),
        }
    }
    fn request(&mut self, fields: &[&str], bridge: &Bridge) -> AnyResult<()> {
        match fields {
            ["connect", port, password] => {
                let port = port
                    .parse::<u16>()
                    .ok()
                    .filter(|p| *p > 0)
                    .ok_or_else(|| err("Invalid OBS port"))?;
                if !safe_text(password, 512) {
                    return Err(err("Invalid OBS password"));
                }
                let password = if password.is_empty() && port == self.settings.port {
                    self.settings.password.clone()
                } else {
                    password.to_string()
                };
                let settings = Settings {
                    enabled: true,
                    port,
                    password,
                };
                settings.save(&self.file)?;
                self.connection = Some(Connection::start(settings.clone()));
                self.settings = settings;
                self.schedule = Schedule::default();
            }
            ["disconnect"] => {
                let mut settings = self.settings.clone();
                settings.enabled = false;
                settings.save(&self.file)?;
                self.connection = None;
                self.settings = settings;
                self.schedule = Schedule::default();
            }
            ["bind", scene, key, delay] if scene_name(scene) => {
                let path = self
                    .rules_path
                    .as_ref()
                    .ok_or_else(|| err("No model profile"))?;
                let delay = delay
                    .parse::<u64>()
                    .ok()
                    .filter(|d| *d <= MAX_DELAY)
                    .ok_or_else(|| err("Invalid OBS scene delay"))?;
                let name = bridge
                    .expressions
                    .assets
                    .iter()
                    .find(|(_, a)| a.key == *key && a.position.is_some())
                    .map(|(name, _)| name.clone())
                    .ok_or_else(|| err("Choose an expression with a model position"))?;
                if self.rules.len() >= 128 && !self.rules.contains_key(*scene) {
                    return Err(err("Too many OBS rules"));
                }
                let mut next = self.rules.clone();
                next.insert(
                    scene.to_string(),
                    Rule {
                        expression: name,
                        delay: Duration::from_millis(delay),
                    },
                );
                save_rules(path, &next)?;
                self.rules = next;
            }
            ["unbind", scene] if scene_name(scene) => {
                let path = self
                    .rules_path
                    .as_ref()
                    .ok_or_else(|| err("No model profile"))?;
                let mut next = self.rules.clone();
                next.remove(*scene);
                save_rules(path, &next)?;
                self.rules = next;
            }
            _ => return Err(err("Invalid OBS command")),
        }
        self.notice.clear();
        Ok(())
    }

    pub fn poll(&mut self, bridge: &Arc<Mutex<Bridge>>) -> AnyResult<()> {
        let now = Instant::now();
        if now < self.next {
            return Ok(());
        }
        self.next = now + POLL;
        let bridge = bridge.lock().map_err(|_| err("Model state unavailable"))?;
        if bridge.paused {
            return Ok(());
        }
        let dir = bridge
            .args
            .sock
            .parent()
            .ok_or_else(|| err("Missing renderer directory"))?;
        if self.socket != bridge.args.sock {
            self.rules_path = bridge
                .args
                .state
                .as_ref()
                .map(|p| p.with_extension("obs-scenes.json"))
                .filter(|p| {
                    outside_model(p, &bridge.args.model).is_ok()
                        && [&bridge.args.tokens, &bridge.args.plugin_store]
                            .into_iter()
                            .flatten()
                            .all(|other| p != other)
                        && p != &self.file
                });
            self.rules = match self.rules_path.as_ref().map(|p| rules_from(p)).transpose() {
                Ok(rules) => {
                    if !self.socket.as_os_str().is_empty() {
                        self.notice.clear();
                    }
                    rules.unwrap_or_default()
                }
                Err(_) => {
                    self.rules_path = None;
                    self.notice = "Could not read saved OBS rules for this model".into();
                    BTreeMap::new()
                }
            };
            self.schedule = Schedule::default();
            self.applied = false;
            self.published.clear();
            self.next_ui = now;
            self.socket = bridge.args.sock.clone();
        }
        let request = dir.join("obs-request.tsv");
        if fs::symlink_metadata(&request).is_ok() {
            let text = read_private(&request, 4096);
            fs::remove_file(&request)?;
            self.notice = match text {
                Ok(Some(text)) => self
                    .request(
                        &text.trim_end_matches('\n').split('\t').collect::<Vec<_>>(),
                        &bridge,
                    )
                    .err()
                    .map(|e| e.to_string())
                    .unwrap_or_default(),
                _ => "Could not read OBS request".into(),
            };
        }
        let state = self.connection.as_ref().map(|c| c.state.lock().unwrap());
        let program = state.as_ref().and_then(|state| state.program.clone());
        let resolving = state.as_ref().is_some_and(|state| state.resolving(now));
        let ui_snapshot =
            (now >= self.next_ui).then(|| state.as_deref().cloned().unwrap_or_default());
        drop(state);
        self.schedule.observe(program, |scene| {
            let rule = self.rules.get(scene)?;
            let position = bridge.expressions.assets.get(&rule.expression)?.position?;
            Some((rule.delay, position))
        });
        let next = if !self.settings.enabled {
            Some(None)
        } else if resolving {
            None
        } else {
            self.schedule.ready(now)
        };
        if let Some(position) = next {
            if position.is_some() || self.applied {
                let packet = position
                    .map(|p| format!("expr on obs-position 0 0{}", p.packet()))
                    .unwrap_or_else(|| "expr hold obs-position".into());
                if let Err(error) = bridge.send(&packet) {
                    if now >= self.next_ui {
                        eprintln!("[obs] {}", error.1);
                    }
                } else {
                    self.applied = position.is_some();
                    self.schedule.pending = None;
                }
            } else {
                self.schedule.pending = None;
            }
        }
        if let Some(snapshot) = ui_snapshot {
            self.next_ui = now + UI_POLL;
            self.publish(dir, &snapshot, &bridge)?;
        }
        Ok(())
    }
    fn publish(&mut self, dir: &Path, snapshot: &SceneState, bridge: &Bridge) -> AnyResult<()> {
        let status = if snapshot.status.is_empty() {
            if self.settings.enabled {
                "Connecting to OBS"
            } else {
                "Disconnected"
            }
        } else {
            &snapshot.status
        };
        let status = status.replace(['\n', '\r', '\t'], " ");
        let program = snapshot
            .program
            .as_ref()
            .map_or("", |(name, _)| name.as_str());
        let mut text = format!(
            "kawari-obs-1\nS\t{status}\t{program}\nC\t{}\t{}\t{}\n",
            self.settings.port,
            u8::from(self.settings.enabled),
            u8::from(!self.settings.password.is_empty())
        );
        if !self.notice.is_empty() {
            text.push_str(&format!(
                "M\t{}\n",
                self.notice.replace(['\n', '\r', '\t'], " ")
            ));
        }
        for name in &snapshot.scenes {
            text.push_str(&format!("N\t{name}\n"));
        }
        for (name, asset) in &bridge.expressions.assets {
            if asset.position.is_some() {
                text.push_str(&format!("E\t{}\t{name}\n", asset.key));
            }
        }
        for (scene, rule) in &self.rules {
            let key = bridge
                .expressions
                .assets
                .get(&rule.expression)
                .map_or("", |a| a.key.as_str());
            text.push_str(&format!("R\t{scene}\t{key}\t{}\n", rule.delay.as_millis()));
        }
        if self.published != text {
            let mut file = tempfile::NamedTempFile::new_in(dir)?;
            file.write_all(text.as_bytes())?;
            file.persist(dir.join("obs-ui.tsv"))?;
            self.published = text;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::expression_editor::parse_asset;
    use std::net::TcpListener;

    fn position(x: i32) -> ModelPosition {
        parse_asset(
            "Move.exp3.json",
            &json!({"KawariPosition":{"X":x,"Y":0,"Speed":1}}),
        )
        .unwrap()
        .position
        .unwrap()
    }

    #[test]
    fn later_scene_replaces_pending_move() {
        let start = Instant::now();
        let mut schedule = Schedule::default();
        let first = Some(("Intro".into(), start));
        schedule.observe(first.clone(), |_| {
            Some((Duration::from_millis(100), position(-1)))
        });
        assert_eq!(schedule.ready(start + Duration::from_millis(99)), None);

        let changed = start + Duration::from_millis(50);
        let second = Some(("Chat".into(), changed));
        let delay = Duration::from_millis(200);
        schedule.observe(second.clone(), |_| Some((delay, position(1))));
        assert_eq!(schedule.ready(start + Duration::from_millis(100)), None);
        assert_eq!(schedule.ready(changed + delay), Some(Some(position(1))));

        schedule.pending = None;
        schedule.observe(second.clone(), |_| Some((delay, position(1))));
        assert_eq!(schedule.ready(changed + delay), None);
        schedule.observe(second, |_| None);
        assert_eq!(schedule.ready(changed + delay), Some(None));

        schedule.observe(first.clone(), |_| Some((delay, position(-1))));
        schedule.observe(Some(("Unbound".into(), changed)), |_| None);
        assert_eq!(schedule.ready(changed), Some(None));
        assert_eq!(schedule.ready(changed + delay), Some(None));

        schedule.observe(first, |_| Some((delay, position(-1))));
        schedule.observe(None, |_| unreachable!());
        assert_eq!(schedule.ready(changed + delay), None);
    }

    #[test]
    fn transition_lookup_keeps_latest_start_time() {
        let start = Instant::now();
        let mut state = SceneState::default();
        state.set_program(&json!("Intro"), start);
        let started = json!({"op":5,"d":{"eventType":"SceneTransitionStarted"}});
        let old = state.update(&started, start).unwrap();
        let next_start = start + Duration::from_millis(50);
        let new = state.update(&started, next_start).unwrap();

        let response = |id: &str, name: &str| json!({"op":7,"d":{"requestId":id,"requestStatus":{"result":true},"responseData":{"sceneName":name}}});
        let now = next_start + Duration::from_millis(20);
        state.update(&response(&old, "Chat"), now);
        assert!(state.resolving(now));
        assert_eq!(state.program.as_ref().unwrap().0, "Intro");
        state.update(&response(&new, "Gameplay"), now);
        assert!(!state.resolving(now));
        assert_eq!(
            state.program.as_ref().unwrap(),
            &("Gameplay".into(), next_start)
        );

        let confirmed = json!({"op":5,"d":{"eventType":"CurrentProgramSceneChanged","eventData":{"sceneName":"Gameplay"}}});
        state.update(&confirmed, now + Duration::from_secs(1));
        assert_eq!(state.program.unwrap().1, next_start);
    }

    #[test]
    fn private_settings_and_scene_rules_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let settings_path = dir.path().join("obs.json");
        let settings = Settings {
            enabled: true,
            port: 4456,
            password: "test password".into(),
        };
        settings.save(&settings_path).unwrap();
        let saved = Settings::load(&settings_path).unwrap();
        assert!(saved.enabled);
        assert_eq!(saved.port, settings.port);
        assert_eq!(saved.password, settings.password);
        assert_eq!(fs::metadata(&settings_path).unwrap().mode() & 0o777, 0o600);

        let rules_path = dir.path().join("state.obs-scenes.json");
        let scene = "Gameplay — 夜".to_string();
        save_rules(
            &rules_path,
            &BTreeMap::from([(
                scene.clone(),
                Rule {
                    expression: "Move.exp3.json".into(),
                    delay: Duration::from_millis(650),
                },
            )]),
        )
        .unwrap();
        let rules = rules_from(&rules_path).unwrap();
        assert_eq!(rules[&scene].expression, "Move.exp3.json");
        assert_eq!(rules[&scene].delay, Duration::from_millis(650));
        assert!(rules_from(&dir.path().join("other-model.obs-scenes.json"))
            .unwrap()
            .is_empty());
    }

    #[test]
    fn authenticates_and_tracks_program_scene() {
        const PROOF: &str = "zTM5ki6L2vVvBQiTG9ckH1Lh64AbnCf6XZ226UmnkIA=";
        let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        let server = thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            stream.set_read_timeout(Some(CONNECT_TIMEOUT)).unwrap();
            stream.set_write_timeout(Some(CONNECT_TIMEOUT)).unwrap();
            let mut ws = tungstenite::accept(stream).unwrap();
            let send = |ws: &mut tungstenite::WebSocket<TcpStream>, value: Value| {
                ws.send(Message::Text(value.to_string().into())).unwrap();
            };
            send(
                &mut ws,
                json!({"op":0,"d":{"rpcVersion":1,"authentication":{"salt":"salt","challenge":"challenge"}}}),
            );
            let identify: Value =
                serde_json::from_str(ws.read().unwrap().to_text().unwrap()).unwrap();
            assert_eq!(
                identify,
                json!({"op":1,"d":{"rpcVersion":1,"authentication":PROOF,"eventSubscriptions":20}})
            );
            send(&mut ws, json!({"op":2,"d":{"negotiatedRpcVersion":1}}));
            let request: Value =
                serde_json::from_str(ws.read().unwrap().to_text().unwrap()).unwrap();
            assert_eq!(
                request,
                json!({"op":6,"d":{"requestType":"GetSceneList","requestId":"kawari-scenes"}})
            );
            send(
                &mut ws,
                json!({"op":7,"d":{"requestId":"kawari-scenes","requestStatus":{"result":true},
                "responseData":{"currentProgramSceneName":"Intro","scenes":[{"sceneName":"Intro"},{"sceneName":"Chat"}]}}}),
            );
            send(
                &mut ws,
                json!({"op":5,"d":{"eventType":"SceneTransitionStarted","eventData":{"transitionName":"Stinger"}}}),
            );
            let lookup: Value =
                serde_json::from_str(ws.read().unwrap().to_text().unwrap()).unwrap();
            assert_eq!(
                lookup,
                json!({"op":6,"d":{"requestType":"GetCurrentProgramScene","requestId":"kawari-program-1"}})
            );
            send(
                &mut ws,
                json!({"op":7,"d":{"requestId":"kawari-program-1","requestStatus":{"result":true},
                "responseData":{"currentProgramSceneName":"Chat"}}}),
            );
            send(
                &mut ws,
                json!({"op":5,"d":{"eventType":"CurrentProgramSceneChanged","eventData":{"sceneName":"Chat"}}}),
            );
            send(
                &mut ws,
                json!({"op":5,"d":{"eventType":"CurrentPreviewSceneChanged","eventData":{"sceneName":"Outro"}}}),
            );
            send(
                &mut ws,
                json!({"op":5,"d":{"eventType":"SceneListChanged",
                "eventData":{"scenes":[{"sceneName":"Intro"},{"sceneName":"Chat"},{"sceneName":"Outro"}]}}}),
            );
        });
        let settings = Settings {
            enabled: true,
            port,
            password: "password".into(),
        };
        let state = Mutex::new(SceneState::default());
        let result = listen(&settings, &state, &AtomicBool::new(false));
        server.join().unwrap();
        assert!(result.is_err());
        let state = state.into_inner().unwrap();
        assert_eq!(state.status, "Connected to OBS");
        assert_eq!(state.program.unwrap().0, "Chat");
        assert_eq!(
            state.scenes,
            BTreeSet::from(["Intro".into(), "Chat".into(), "Outro".into()])
        );
    }
}

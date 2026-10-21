//! The launcher core (`gothic-launcher control`, Python) as a child process. Requests go to its stdin as
//! JSON lines, answers and events come back on its stdout; events are passed on to the windows as `core`
//! events. The core runs from the repository with `uv run`, like the services it starts.
//!
//! When the app goes away, stdin closes and the core stops the game and the services, even if the app
//! crashed. On a normal quit `shutdown` waits for that.

use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::{json, Value};
use tauri::{AppHandle, Emitter};
use tokio::sync::oneshot;

type Answer = Result<Value, String>;
type Pending = Arc<Mutex<HashMap<u64, oneshot::Sender<Answer>>>>;

/// uv from the Dock gets a PATH without Homebrew and ~/.local/bin: look where installers put it.
const UV_CANDIDATES: [&str; 4] = [
    "~/.local/bin/uv",
    "/opt/homebrew/bin/uv",
    "/usr/local/bin/uv",
    "~/.cargo/bin/uv",
];
const SHUTDOWN_WAIT: Duration = Duration::from_secs(20);

pub struct Core {
    stdin: Mutex<Option<ChildStdin>>,
    child: Mutex<Option<Child>>,
    pending: Pending,
    next_id: AtomicU64,
    /// A session runs: closing the window hides it to the menu bar instead of quitting.
    pub running: Arc<AtomicBool>,
}

impl Core {
    pub fn spawn(app: AppHandle) -> Result<Core, String> {
        let repo = repo_dir();
        let uv = find_uv().ok_or("uv не найден: установите его (https://docs.astral.sh/uv/)")?;
        let mut child = Command::new(&uv)
            .arg("run")
            .arg("--project")
            .arg(repo.join("launcher").join("core"))
            .args(["gothic-launcher", "control"])
            .current_dir(home())
            .env_remove("VIRTUAL_ENV")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(
                log_file()
                    .map(Stdio::from)
                    .unwrap_or_else(|_| Stdio::null()),
            )
            .process_group(0) // its own group: a stuck core can be killed with whatever it started
            .spawn()
            .map_err(|e| format!("не удалось запустить ядро лаунчера ({}): {e}", uv.display()))?;
        let stdout = child.stdout.take().expect("piped stdout");
        let pending: Pending = Arc::default();
        let running = Arc::new(AtomicBool::new(false));
        let (reader_pending, reader_running) = (pending.clone(), running.clone());
        std::thread::spawn(move || read(app, stdout, reader_pending, reader_running));
        Ok(Core {
            stdin: Mutex::new(child.stdin.take()),
            child: Mutex::new(Some(child)),
            pending,
            next_id: AtomicU64::new(1),
            running,
        })
    }

    pub async fn call(&self, method: &str, params: Value) -> Answer {
        let id = self.next_id.fetch_add(1, Ordering::SeqCst);
        let (tx, rx) = oneshot::channel();
        self.pending.lock().unwrap().insert(id, tx);
        let line = json!({"id": id, "method": method, "params": params}).to_string() + "\n";
        let sent = match self.stdin.lock().unwrap().as_mut() {
            Some(stdin) => stdin
                .write_all(line.as_bytes())
                .and_then(|_| stdin.flush())
                .map_err(|e| e.to_string()),
            None => Err("остановлено".to_string()),
        };
        if let Err(err) = sent {
            self.pending.lock().unwrap().remove(&id);
            return Err(format!("ядро лаунчера не отвечает: {err}"));
        }
        rx.await
            .unwrap_or_else(|_| Err("ядро лаунчера остановилось".into()))
    }

    /// Closes the core's stdin: it stops the session and exits. Waits for that, then kills its group.
    pub fn shutdown(&self) {
        self.stdin.lock().unwrap().take();
        let Some(mut child) = self.child.lock().unwrap().take() else {
            return;
        };
        let deadline = Instant::now() + SHUTDOWN_WAIT;
        while Instant::now() < deadline {
            match child.try_wait() {
                Ok(Some(_)) | Err(_) => return,
                Ok(None) => std::thread::sleep(Duration::from_millis(100)),
            }
        }
        unsafe { libc::killpg(child.id() as i32, libc::SIGKILL) };
        let _ = child.wait();
    }
}

fn read(app: AppHandle, stdout: impl std::io::Read, pending: Pending, running: Arc<AtomicBool>) {
    for line in BufReader::new(stdout).lines() {
        let Ok(line) = line else { break };
        let Ok(msg) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        if let Some(id) = msg.get("id").and_then(Value::as_u64) {
            let answer = match msg.get("error") {
                Some(err) => Err(err.as_str().unwrap_or("ошибка").to_string()),
                None => Ok(msg.get("result").cloned().unwrap_or(Value::Null)),
            };
            if let Some(tx) = pending.lock().unwrap().remove(&id) {
                let _ = tx.send(answer);
            }
        } else if msg.get("event").is_some() {
            if msg["event"] == "session" {
                running.store(msg["data"]["state"] == "running", Ordering::SeqCst);
            }
            let _ = app.emit("core", &msg);
        }
    }
    for (_, tx) in pending.lock().unwrap().drain() {
        let _ = tx.send(Err("ядро лаунчера остановилось".into()));
    }
    running.store(false, Ordering::SeqCst);
    let _ = app.emit("core", json!({"event": "core_exit", "data": {}}));
}

/// The repository this app was built from (the launcher runs everything from it), or GOTHIC_AI_REPO.
fn repo_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("GOTHIC_AI_REPO") {
        return PathBuf::from(dir);
    }
    let built = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..");
    built.canonicalize().unwrap_or(built)
}

fn find_uv() -> Option<PathBuf> {
    if let Ok(uv) = std::env::var("GOTHIC_UV") {
        return Some(PathBuf::from(uv));
    }
    UV_CANDIDATES
        .iter()
        .map(|c| match c.strip_prefix("~/") {
            Some(rest) => home().join(rest),
            None => PathBuf::from(c),
        })
        .find(|p| p.is_file())
}

fn home() -> PathBuf {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

/// The core's own log, appended alongside the platform's session logs.
fn log_file() -> std::io::Result<File> {
    #[cfg(target_os = "macos")]
    let dir = home().join("Library/Logs/GothicAI");
    #[cfg(target_os = "linux")]
    let dir = home().join(".local/state/gothic-ai/logs");
    std::fs::create_dir_all(&dir)?;
    OpenOptions::new()
        .create(true)
        .append(true)
        .open(dir.join("launcher.log"))
}

//! The launcher window and its menu bar icon. Everything the launcher does is in the core (bridge.rs);
//! this side shows windows and gives the page what a web page cannot do: folder dialogs (plugin),
//! Finder, the menu bar.
//!
//! While the game runs the window hides and the launcher lives in the menu bar: a click on its icon
//! opens a small panel (the `tray` window) with the services' state and the journal, a right click a
//! menu. Quitting while the game runs stops the game too, so it asks first (`quit_requested` to the
//! page, which answers with `quit_app`).

mod bridge;

use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use serde_json::Value;
use tauri::image::Image;
#[cfg(target_os = "macos")]
use tauri::menu::{Menu, SubmenuBuilder};
use tauri::menu::{MenuBuilder, MenuEvent, MenuItemBuilder};
use tauri::tray::TrayIconBuilder;
#[cfg(target_os = "macos")]
use tauri::tray::{MouseButton, MouseButtonState, TrayIconEvent};
#[cfg(target_os = "macos")]
use tauri::{ActivationPolicy, Wry};
use tauri::{AppHandle, Emitter, Manager, RunEvent, State, WindowEvent};
#[cfg(target_os = "macos")]
use tauri_plugin_positioner::{Position, WindowExt};

use bridge::Core;

/// Set once the player agreed to stop the game: the next exit request goes through.
#[derive(Default)]
struct QuitAllowed(AtomicBool);

#[tauri::command]
async fn core_call(
    core: State<'_, Arc<Core>>,
    method: String,
    params: Option<Value>,
) -> Result<Value, String> {
    core.call(
        &method,
        params.unwrap_or_else(|| Value::Object(Default::default())),
    )
    .await
}

/// Hides the window: the launcher stays in the menu bar (no Dock icon) while the game runs.
#[tauri::command]
fn to_menu_bar(app: AppHandle) {
    if let Some(main) = app.get_webview_window("main") {
        let _ = main.hide();
    }
    #[cfg(target_os = "macos")]
    let _ = app.set_activation_policy(ActivationPolicy::Accessory);
}

#[tauri::command]
fn show_window(app: AppHandle) {
    show_main(&app);
}

/// The player confirmed: stop the game and quit.
#[tauri::command]
fn quit_app(app: AppHandle) {
    app.state::<QuitAllowed>().0.store(true, Ordering::SeqCst);
    app.exit(0);
}

/// Shows a file or folder in Finder.
#[tauri::command]
fn reveal(path: String) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    {
        Command::new("/usr/bin/open")
            .arg("-R")
            .arg(&path)
            .status()
            .map(|_| ())
            .map_err(|e| e.to_string())
    }
    #[cfg(target_os = "linux")]
    {
        let path = std::path::Path::new(&path);
        let folder = if path.is_dir() {
            path
        } else {
            path.parent().unwrap_or(path)
        };
        open_folder(folder.to_string_lossy().into_owned())
    }
}

/// Opens a folder in Finder.
#[tauri::command]
fn open_folder(path: String) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    let opener = "/usr/bin/open";
    #[cfg(target_os = "linux")]
    let opener = "xdg-open";
    Command::new(opener)
        .arg(&path)
        .status()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

fn show_main(app: &AppHandle) {
    #[cfg(target_os = "macos")]
    let _ = app.set_activation_policy(ActivationPolicy::Regular);
    if let Some(tray) = app.get_webview_window("tray") {
        let _ = tray.hide();
    }
    if let Some(main) = app.get_webview_window("main") {
        let _ = main.show();
        let _ = main.unminimize();
        let _ = main.set_focus();
    }
}

fn game_runs(app: &AppHandle) -> bool {
    app.state::<Arc<Core>>().running.load(Ordering::SeqCst)
}

/// Quit, unless the game runs: then the window asks first.
fn request_quit(app: &AppHandle) {
    if game_runs(app) && !app.state::<QuitAllowed>().0.load(Ordering::SeqCst) {
        show_main(app);
        let _ = app.emit_to("main", "quit_requested", ());
    } else {
        app.exit(0);
    }
}

#[cfg(target_os = "macos")]
fn toggle_panel(app: &AppHandle) {
    let Some(panel) = app.get_webview_window("tray") else {
        return;
    };
    if panel.is_visible().unwrap_or(false) {
        let _ = panel.hide();
    } else {
        let _ = panel.move_window(Position::TrayCenter);
        let _ = panel.show();
        let _ = panel.set_focus();
    }
}

/// The app menu with our own Quit (it asks while the game runs) and the Edit commands text fields need.
#[cfg(target_os = "macos")]
fn app_menu(app: &AppHandle) -> tauri::Result<Menu<Wry>> {
    let quit = MenuItemBuilder::with_id("quit", "Выйти из Gothic AI")
        .accelerator("CmdOrCtrl+Q")
        .build(app)?;
    let launcher = SubmenuBuilder::new(app, "Gothic AI")
        .about(None)
        .separator()
        .hide()
        .hide_others()
        .show_all()
        .separator()
        .item(&quit)
        .build()?;
    let edit = SubmenuBuilder::new(app, "Правка")
        .undo()
        .redo()
        .separator()
        .cut()
        .copy()
        .paste()
        .select_all()
        .build()?;
    let window = SubmenuBuilder::new(app, "Окно")
        .minimize()
        .close_window()
        .build()?;
    MenuBuilder::new(app)
        .items(&[&launcher, &edit, &window])
        .build()
}

fn on_menu(app: &AppHandle, event: MenuEvent) {
    match event.id().as_ref() {
        "quit" => request_quit(app),
        "open" => show_main(app),
        _ => {}
    }
}

pub fn run() {
    let builder = tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            show_main(app)
        }))
        .plugin(tauri_plugin_dialog::init())
        .manage(QuitAllowed::default());
    #[cfg(target_os = "macos")]
    let builder = builder
        .plugin(tauri_plugin_positioner::init())
        .menu(app_menu);
    let app = builder
        .on_menu_event(on_menu) // the app menu and the tray menu alike
        .setup(|app| {
            let core = Core::spawn(app.handle().clone())?;
            app.manage(Arc::new(core));
            let open = MenuItemBuilder::with_id("open", "Открыть Gothic AI").build(app)?;
            let quit = MenuItemBuilder::with_id("quit", "Выйти").build(app)?;
            let tray_menu = MenuBuilder::new(app)
                .item(&open)
                .separator()
                .item(&quit)
                .build()?;
            let tray = TrayIconBuilder::with_id("gothic")
                .tooltip("Gothic AI")
                .menu(&tray_menu);
            #[cfg(target_os = "macos")]
            let tray = tray
                .icon(Image::from_bytes(include_bytes!("../icons/tray.png"))?)
                .icon_as_template(true)
                .show_menu_on_left_click(false) // a left click opens the panel, a right click the menu
                .on_tray_icon_event(|tray, event| {
                    tauri_plugin_positioner::on_tray_event(tray.app_handle(), &event);
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        toggle_panel(tray.app_handle());
                    }
                });
            // AppIndicator delivers its menu on Linux; tray click events are macOS-only.
            #[cfg(target_os = "linux")]
            let tray = tray
                .icon(Image::from_bytes(include_bytes!("../icons/128x128.png"))?)
                .show_menu_on_left_click(true);
            tray.build(app)?;
            Ok(())
        })
        .on_window_event(|window, event| match (window.label(), event) {
            ("tray", WindowEvent::Focused(false)) => {
                let _ = window.hide();
            }
            ("main", WindowEvent::CloseRequested { api, .. }) => {
                let app = window.app_handle();
                #[cfg(target_os = "macos")]
                if game_runs(app) {
                    api.prevent_close(); // the game runs: keep going from the menu bar
                    to_menu_bar(app.clone());
                } else {
                    app.exit(0);
                }
                #[cfg(target_os = "linux")]
                if game_runs(app) {
                    api.prevent_close();
                    request_quit(app);
                } else {
                    app.exit(0);
                }
            }
            _ => {}
        })
        .invoke_handler(tauri::generate_handler![
            core_call,
            to_menu_bar,
            show_window,
            quit_app,
            reveal,
            open_folder
        ])
        .build(tauri::generate_context!())
        .expect("error while building the launcher");

    app.run(|app, event| match event {
        #[cfg(target_os = "macos")]
        RunEvent::Reopen { .. } => show_main(app), // a click on the Dock icon
        // Quit from the Dock or the system (logout asks the same way); app.exit() passes a code
        RunEvent::ExitRequested {
            code: None, api, ..
        } if game_runs(app) && !app.state::<QuitAllowed>().0.load(Ordering::SeqCst) => {
            api.prevent_exit();
            request_quit(app);
        }
        RunEvent::Exit => app.state::<Arc<Core>>().shutdown(),
        _ => {}
    });
}

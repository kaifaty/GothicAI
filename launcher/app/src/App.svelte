<script lang="ts">
  // Which screen the window shows: the wizard until the setup is complete, the game while it runs,
  // the failure of the last start until dismissed, otherwise the main screen or the settings.
  import { onMount } from "svelte";
  import { onQuitRequested, quitApp } from "./lib/core";
  import { launcher } from "./lib/launcher.svelte";
  import Failed from "./screens/Failed.svelte";
  import Main from "./screens/Main.svelte";
  import Running from "./screens/Running.svelte";
  import Settings from "./screens/Settings.svelte";
  import Wizard from "./screens/Wizard.svelte";

  let view = $state<"main" | "settings" | "wizard">("main");
  let dismissed = $state<number | null>(null); // the start time of the failed session put aside
  let loadError = $state<string | null>(null);
  let confirmQuit = $state(false);
  let quitting = $state(false);

  onMount(() => {
    const unlisten = onQuitRequested(() => (confirmQuit = true));
    launcher
      .init()
      .then(() => {
        // the first setup stays open until its "Готово", even once the folder alone makes it complete
        if (!launcher.state?.configured) view = "wizard";
      })
      .catch((err) => (loadError = String(err)));
    return () => void unlisten.then((stop) => stop());
  });

  function quit() {
    quitting = true;
    void quitApp();
  }

  const screen = $derived.by(() => {
    const s = launcher.state;
    if (launcher.coreDown) return "core";
    if (!s) return loadError ? "core" : "loading";
    if (s.session?.state === "running" || s.session?.state === "waiting") return "running";
    if (s.session?.failed && s.session.code !== 0 && dismissed !== s.session.started) return "failed";
    if (view === "wizard" || !s.configured) return "wizard";
    return view;
  });
</script>

{#if screen === "loading"}
  <div class="center" data-tauri-drag-region><span class="display">Gothic II</span></div>
{:else if screen === "core"}
  <div class="center" data-tauri-drag-region>
    <h1 class="display">Ядро лаунчера остановилось</h1>
    <p class="muted selectable">{loadError ?? (navigator.platform.startsWith("Mac") ? "Подробности в ~/Library/Logs/GothicAI/launcher.log" : "Подробности в ~/.local/state/gothic-ai/logs/launcher.log")}</p>
  </div>
{:else if screen === "running"}
  <Running />
{:else if screen === "failed"}
  <Failed onSettings={() => { dismissed = launcher.state!.session!.started; view = "settings"; }} onDismiss={() => (dismissed = launcher.state!.session!.started)} />
{:else if screen === "wizard"}
  <Wizard onDone={() => (view = "main")} />
{:else if screen === "settings"}
  <Settings onBack={() => (view = "main")} onWizard={() => (view = "wizard")} />
{:else}
  <Main onSettings={() => (view = "settings")} />
{/if}

{#if confirmQuit}
  <div class="backdrop">
    <div class="dialog panel" role="alertdialog" aria-modal="true" aria-labelledby="quit-title" aria-describedby="quit-text">
      <h2 id="quit-title" class="display">Игра ещё идёт</h2>
      <p id="quit-text" class="muted">
        Если выйти, лаунчер остановит игру и сервисы. Несохранённое в игре пропадёт.
      </p>
      <div class="actions">
        <button class="btn btn-stop" onclick={quit} disabled={quitting}>
          {quitting ? "Останавливаю…" : "Остановить игру и выйти"}
        </button>
        <button class="btn btn-ghost" onclick={() => (confirmQuit = false)} disabled={quitting}>Отмена</button>
      </div>
    </div>
  </div>
{/if}

<style>
  .center {
    height: 100%;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 12px;
    padding: 32px;
    text-align: center;
    font-size: 28px;
  }
  .center p {
    font-size: 15px;
  }
  .backdrop {
    position: fixed;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 32px;
    background: rgba(10, 8, 6, 0.72);
  }
  .dialog {
    max-width: 440px;
    display: flex;
    flex-direction: column;
    gap: 12px;
    padding: 22px 24px;
  }
  .dialog h2 {
    font-size: 26px;
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    margin-top: 6px;
  }
</style>

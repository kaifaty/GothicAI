<script lang="ts">
  import Header from "../components/Header.svelte";
  import Icon from "../components/Icon.svelte";
  import UpdateNotice from "../components/UpdateNotice.svelte";
  import { call, reveal } from "../lib/core";
  import { launcher, SERVICE_NAMES } from "../lib/launcher.svelte";
  import { updateKind } from "../lib/status";

  let { onSettings, onDismiss }: { onSettings: () => void; onDismiss: () => void } = $props();

  const session = $derived(launcher.state!.session!);
  const failed = $derived(session.failed!);
  const name = $derived(SERVICE_NAMES[failed.proc] ?? failed.proc);
  const lines = $derived(failed.detail.split("\n"));
  const reason = $derived(lines[0]);
  // a newer main may already fix what failed: the update comes before a retry
  const update = $derived(updateKind(launcher.state!.update));
  const updating = $derived(update === "updating" || update === "restarting");
  const tail = $derived(lines.slice(1).map((l) => l.replace(/^ {4}/, "")).join("\n"));

  async function retry() {
    onDismiss();
    await launcher.start(session.mode as "menu" | "continue" | "new");
  }

  async function report() {
    try {
      const { path } = await call<{ path: string }>("report.create");
      await reveal(path);
    } catch (err) {
      launcher.error = String(err);
    }
  }
</script>

<div class="screen">
  <Header subtitle="живые NPC" tone="bad" />
  <main>
    <div class="title">
      <Icon name="warn" size={34} color="var(--bad)" />
      <div>
        <h1 class="display">{name}: не запустился</h1>
        <p class="muted">
          {reason.replace(/^exited with code (-?\d+) before it was ready$/, "Сервис завершился с кодом $1, не дождавшись готовности")}.
          Игра не запущена, остальные сервисы остановлены.
        </p>
      </div>
    </div>
    {#if update && update !== "updated"}<UpdateNotice />{/if}
    {#if tail}
      <section aria-labelledby="tail-title">
        <h2 id="tail-title" class="faint">Последние строки вывода</h2>
        <pre class="selectable">{tail}</pre>
      </section>
    {/if}
    <div class="actions">
      <button class="btn btn-primary" onclick={onSettings}>Открыть настройки</button>
      <button class="btn btn-ghost" onclick={retry} disabled={updating}>Повторить</button>
      <button class="btn btn-ghost" onclick={report}>Собрать отчёт</button>
      <button class="btn btn-ghost" onclick={onDismiss}>Назад</button>
    </div>
  </main>
</div>

<style>
  .screen {
    height: 100%;
    display: flex;
    flex-direction: column;
  }
  main {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    gap: 18px;
    padding: 24px 32px;
    max-width: 880px;
  }
  .title {
    display: flex;
    align-items: flex-start;
    gap: 16px;
  }
  .title :global(svg) {
    flex: none;
    margin-top: 4px;
  }
  h1 {
    font-size: 34px;
    line-height: 1.1;
    margin-bottom: 6px;
  }
  section {
    display: flex;
    flex-direction: column;
    min-height: 0; /* the output shrinks first when the update notice needs the room */
  }
  h2 {
    font-weight: 500;
    margin-bottom: 8px;
  }
  pre {
    min-height: 0;
    margin: 0;
    max-height: 220px;
    overflow: auto;
    padding: 14px 16px;
    background: var(--sunk);
    border: 1px solid var(--line);
    border-radius: 6px;
    font-family: var(--mono);
    font-size: 13px;
    line-height: 1.6;
    color: var(--log);
    white-space: pre-wrap;
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
  }
</style>

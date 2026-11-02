<script lang="ts">
  import Header from "../components/Header.svelte";
  import Icon from "../components/Icon.svelte";
  import StatusRow from "../components/StatusRow.svelte";
  import UpdateNotice from "../components/UpdateNotice.svelte";
  import { call, openFolder, reveal } from "../lib/core";
  import { launcher } from "../lib/launcher.svelte";
  import { beforeGame, updateKind } from "../lib/status";

  let { onSettings }: { onSettings: () => void } = $props();

  const s = $derived(launcher.state!);
  const rows = $derived(beforeGame(launcher.providers, launcher.checking, s.problems, s.settings.game.dir));
  const save = $derived(s.last_save);
  let starting = $state(false);
  const update = $derived(updateKind(s.update));
  // the checkout changes under the core: nothing starts until the app comes back on the new version
  const busy = $derived(starting || update === "updating" || update === "restarting");

  function when(t: number): string {
    const d = new Date(t * 1000);
    const today = new Date().toDateString() === d.toDateString();
    const time = d.toLocaleTimeString("ru-RU", { hour: "2-digit", minute: "2-digit" });
    return today ? `сегодня, ${time}` : `${d.toLocaleDateString("ru-RU", { day: "numeric", month: "long" })}, ${time}`;
  }

  async function start(mode: "menu" | "continue" | "new") {
    starting = true;
    try {
      await launcher.start(mode);
    } finally {
      starting = false;
    }
  }

  async function report() {
    try {
      const { path } = await call<{ path: string }>("report.create");
      await reveal(path);
    } catch (err) {
      launcher.error = String(err);
    }
  }

  const lastRun = $derived.by(() => {
    const session = s.session;
    if (!session?.ended) return null;
    const time = new Date(session.ended * 1000).toLocaleTimeString("ru-RU", { hour: "2-digit", minute: "2-digit" });
    return session.code === 0 ? `Последняя игра закончилась в ${time}, без ошибок` : `Последняя игра закончилась в ${time} с ошибкой`;
  });
</script>

<div class="screen">
  <Header subtitle="живые NPC">
    <button class="btn btn-ghost" onclick={onSettings}><Icon name="sliders" size={18} />Настройки</button>
  </Header>
  <main>
    <section class="play">
      <h1 class="display">Ночь Ворона</h1>
      {#if update}
        <UpdateNotice />
      {:else}
        <p class="muted">
          NPC слышат вас и отвечают голосом. В игре посмотрите на NPC и удерживайте <kbd>T</kbd>, чтобы сказать реплику.
        </p>
      {/if}
      {#if save}
        <button class="primary" disabled={busy} onclick={() => start("continue")}>
          <Icon name="play" size={26} />
          <span class="label">
            <span class="big">Продолжить</span>
            <span class="small">Сохранение {save.slot} · {when(save.time)}</span>
          </span>
        </button>
      {:else}
        <button class="primary" disabled={busy} onclick={() => start("menu")}>
          <Icon name="play" size={26} />
          <span class="label"><span class="big">Играть</span><span class="small">Откроется меню игры</span></span>
        </button>
      {/if}
      <div class="more">
        {#if save}<button class="btn btn-ghost" disabled={busy} onclick={() => start("menu")}>Меню игры</button>{/if}
        <button class="btn btn-ghost" disabled={busy} onclick={() => start("new")}>Новая игра</button>
      </div>
      {#if launcher.error}<p class="error selectable" role="alert">{launcher.error}</p>{/if}
    </section>
    <section class="ready panel" aria-labelledby="ready-title">
      <div class="ready-head">
        <h2 id="ready-title" class="display">Готовность</h2>
        <button class="link" onclick={() => launcher.checkProviders()} disabled={launcher.checking}>
          {launcher.checking ? "Проверяю…" : "Проверить"}
        </button>
      </div>
      {#each rows as row (row.key)}
        <StatusRow icon={row.icon} title={row.title} detail={row.detail} chip={row.chip} tone={row.tone} />
      {/each}
    </section>
  </main>
  <footer>
    <span>{lastRun ?? `Сохранения игры: ${s.paths.game_home.replace(/^\/Users\/[^/]+/, "~")}`}</span>
    <div class="links">
      <button class="link" onclick={() => openFolder(s.paths.logs)}>Папка логов</button>
      <button class="link" onclick={report}>Собрать отчёт</button>
    </div>
  </footer>
</div>

<style>
  .screen {
    height: 100%;
    display: flex;
    flex-direction: column;
  }
  main {
    flex: 1;
    display: flex;
    flex-wrap: wrap;
    gap: 32px;
    padding: 28px 32px;
    min-height: 0;
  }
  .play {
    flex: 1 1 380px;
    min-width: 0;
    display: flex;
    flex-direction: column;
    justify-content: center;
    gap: 20px;
  }
  h1 {
    font-size: 54px;
    line-height: 1;
  }
  .play p {
    max-width: 410px;
  }
  kbd {
    font-family: var(--mono);
    font-size: 14px;
    color: var(--text);
    border: 1px solid #5a4a3a;
    border-radius: 3px;
    padding: 0 6px;
  }
  .primary {
    display: flex;
    align-items: center;
    gap: 16px;
    max-width: 410px;
    padding: 16px 22px;
    background: var(--ember);
    color: var(--on-ember);
    border: 0;
    border-radius: 4px;
    text-align: left;
    font: inherit;
    cursor: pointer;
  }
  .primary:hover:not(:disabled) {
    background: var(--ember-hover);
  }
  .primary:disabled {
    opacity: 0.6;
    cursor: default;
  }
  .label {
    display: flex;
    flex-direction: column;
  }
  .big {
    font-family: var(--display);
    font-weight: 700;
    font-size: 28px;
    line-height: 1.05;
  }
  .small {
    font-size: 14px;
    color: #3d2a10;
  }
  .more {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
  }
  .error {
    max-width: 410px;
    color: var(--bad-text);
    font-size: 14px;
  }
  .ready {
    flex: 1 1 360px;
    min-width: 0;
    align-self: center;
    padding: 18px 22px 6px;
  }
  .ready-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 4px;
  }
  .ready h2 {
    font-size: 22px;
    letter-spacing: 0.04em;
  }
  .ready .link {
    font-size: 14px;
    min-height: 32px;
  }
  footer {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 8px 20px;
    padding: 4px 32px 8px;
    border-top: 1px solid var(--line);
    color: var(--text-3);
    font-size: 14px;
  }
  .links {
    display: flex;
    gap: 20px;
  }
</style>

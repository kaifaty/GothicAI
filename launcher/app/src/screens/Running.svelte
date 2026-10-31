<script lang="ts">
  import Header from "../components/Header.svelte";
  import Icon from "../components/Icon.svelte";
  import Journal from "../components/Journal.svelte";
  import StatusRow from "../components/StatusRow.svelte";
  import { openFolder } from "../lib/core";
  import { launcher } from "../lib/launcher.svelte";
  import { duringGame } from "../lib/status";

  const s = $derived(launcher.state!);
  const session = $derived(s.session!);
  const rows = $derived(duringGame(session, launcher.providers, !s.settings.voice.enabled, !s.settings.tts.enabled));
  let now = $state(Date.now() / 1000);
  let stopping = $state(false);

  $effect(() => {
    const timer = setInterval(() => (now = Date.now() / 1000), 1000);
    return () => clearInterval(timer);
  });

  const elapsed = $derived.by(() => {
    const total = Math.max(0, Math.floor(now - session.started));
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const sec = String(total % 60).padStart(2, "0");
    return h ? `${h}:${String(m).padStart(2, "0")}:${sec}` : `${m}:${sec}`;
  });

  async function stop() {
    stopping = true;
    try {
      await launcher.stop();
    } finally {
      stopping = false;
    }
  }
</script>

<div class="screen">
  <Header subtitle={session.state === "waiting" ? "ожидание окна" : "идёт игра"}>
    <span class="mono elapsed">{elapsed}</span>
    <button class="btn btn-stop" onclick={stop} disabled={stopping}>
      <Icon name="stop" size={16} />{stopping ? "Останавливаю…" : "Остановить игру и сервисы"}
    </button>
  </Header>
  <main>
    <section class="now panel" aria-labelledby="now-title">
      <h2 id="now-title" class="display">Сейчас</h2>
      {#if session.state === "waiting"}
        <StatusRow icon="shield" title="Очередь игр" detail={session.services.queue?.detail ?? "Ожидаем допуска gq"} chip="ожидание" tone="idle" />
      {:else}
        {#each rows as row (row.key)}
          <StatusRow icon={row.icon} title={row.title} detail={row.detail} chip={row.chip} tone={row.tone} />
        {/each}
      {/if}
    </section>
    <section class="log" aria-labelledby="log-title">
      <h2 id="log-title" class="display">Журнал</h2>
      <Journal lines={launcher.lines} />
    </section>
  </main>
  <footer>
    <span>Логи: <span class="mono selectable">{session.log_dir.replace(/^\/Users\/[^/]+/, "~")}</span></span>
    <button class="link" onclick={() => openFolder(session.log_dir)}>Папка логов</button>
  </footer>
</div>

<style>
  .screen {
    height: 100%;
    display: flex;
    flex-direction: column;
  }
  .elapsed {
    color: var(--text-3);
    margin-left: auto;
  }
  main {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-wrap: wrap;
    gap: 24px;
    padding: 24px 32px;
  }
  .now {
    flex: 1 1 360px;
    min-width: 0;
    align-self: flex-start;
    padding: 16px 22px 4px;
  }
  h2 {
    font-size: 22px;
    letter-spacing: 0.04em;
    margin-bottom: 6px;
  }
  .log {
    flex: 1 1 400px;
    min-width: 0;
    min-height: 0;
    display: flex;
    flex-direction: column;
    max-height: 100%;
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
</style>

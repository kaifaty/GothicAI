<script lang="ts">
  // The personal Ryzen install against main, above the play buttons: an open launcher holds the install
  // lock, so the automatic update skips it. One button releases the lock, updates and restarts the app.
  import Icon from "./Icon.svelte";
  import { launcher } from "../lib/launcher.svelte";
  import { plural, updateKind } from "../lib/status";

  const u = $derived(launcher.state?.update ?? null);
  const kind = $derived(updateKind(u));
  let pressed = $state(false);
  let now = $state(Date.now() / 1000);

  $effect(() => {
    if (kind !== "updating") return;
    const timer = setInterval(() => (now = Date.now() / 1000), 1000);
    return () => clearInterval(timer);
  });

  function clock(iso: string): string {
    const d = new Date(iso);
    const time = d.toLocaleTimeString("ru-RU", { hour: "2-digit", minute: "2-digit" });
    if (new Date().toDateString() === d.toDateString()) return time;
    return `${d.toLocaleDateString("ru-RU", { day: "numeric", month: "long" })}, ${time}`;
  }

  /** Why the install is still behind: the last automatic run, in words. */
  const why = $derived.by(() => {
    const report = u?.report;
    if (report?.result === "busy") return `Автообновление в ${clock(report.time)} пропущено: лаунчер открыт.`;
    if (report?.result === "error" && report.error) {
      return `Автообновление в ${clock(report.time)} не удалось: ${report.error.split("\n")[0]}`;
    }
    return "Пока лаунчер открыт, автообновление его пропускает.";
  });

  const elapsed = $derived.by(() => {
    const total = Math.max(0, Math.floor(now - (u?.updating?.started ?? now)));
    return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
  });

  async function update() {
    pressed = true;
    try {
      await launcher.update();
    } finally {
      pressed = false;
    }
  }
</script>

{#if u && kind}
  <section
    class="notice"
    class:warn={kind === "failed"}
    class:ok={kind === "updated"}
    aria-live="polite"
    aria-labelledby="update-title"
  >
    {#if kind === "updating" || kind === "restarting"}
      <div class="head">
        <span class="spin"><Icon name="update" size={20} color="var(--ember)" /></span>
        <h2 id="update-title">{kind === "updating" ? "Обновляю установку" : "Перезапускаю лаунчер"}</h2>
        {#if kind === "updating"}<span class="aside mono">{elapsed}</span>{/if}
      </div>
      {#if kind === "updating"}
        <p class="detail mono selectable" title={u.updating?.detail}>{u.updating?.detail || "Запускаю обновление"}</p>
        <p class="why">Играть можно будет после сборки. Лаунчер перезапустится сам.</p>
      {:else}
        <p class="why">
          {u.updating?.error ? `Обновление не удалось: ${u.updating.error.split("\n")[0]}` : "Открою его уже новой версией."}
        </p>
      {/if}
    {:else if kind === "failed"}
      <div class="head">
        <Icon name="warn" size={20} color="var(--warn)" />
        <h2 id="update-title">Новая версия не собралась</h2>
      </div>
      <p class="why">
        Играете на последней рабочей сборке <span class="mono">{u.head.slice(0, 7)}</span>. Исправление придёт со следующим
        обновлением main.
      </p>
    {:else if kind === "updated"}
      <div class="head">
        <Icon name="check" size={20} color="var(--ok)" />
        <h2 id="update-title">Установка обновлена</h2>
        {#if u.report}<span class="aside">{clock(u.report.time)}</span>{/if}
      </div>
    {:else}
      <div class="head">
        <Icon name="update" size={20} color="var(--ember)" />
        <h2 id="update-title">{kind === "behind" ? "Есть обновление" : "Установка собрана не до конца"}</h2>
        {#if kind === "behind"}
          <span class="aside">{u.behind} {plural(u.behind, "изменение", "изменения", "изменений")}</span>
        {/if}
      </div>
      {#if u.changes.length}
        <ul>
          {#each u.changes.slice(0, 2) as change (change)}
            <li title={change.title}>
              {#if change.pr !== null}<span class="pr mono">#{change.pr}</span>{/if}
              {change.title}
            </li>
          {/each}
        </ul>
      {/if}
      {#if u.auto}
        <p class="why">{why} Сборка займёт несколько минут.</p>
        <div class="act">
          <button class="btn btn-update" onclick={update} disabled={pressed}>
            {pressed ? "Запускаю…" : kind === "behind" ? "Обновить и перезапустить" : "Собрать и перезапустить"}
          </button>
        </div>
      {:else}
        <p class="why">{why}</p>
        <p class="why">Обновите с Mac: <span class="mono selectable">scripts/ryzen-play.sh update</span></p>
      {/if}
    {/if}
  </section>
{/if}

<style>
  .notice {
    display: flex;
    flex-direction: column;
    gap: 6px;
    max-width: 410px;
    padding: 12px 16px 14px 14px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-left: 3px solid var(--ember);
    border-radius: 4px;
  }
  .notice.warn {
    border-left-color: var(--warn);
  }
  .notice.ok {
    border-left-color: var(--ok-line);
    padding-bottom: 12px;
  }
  .head {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  h2 {
    font-size: 16px;
    font-weight: 700;
  }
  .aside {
    margin-left: auto;
    color: var(--text-3);
    font-size: 14px;
    white-space: nowrap;
  }
  ul,
  .why,
  .detail,
  .act {
    margin: 0 0 0 30px;
  }
  ul {
    padding: 0;
    list-style: none;
  }
  li {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--text-2);
    font-size: 14px;
  }
  .pr {
    color: var(--ember);
    margin-right: 4px;
  }
  .why {
    color: var(--text-3);
    font-size: 14px;
  }
  .detail {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--log);
    font-size: 12.5px;
  }
  .act {
    margin-top: 6px;
  }
  .btn-update {
    min-height: 40px;
    padding: 0 14px;
    background: rgba(224, 154, 62, 0.06);
    color: var(--ember);
    border: 1px solid var(--ember);
    font-weight: 700;
  }
  .btn-update:hover:not(:disabled) {
    background: rgba(224, 154, 62, 0.14);
    color: var(--ember-hover);
    border-color: var(--ember-hover);
  }
  .spin {
    display: inline-flex;
    animation: spin 1.4s linear infinite;
  }
  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .spin {
      animation: none;
    }
  }
</style>

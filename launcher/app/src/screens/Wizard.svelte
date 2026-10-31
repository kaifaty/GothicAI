<script lang="ts">
  // First setup in four steps: the game, the npc brain, the player's voice, the npc voices.
  import Header from "../components/Header.svelte";
  import Icon from "../components/Icon.svelte";
  import Reach from "../components/Reach.svelte";
  import Setting from "../components/Setting.svelte";
  import SignIn from "../components/SignIn.svelte";
  import SpeechTest from "../components/SpeechTest.svelte";
  import VoiceTest from "../components/VoiceTest.svelte";
  import { call, type Check, type FoundGame } from "../lib/core";
  import { launcher, seconds } from "../lib/launcher.svelte";
  import { ASR, ASR_FALLBACK, TTS, TTS_FALLBACK } from "../lib/options";

  let { onDone }: { onDone: () => void } = $props();

  const STEPS = ["Игра", "Мозг NPC", "Голос игрока", "Озвучка NPC"];
  let step = $state(0);
  const s = $derived(launcher.state!);

  // step 1: the game folder
  let gameChecks = $state<Check[]>([]);
  $effect(() => {
    void s.settings.game.dir; // again whenever the folder changes
    void call<Check[]>("check.game").then((c) => (gameChecks = c));
  });
  const gameReady = $derived(gameChecks.length > 0 && gameChecks.every((c) => c.ok));
  // copies found in Steam libraries and Wine bottles, offered while no folder is chosen
  let found = $state<FoundGame[] | null>(null);
  $effect(() => {
    if (!s.settings.game.dir && found === null) void call<FoundGame[]>("find.game").then((f) => (found = f));
  });
  // problems other than the folder (the game is not built, no uv): the folder step shows them too
  const otherProblems = $derived(s.problems.filter((p) => !p.startsWith("game folder") && !p.includes("has no Data")));

  // step 2: one reply from the brain
  type Reply = { text: string; first_token_s: number; total_s: number; llm: string; model: string | null };
  let reply = $state<Reply | null>(null);
  let testing = $state(false);
  let testError = $state<string | null>(null);
  async function testBrain() {
    testing = true;
    testError = null;
    try {
      reply = await call<Reply>("test.llm");
    } catch (err) {
      testError = String(err);
    } finally {
      testing = false;
    }
  }

  const canGoOn = $derived(step !== 0 || gameReady);
  const last = $derived(step === STEPS.length - 1);
</script>

<div class="screen">
  <Header subtitle="первая настройка">
    <span class="faint">шаг {step + 1} из {STEPS.length}</span>
  </Header>
  <ol class="steps" aria-label="Шаги настройки">
    {#each STEPS as name, i (name)}
      <li class:current={i === step} class:done={i < step} aria-current={i === step ? "step" : undefined}>
        <span class="num">{#if i < step}<Icon name="check" size={14} />{:else}{i + 1}{/if}</span>{name}
      </li>
    {/each}
  </ol>
  <main>
    {#if step === 0}
      <h1 class="display">Где лежит игра</h1>
      <p class="muted">Нужна своя копия Gothic II с аддоном «Ночь Ворона» и русской локализацией. Файлы игры лаунчер не меняет.</p>
      <Setting key="game.dir" kind="folder" title="Папка игры" description="" />
      {#if !s.settings.game.dir && found}
        {#if found.length}
          <ul class="found" aria-label="Найденные копии игры">
            {#each found as f (f.path)}
              <li>
                <span class="mono selectable">{f.path.replace(/^\/Users\/[^/]+/, "~")}</span>
                <span class="chip" class:bad={!f.ok}>{f.ok ? "подходит" : "не всё на месте"}</span>
                <button class="btn btn-ghost" onclick={() => launcher.set("game.dir", f.path)}>Использовать</button>
              </li>
            {/each}
          </ul>
        {:else}
          <p class="faint">В библиотеках Steam и бутылках CrossOver и Whisky игра не нашлась: выберите папку вручную.</p>
        {/if}
      {/if}
      <ul class="checks panel" aria-label="Проверка папки">
        {#each gameChecks as c (c.id)}
          <li>
            <Icon name={c.ok ? "check" : "cross"} size={20} color={c.ok ? "var(--ok)" : "var(--bad)"} />
            <div><div class="title">{c.title}</div><div class="faint">{c.detail}</div></div>
          </li>
        {/each}
        {#each otherProblems as p (p)}
          <li>
            <Icon name="cross" size={20} color="var(--bad)" />
            <div><div class="title">Лаунчер</div><div class="faint selectable">{p}</div></div>
          </li>
        {/each}
      </ul>
      <p class="faint">Сохранения и настройки графики будут лежать отдельно: <span class="mono">{s.paths.game_home.replace(/^\/Users\/[^/]+/, "~")}</span></p>
    {:else if step === 1}
      <h1 class="display">Кто думает за NPC</h1>
      <p class="muted">Языковая модель пишет реплики и решает, что NPC сделает.</p>
      <fieldset class="cards">
        <legend class="sr">Чем думают NPC</legend>
        {#each [{ value: "chatgpt", title: "Подписка ChatGPT", text: "Вход через браузер. Реплики тратят лимит подписки." }, { value: "openai", title: "Ключ API", text: "OpenAI или совместимый сервер, свой адрес и ключ." }] as card (card.value)}
          <label class="card" class:selected={s.settings.llm.backend === card.value}>
            <span class="pick">
              <input type="radio" name="backend" checked={s.settings.llm.backend === card.value} onchange={() => launcher.set("llm.backend", card.value)} />
              <span class="title">{card.title}</span>
            </span>
            <span class="faint">{card.text}</span>
          </label>
        {/each}
      </fieldset>
      {#if s.settings.llm.backend === "chatgpt"}
        <SignIn />
      {:else}
        <Setting key="secrets.openai_api_key" kind="secret" />
        <Setting key="llm.base_url" placeholder="https://api.openai.com/v1" mono />
      {/if}
      <section class="test panel" aria-labelledby="test-title">
        <div class="test-head">
          <div>
            <h2 id="test-title">Проверка</h2>
            <span class="faint">Ксардас ответит на «Привет, Ксардас.» так же, как в игре</span>
          </div>
          <button class="btn btn-ghost" onclick={testBrain} disabled={testing}>{testing ? "Жду ответ…" : reply ? "Проверить снова" : "Проверить"}</button>
        </div>
        {#if reply}
          <blockquote class="selectable">{reply.text}</blockquote>
          <span class="faint">Первое слово через {seconds(reply.first_token_s, 1)}, ответ целиком через {seconds(reply.total_s, 1)}{reply.model ? ` · ${reply.model}` : ""}</span>
        {/if}
        {#if testError}<span class="failed selectable" role="alert">{testError}</span>{/if}
      </section>
    {:else if step === 2}
      <h1 class="display">Как NPC вас слышат</h1>
      <p class="muted">В игре удерживайте T и говорите: речь распознаётся, пока вы говорите.</p>
      <Setting key="voice.enabled" kind="toggle" title="Голосовой ввод" description="Выключено: только текстовый чат" />
      {#if s.settings.voice.enabled}
        <Setting key="voice.provider" kind="choice" options={ASR} />
        {#if s.settings.voice.provider === "inworld" || s.settings.voice.fallback === "inworld"}
          <Setting key="secrets.inworld_api_key" kind="secret" />
        {/if}
        {#if s.settings.voice.provider === "voxtral" || s.settings.voice.fallback === "voxtral"}
          <Setting key="voice.voxtral_url" placeholder="http://gpu-server:47830" mono />
        {/if}
        <Setting key="voice.fallback" kind="choice" options={ASR_FALLBACK} />
        <Reach part={launcher.providers?.voice} />
        <VoiceTest />
        <p class="faint">{navigator.platform.startsWith("Mac") ? "macOS один раз спросит доступ к микрофону для лаунчера." : "Выберите микрофон в настройках звука GNOME и проверьте его здесь."}</p>
      {/if}
    {:else}
      <h1 class="display">Какими голосами говорят NPC</h1>
      <p class="muted">Голос каждого NPC создаётся один раз по описанию из его персоны. Это не голос актёра озвучки.</p>
      <Setting key="tts.enabled" kind="toggle" title="Озвучка NPC" description="Выключено: только субтитры" />
      {#if s.settings.tts.enabled}
        <Setting key="tts.provider" kind="choice" options={TTS} />
        {#if s.settings.tts.provider === "inworld" || s.settings.tts.fallback === "inworld"}
          <Setting key="secrets.inworld_api_key" kind="secret" />
        {/if}
        {#if s.settings.tts.provider === "voxcpm" || s.settings.tts.fallback === "voxcpm"}
          <Setting key="tts.voxcpm_url" placeholder="ws://gpu-server:47831" mono />
        {/if}
        <Setting key="tts.fallback" kind="choice" options={TTS_FALLBACK} />
        <Reach part={launcher.providers?.speech} />
        <SpeechTest />
      {/if}
    {/if}
  </main>
  <footer>
    {#if step > 0}
      <button class="btn btn-ghost" onclick={() => (step -= 1)}><Icon name="back" size={16} />Назад</button>
    {:else}
      <span class="faint">Всё можно поменять потом в настройках</span>
    {/if}
    {#if last}
      <button class="btn btn-primary" onclick={async () => { await launcher.refresh(); onDone(); }}>Готово</button>
    {:else}
      <button class="btn btn-primary" onclick={() => (step += 1)} disabled={!canGoOn}>Далее<Icon name="next" size={16} /></button>
    {/if}
  </footer>
</div>

<style>
  .screen {
    height: 100%;
    display: flex;
    flex-direction: column;
  }
  .steps {
    display: flex;
    flex-wrap: wrap;
    gap: 8px 28px;
    margin: 0;
    padding: 14px 32px;
    list-style: none;
    border-bottom: 1px solid var(--line);
    color: var(--text-3);
  }
  .steps li {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .num {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 28px;
    border-radius: 50%;
    border: 1px solid var(--control);
    font-size: 14px;
  }
  .current {
    color: var(--text);
    font-weight: 700;
  }
  .current .num {
    background: var(--ember);
    border-color: var(--ember);
    color: var(--on-ember);
  }
  .done {
    color: var(--text-2);
  }
  .done .num {
    border-color: var(--ok-line);
    color: var(--ok);
  }
  main {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 12px;
    padding: 20px 32px;
    max-width: 860px;
  }
  h1 {
    font-size: 34px;
    line-height: 1.1;
  }
  h2 {
    font-size: 16px;
  }
  .title {
    font-weight: 700;
  }
  .checks {
    margin: 0;
    padding: 0 18px;
    list-style: none;
  }
  .checks li {
    display: flex;
    align-items: flex-start;
    gap: 12px;
    padding: 11px 0;
  }
  .checks li + li {
    border-top: 1px solid var(--line);
  }
  .checks :global(svg) {
    flex: none;
    margin-top: 2px;
  }
  .found {
    margin: 0;
    padding: 0;
    list-style: none;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .found li {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 10px 14px;
    padding: 10px 14px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 6px;
  }
  .found .mono {
    flex: 1 1 260px;
    min-width: 0;
    overflow-x: auto;
    white-space: nowrap;
  }
  .found .chip.bad {
    border-color: var(--bad-line);
    color: var(--bad-text);
  }
  .cards {
    display: flex;
    flex-wrap: wrap;
    gap: 12px;
    margin: 0;
    padding: 0;
    border: 0;
  }
  .sr {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0 0 0 0);
  }
  .card {
    flex: 1 1 300px;
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 14px 16px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 6px;
    cursor: pointer;
  }
  .card.selected {
    background: var(--selected);
    border-color: var(--ember);
  }
  .pick {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .test {
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 14px 18px;
  }
  .test-head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
  }
  blockquote {
    margin: 0;
    padding: 10px 14px;
    background: var(--sunk);
    border-radius: 4px;
    font-style: italic;
    font-size: 18px;
  }
  .failed {
    color: var(--bad-text);
    font-size: 14px;
  }
  footer {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 10px 20px;
    padding: 12px 32px 16px;
    border-top: 1px solid var(--line);
  }
</style>

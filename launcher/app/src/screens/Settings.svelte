<script lang="ts">
  import Header from "../components/Header.svelte";
  import Icon from "../components/Icon.svelte";
  import Reach from "../components/Reach.svelte";
  import Setting from "../components/Setting.svelte";
  import SignIn from "../components/SignIn.svelte";
  import SpeechTest from "../components/SpeechTest.svelte";
  import VoiceTest from "../components/VoiceTest.svelte";
  import { openFolder } from "../lib/core";
  import { launcher } from "../lib/launcher.svelte";
  import { ASR, ASR_FALLBACK, EFFORT, LLM_BACKENDS, TTS, TTS_FALLBACK } from "../lib/options";

  let { onBack, onWizard }: { onBack: () => void; onWizard: () => void } = $props();

  const TABS = [
    { id: "game", label: "Игра" },
    { id: "brain", label: "Мозг NPC" },
    { id: "voice", label: "Голос игрока" },
    { id: "speech", label: "Озвучка NPC" },
    { id: "more", label: "Дополнительно" },
  ] as const;
  let tab = $state<(typeof TABS)[number]["id"]>("game");
  const s = $derived(launcher.state!.settings);
</script>

<div class="screen">
  <Header subtitle="настройки">
    <button class="btn btn-ghost" onclick={onBack}><Icon name="back" size={16} />К игре</button>
  </Header>
  <div class="body">
    <nav aria-label="Разделы настроек">
      {#each TABS as t (t.id)}
        <button class="tab" class:current={tab === t.id} aria-current={tab === t.id ? "page" : undefined} onclick={() => (tab = t.id)}>
          {t.label}
        </button>
      {/each}
      <button class="link wizard" onclick={onWizard}>Пройти мастер заново</button>
    </nav>
    <main>
      <h1 class="display">{TABS.find((t) => t.id === tab)?.label}</h1>
      {#if tab === "game"}
        <Setting key="game.dir" kind="folder" />
        <Setting key="paths.game_home" kind="folder" placeholder="~/.local/share/gothic-ai/game" description="Туда игра пишет сохранения, Gothic.ini и log.txt" />
      {:else if tab === "brain"}
        <Setting key="llm.backend" kind="choice" options={LLM_BACKENDS} />
        {#if s.llm.backend === "chatgpt"}<SignIn />{/if}
        {#if s.llm.backend === "openai"}<Setting key="secrets.openai_api_key" kind="secret" />{/if}
        {#if s.llm.backend === "local" || s.llm.backend === "openai"}
          <Setting key="llm.base_url" placeholder="http://127.0.0.1:1234/v1" mono />
        {/if}
        <Setting key="llm.model" placeholder="по умолчанию" />
        <Setting key="llm.reasoning_effort" kind="choice" options={EFFORT} />
        <Reach part={launcher.providers?.brain} />
      {:else if tab === "voice"}
        <Setting key="voice.enabled" kind="toggle" />
        {#if s.voice.enabled}
          <Setting key="voice.provider" kind="choice" options={ASR} />
          {#if s.voice.provider === "inworld" || s.voice.fallback === "inworld"}
            <Setting key="secrets.inworld_api_key" kind="secret" />
          {/if}
          <Setting key="voice.fallback" kind="choice" options={ASR_FALLBACK} />
          {#if s.voice.provider === "voxtral" || s.voice.fallback === "voxtral"}
            <Setting key="voice.voxtral_url" placeholder="http://127.0.0.1:47830" mono />
          {/if}
          <Reach part={launcher.providers?.voice} />
          <VoiceTest />
        {/if}
      {:else if tab === "speech"}
        <Setting key="tts.enabled" kind="toggle" />
        {#if s.tts.enabled}
          <Setting key="tts.provider" kind="choice" options={TTS} />
          {#if s.tts.provider === "inworld" || s.tts.fallback === "inworld"}
            <Setting key="secrets.inworld_api_key" kind="secret" />
          {/if}
          <Setting key="tts.fallback" kind="choice" options={TTS_FALLBACK} />
          <Setting key="tts.voxcpm_url" placeholder="ws://127.0.0.1:47831" mono />
          <Setting key="tts.loudness" placeholder="как озвучка игры" />
          <Reach part={launcher.providers?.speech} />
          <SpeechTest />
        {/if}
      {:else}
        <Setting key="game.dev_mode" kind="toggle" />
        <Setting key="game.chat_scale" kind="number" placeholder="1" />
        <Setting key="paths.uv" placeholder="найти самому" mono />
        <Setting key="paths.game_bin" placeholder="OpenGothic/build/opengothic/Gothic2Notr" mono />
        <Setting key="paths.repo" placeholder="тот, где лежит лаунчер" mono />
        <div class="files faint">
          <span>Настройки: <span class="mono selectable">{launcher.state!.paths.config.replace(/^\/Users\/[^/]+/, "~")}</span>, ключи в secrets.toml</span>
          <button class="link" onclick={() => openFolder(launcher.state!.paths.config)}>Открыть папку настроек</button>
        </div>
      {/if}
    </main>
  </div>
</div>

<style>
  .screen {
    height: 100%;
    display: flex;
    flex-direction: column;
  }
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
    gap: 24px;
    padding: 20px 32px 0;
  }
  nav {
    flex: 0 0 200px;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .tab {
    min-height: 44px;
    padding: 0 12px;
    text-align: left;
    background: none;
    border: 0;
    border-radius: 4px;
    color: var(--text-2);
    font: inherit;
    cursor: pointer;
  }
  .tab:hover {
    color: var(--text);
    background: var(--panel);
  }
  .tab.current {
    background: var(--selected);
    box-shadow: inset 0 0 0 1px var(--ember);
    color: var(--text);
    font-weight: 700;
  }
  .wizard {
    margin-top: 12px;
    padding: 0 12px;
    text-align: left;
    font-size: 15px;
  }
  main {
    flex: 1;
    min-width: 0;
    overflow-y: auto;
    padding-bottom: 24px;
  }
  h1 {
    font-size: 32px;
    line-height: 1.1;
    margin-bottom: 4px;
  }
  .files {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 8px 16px;
    padding-top: 16px;
  }
</style>

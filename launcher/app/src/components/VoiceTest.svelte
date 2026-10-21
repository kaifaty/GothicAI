<script lang="ts">
  // The microphone and recognition check: records a few seconds through a temporary ASR service.
  import { onMount } from "svelte";
  import { call, type Mic, type VoiceResult } from "../lib/core";
  import { launcher, seconds } from "../lib/launcher.svelte";
  import { providerTitle } from "../lib/status";
  import Icon from "./Icon.svelte";
  import Setting from "./Setting.svelte";

  const RECORD_S = 4;
  let mics = $state<Mic[]>([]);
  let phase = $state<"idle" | "busy" | "done" | "failed">("idle");
  let result = $state<VoiceResult | null>(null);
  let failed = $state<string | null>(null);

  onMount(() => {
    call<Mic[]>("mics").then((m) => (mics = m)).catch(() => (mics = []));
  });

  const system = $derived(mics.find((m) => m.default));
  const options = $derived([
    { value: "", label: `Системный${system ? ` (${system.name})` : ""}` },
    ...mics.map((m) => ({ value: m.name, label: m.name })),
  ]);
  const recording = $derived(phase === "busy" && launcher.voiceTest.levels.length > 0);
  const bars = $derived([...Array(24 - launcher.voiceTest.levels.length).fill(0), ...launcher.voiceTest.levels]);
  const denied = $derived(failed?.includes("тишину") || failed?.includes("доступ"));

  async function run() {
    phase = "busy";
    failed = null;
    result = null;
    launcher.voiceTest = { levels: [], partial: "" };
    try {
      result = await call<VoiceResult>("test.voice", { seconds: RECORD_S });
      phase = "done";
    } catch (err) {
      failed = String(err);
      phase = "failed";
    }
  }
</script>

<Setting key="voice.mic_device" kind="choice" {options} title="Микрофон" />
<section class="test panel" aria-labelledby="voice-test-title">
  <div class="head">
    <div>
      <h2 id="voice-test-title">Проверка микрофона</h2>
      <span class="faint">{RECORD_S} с записи, потом распознавание как в игре</span>
    </div>
    <button class="btn btn-ghost" onclick={run} disabled={phase === "busy"}>
      <Icon name="mic" size={18} color="var(--ember)" />
      {recording ? "Говорите…" : phase === "busy" ? "Запускаю распознавание…" : "Скажите фразу"}
    </button>
  </div>
  <div class="meter" class:live={recording} role="img" aria-label="Уровень сигнала микрофона">
    {#each bars as level, i (i)}
      <div style="height: {Math.max(6, level * 100)}%" class:lit={level > 0.05}></div>
    {/each}
  </div>
  {#if phase === "busy" && launcher.voiceTest.partial}
    <span class="heard">«{launcher.voiceTest.partial}…»</span>
  {/if}
  {#if result}
    <span class="heard">«{result.text || "ничего не распознано"}»</span>
    <span class="faint">{providerTitle(result.provider)}, {seconds(result.latency_s)} после конца записи</span>
  {/if}
  {#if failed}
    <span class="failed selectable" role="alert">{failed}</span>
    {#if denied}
      <span class="faint">{navigator.platform.startsWith("Mac") ? "Разрешите доступ: Системные настройки → Конфиденциальность и безопасность → Микрофон → Gothic AI. В первый раз macOS спрашивает сама: ответьте и проверьте снова." : "Проверьте выбранный микрофон и уровень входа: Настройки GNOME → Звук → Вход. Затем повторите проверку."}</span>
    {/if}
  {/if}
</section>

<style>
  .test {
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 14px 18px;
    margin-top: 12px;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
  }
  h2 {
    font-size: 16px;
  }
  .meter {
    display: flex;
    align-items: flex-end;
    gap: 3px;
    height: 28px;
  }
  .meter div {
    flex: 1;
    border-radius: 1px;
    background: var(--control);
    transition: height 0.08s linear;
  }
  .meter.live div.lit {
    background: var(--ember);
  }
  .heard {
    padding: 8px 12px;
    background: var(--sunk);
    border-radius: 4px;
    font-style: italic;
    font-size: 17px;
  }
  .failed {
    color: var(--bad-text);
    font-size: 14px;
  }
</style>

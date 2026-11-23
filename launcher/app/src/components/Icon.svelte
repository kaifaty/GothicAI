<script lang="ts">
  // Stroke icons in the mockup's style; decorative unless `label` is given.
  type Name =
    | "candle" | "mic" | "speaker" | "shield" | "check" | "warn" | "play" | "stop"
    | "sliders" | "back" | "next" | "folder" | "cross" | "update";
  let { name, size = 20, color = "currentColor", label }: { name: Name; size?: number; color?: string; label?: string } =
    $props();
</script>

<svg
  width={size}
  height={size}
  viewBox="0 0 24 24"
  fill="none"
  stroke={color}
  stroke-width={name === "check" || name === "cross" ? 2.2 : 1.7}
  stroke-linecap="round"
  stroke-linejoin="round"
  role={label ? "img" : undefined}
  aria-label={label}
  aria-hidden={label ? undefined : "true"}
>
  {#if name === "candle"}
    <path d="M12 3c2.5 3 4 5.2 4 7.6A4 4 0 0 1 12 15a4 4 0 0 1-4-4.4C8 8.6 9.6 6.6 12 3z" /><path d="M10 15v6h4v-6" />
  {:else if name === "mic"}
    <rect x="9" y="3" width="6" height="11" rx="3" /><path d="M5 11a7 7 0 0 0 14 0M12 18v3" />
  {:else if name === "speaker"}
    <path d="M4 9h4l5-4v14l-5-4H4z" /><path d="M16.5 8.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12" />
  {:else if name === "shield"}
    <path d="M12 3l7 3v5c0 4.5-3 8-7 10-4-2-7-5.5-7-10V6z" />
  {:else if name === "check"}
    <path d="M5 12.5l4.5 4.5L19 7.5" />
  {:else if name === "cross"}
    <path d="M6 6l12 12M18 6L6 18" />
  {:else if name === "warn"}
    <path d="M12 4l9 16H3z" /><path d="M12 10v4M12 17v.5" />
  {:else if name === "play"}
    <path d="M7 4.5v15l13-7.5z" fill={color} stroke="none" />
  {:else if name === "stop"}
    <rect x="6" y="6" width="12" height="12" rx="1.5" fill={color} stroke="none" />
  {:else if name === "sliders"}
    <path d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12" /><circle cx="16" cy="6" r="2" /><circle cx="10" cy="12" r="2" /><circle
      cx="18"
      cy="18"
      r="2"
    />
  {:else if name === "back"}
    <path d="M15 5l-7 7 7 7" />
  {:else if name === "next"}
    <path d="M9 5l7 7-7 7" />
  {:else if name === "update"}
    <path d="M19.5 12a7.5 7.5 0 1 1-2.2-5.3" /><path d="M19.5 4.5v4h-4" />
  {:else if name === "folder"}
    <path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
  {/if}
</svg>

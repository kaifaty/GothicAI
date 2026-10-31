// Choices for the provider settings; a value set by hand that is not listed still shows in the select.
type Option = { value: string; label: string };

export const LLM_BACKENDS: Option[] = [
  { value: "chatgpt", label: "Подписка ChatGPT" },
  { value: "openai", label: "Ключ OpenAI" },
  { value: "local", label: "Свой сервер, OpenAI-совместимый" },
  { value: "fake", label: "Заглушка, без модели" },
];

export const EFFORT: Option[] = [
  { value: "low", label: "Низкая: быстрее ответ" },
  { value: "medium", label: "Средняя" },
  { value: "high", label: "Высокая: дольше думает" },
];

export const ASR: Option[] = [
  { value: "inworld", label: "Inworld" },
  { value: "voxtral", label: "Voxtral на своём сервере" },
  { value: "chatgpt", label: "Подписка ChatGPT" },
  { value: "openai", label: "Ключ OpenAI" },
];
export const ASR_FALLBACK: Option[] = [...ASR, { value: "none", label: "Без запасного" }];

export const TTS: Option[] = [
  { value: "inworld", label: "Inworld" },
  { value: "voxcpm", label: "VoxCPM2 на своём сервере" },
  { value: "openai", label: "Ключ OpenAI" },
];
export const TTS_FALLBACK: Option[] = [...TTS, { value: "none", label: "Только субтитры" }];

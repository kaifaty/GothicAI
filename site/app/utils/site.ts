// The words of the interface in both languages and the small pieces every page shares.
import data from "#site-content";

export type Lang = "en" | "ru";
export const site = data;

export const ROMAN = "I II III IV V VI VII VIII IX X XI XII".split(" ");
// static/art/<name>.webp, in the order of `people` in home.toml
export const PEOPLE = ["miner", "guard", "novice", "hunter"];

export const UI = {
  en: {
    devlog: "Devlog", source: "Source", other: "Русский", other_lang: "ru", home: "Gothic AI",
    all_posts: "All posts", feed: "RSS feed", play: "Play the short", back: "Back to the devlog",
    telegram: "Telegram", not_found: "There is no such page.", to_home: "Go to the home page",
    skip: "Skip to content", menu: "Main", short: "Short", newer: "Newer", older: "Older",
    empty: "Nothing here yet.", pause: "Pause",
  },
  ru: {
    devlog: "Дневник", source: "Код", other: "English", other_lang: "en", home: "Gothic AI",
    all_posts: "Все записи", feed: "RSS", play: "Смотреть шортс", back: "Ко всем записям",
    telegram: "Telegram", not_found: "Такой страницы нет.", to_home: "На главную",
    skip: "К содержанию", menu: "Главное", short: "Шортс", newer: "Позже", older: "Раньше",
    empty: "Пока пусто.", pause: "Пауза",
  },
} as const satisfies Record<Lang, Record<string, string>>;

export const otherLang = (lang: Lang): Lang => (lang === "en" ? "ru" : "en");
export const prefix = (lang: Lang) => (lang === "en" ? "" : `${lang}/`);

/** Links under the base path (/GothicAI/ on GitHub Pages, / in development). */
export function useLinks() {
  const base = useRuntimeConfig().app.baseURL.replace(/\/?$/, "/");
  const url = (lang: Lang, rel = "") => `${base}${prefix(lang)}${rel}`;
  /** A link under the base path as the published address (for search engines and link previews). */
  const absolute = (link: string) => {
    const siteUrl = String(site.settings.url ?? "").replace(/\/+$/, "");
    return link.startsWith(base) ? `${siteUrl}/${link.slice(base.length)}` : link;
  };
  return { base, url, absolute };
}

/** An illustration from static/art/ (ours, drawn for the site, not a frame of the game). */
export const art = (name: string) => `/static/art/${name}.webp`;

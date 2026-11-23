// The finishing of every page as it is prerendered:
// - no script but ours: Nuxt's speculation rules go (the pages are plain links);
// - the faces of the first screen are preloaded: the headline's and the running text's, in the subsets of
//   the page's language (@font-face rules come inlined from @nuxt/fonts; style.css names the families).
const FIRST_SCREEN = [
  { family: "Cormorant Garamond", weight: "600" },
  { family: "Spectral", weight: "400" },
];
const SUBSETS: Record<string, RegExp[]> = {
  en: [/^U\+(0000-00FF|\?\?)/i],
  ru: [/^U\+(0000-00FF|\?\?)/i, /^U\+(0301|301),U\+(0400|400)-/i],
};

export default defineNitroPlugin((nitro) => {
  nitro.hooks.hook("render:html", (html) => {
    html.head = html.head.map((chunk) => chunk.replace(/<script type="speculationrules">[\s\S]*?<\/script>/g, ""));
    const lang = html.htmlAttrs.join(" ").match(/lang="(\w+)"/)?.[1] ?? "en";
    const head = html.head.join("");
    const links: string[] = [];
    for (const rule of head.match(/@font-face\{[^}]*\}/g) ?? []) {
      const family = rule.match(/font-family:\s*"?([^;"]+)"?;/)?.[1];
      const weight = rule.match(/font-weight:\s*(\d+)/)?.[1];
      const style = rule.match(/font-style:\s*(\w+)/)?.[1] ?? "normal";
      const range = rule.match(/unicode-range:\s*([^;}]+)/)?.[1] ?? "";
      const url = rule.match(/url\(([^)]+\.woff2)\)/)?.[1]?.replace(/^["']|["']$/g, "");
      if (!url || style !== "normal" || !FIRST_SCREEN.some((f) => f.family === family && f.weight === weight)) continue;
      if (!(SUBSETS[lang] ?? SUBSETS.en!).some((re) => re.test(range))) continue;
      links.push(`<link rel="preload" as="font" type="font/woff2" href="${url}" crossorigin>`);
    }
    html.head.push(links.join(""));
  });
});

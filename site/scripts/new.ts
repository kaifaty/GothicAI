// `bun run new <slug> --title "…" [--title-ru "…"]`: start a post, content/posts/<today>-<slug>/{en,ru}.md.
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { parseArgs } from "node:util";

const { values, positionals } = parseArgs({
  allowPositionals: true,
  options: { title: { type: "string" }, "title-ru": { type: "string" } },
});
const [slug] = positionals;
if (!slug || !values.title) {
  console.error('usage: bun run new <slug> --title "The title" [--title-ru "Заголовок"]');
  process.exit(2);
}
// now, to the second, in this computer's time zone: 2026-11-05T00:20:31+03:00
const now = new Date();
const pad = (n: number) => String(Math.abs(n)).padStart(2, "0");
const offset = -now.getTimezoneOffset();
const day = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`;
const stamp = `${day}T${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}${offset < 0 ? "-" : "+"}${pad(Math.trunc(offset / 60))}:${pad(offset % 60)}`;

const folder = join(import.meta.dir, "..", "content", "posts", `${day}-${slug}`);
if (existsSync(folder)) {
  console.error(`error: ${folder} exists`);
  process.exit(1);
}
mkdirSync(folder, { recursive: true });
for (const [lang, title] of [["en", values.title], ["ru", values["title-ru"] || values.title]] as const) {
  writeFileSync(
    join(folder, `${lang}.md`),
    `+++\ntitle = ${JSON.stringify(title)}\nsummary = ""\ndate = ${stamp}\n# cover = "cover.jpg"\n# youtube = ""\n+++\n`,
  );
}
console.log(folder);

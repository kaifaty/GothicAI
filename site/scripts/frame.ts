// `bun run frame <video> <out.jpg> [--at 41] [--width 1080]`: a still from a video, scaled to `width`, as a
// JPEG (covers and pictures in posts). Needs ffmpeg.
import { mkdirSync, statSync } from "node:fs";
import { dirname } from "node:path";
import { parseArgs } from "node:util";

const { values, positionals } = parseArgs({
  allowPositionals: true,
  options: { at: { type: "string", default: "0" }, width: { type: "string", default: "1080" } },
});
const [video, out] = positionals;
if (!video || !out) {
  console.error("usage: bun run frame <video> <out.jpg> [--at seconds] [--width px]");
  process.exit(2);
}
mkdirSync(dirname(out), { recursive: true });
const run = Bun.spawnSync(
  ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", values.at!, "-i", video,
   "-frames:v", "1", "-vf", `scale=${values.width}:-2`, "-q:v", "4", out],
  { stdout: "inherit", stderr: "inherit" },
);
if (run.exitCode !== 0) process.exit(run.exitCode ?? 1);
console.log(`${out} (${Math.floor(statSync(out).size / 1024)} KB)`);

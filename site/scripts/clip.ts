// `bun run clip <video> <out.mp4> [--start 33.6] [--length 8] [--width 540] [--crf 28]`: a short muted loop
// from a video (the home page's short until it is on YouTube): small, so it can live in the repository.
// Needs ffmpeg.
import { mkdirSync, statSync } from "node:fs";
import { dirname } from "node:path";
import { parseArgs } from "node:util";

const { values, positionals } = parseArgs({
  allowPositionals: true,
  options: {
    start: { type: "string", default: "0" },
    length: { type: "string", default: "8" },
    width: { type: "string", default: "540" },
    crf: { type: "string", default: "28" },
  },
});
const [video, out] = positionals;
if (!video || !out) {
  console.error("usage: bun run clip <video> <out.mp4> [--start s] [--length s] [--width px] [--crf n]");
  process.exit(2);
}
mkdirSync(dirname(out), { recursive: true });
const run = Bun.spawnSync(
  ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", values.start!, "-t", values.length!, "-i", video,
   "-an", "-vf", `scale=${values.width}:-2,fps=30`, "-c:v", "libx264", "-preset", "slow", "-crf", values.crf!,
   "-pix_fmt", "yuv420p", "-movflags", "+faststart", out],
  { stdout: "inherit", stderr: "inherit" },
);
if (run.exitCode !== 0) process.exit(run.exitCode ?? 1);
const size = statSync(out).size;
console.log(`${out} (${Math.floor(size / 1024)} KB)`);
if (size > 3 * 1024 * 1024) console.error("warning: over 3 MB, use a shorter clip or a higher --crf");

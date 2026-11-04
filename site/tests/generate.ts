// Generate the site for GitHub Pages' base path before the tests read it (.output/public). SITE_OUT points
// at a site generated already, to skip the build.
import { spawnSync } from "node:child_process";
import { join } from "node:path";

export default function setup({ provide }: { provide: (key: string, value: string) => void }) {
  const root = join(import.meta.dirname, "..");
  let out = process.env.SITE_OUT;
  if (!out) {
    const run = spawnSync(join(root, "node_modules/.bin/nuxt"), ["generate"], {
      cwd: root,
      env: { ...process.env, NUXT_APP_BASE_URL: "/GothicAI/" },
      encoding: "utf8",
    });
    if (run.status !== 0) throw new Error(`nuxt generate failed:\n${run.stdout}\n${run.stderr}`);
    out = join(root, ".output/public");
  }
  provide("out", out);
}

declare module "vitest" {
  export interface ProvidedContext {
    out: string;
  }
}

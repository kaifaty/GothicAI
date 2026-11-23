// `bun run preview [--port 47861] [--base /GothicAI/] [--dir .output/public]`: serve the generated site the
// way GitHub Pages does: under the base path, compressed, 404.html for a missing address.
import { existsSync, statSync } from "node:fs";
import { join, normalize, resolve } from "node:path";
import { parseArgs } from "node:util";

const { values } = parseArgs({
  options: { port: { type: "string", default: "47861" }, base: { type: "string" }, dir: { type: "string" } },
});
const root = resolve(values.dir ?? join(import.meta.dir, "..", ".output", "public"));
if (!existsSync(join(root, "index.html"))) {
  console.error(`error: no ${root}/index.html: run \`bun run generate\` first`);
  process.exit(1);
}
// the base the pages were generated for, read from their own links unless given
const base = (values.base ?? (await Bun.file(join(root, "404.html")).text()).match(/<link rel="canonical" href="[^"]*?(\/[^"/]*\/)?404\.html"/)?.[1] ?? "/").replace(/\/?$/, "/");
const COMPRESS = /\.(html|css|js|xml|svg|json|webmanifest|txt)$/;

function file(pathname: string): string | null {
  if (!pathname.startsWith(base)) return null;
  const rel = normalize(decodeURIComponent(pathname.slice(base.length)));
  if (rel.startsWith("..")) return null;
  let path = join(root, rel);
  if (existsSync(path) && statSync(path).isDirectory()) path = join(path, "index.html");
  return existsSync(path) ? path : null;
}

const server = Bun.serve({
  port: Number(values.port),
  async fetch(req) {
    const { pathname } = new URL(req.url);
    if (pathname === "/" && base !== "/") return Response.redirect(base, 302);
    const path = file(pathname);
    const status = path ? 200 : 404;
    const body = Bun.file(path ?? join(root, "404.html"));
    const headers: Record<string, string> = { "content-type": body.type, "cache-control": "max-age=600" };
    if (COMPRESS.test(path ?? ".html") && (req.headers.get("accept-encoding") ?? "").includes("gzip")) {
      headers["content-encoding"] = "gzip";
      return new Response(Bun.gzipSync(await body.bytes()), { status, headers });
    }
    return new Response(body, { status, headers });
  },
});
console.log(`http://127.0.0.1:${server.port}${base}`);

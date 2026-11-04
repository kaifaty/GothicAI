import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    include: ["tests/**/*.test.ts"],
    globalSetup: ["tests/generate.ts"], // the site is generated once for /GothicAI/, then read as files
    hookTimeout: 600_000,
    testTimeout: 30_000,
  },
});

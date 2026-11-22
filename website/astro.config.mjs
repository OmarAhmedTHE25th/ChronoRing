import { defineConfig } from "astro/config";
import tailwindcss from "@tailwindcss/vite";

export default defineConfig({
  site: "https://omarahmedthe25th.github.io",
  vite: {
    plugins: [tailwindcss()],
    server:
      process.platform === "win32"
        ? {
            watch: {
              usePolling: true,
              interval: 300,
            },
          }
        : undefined,
  },
});

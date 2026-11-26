import { fileURLToPath } from "node:url"
import { defineConfig } from "vite"
import react from "@vitejs/plugin-react"

// The site is served from the root of the `pages` branch
// (https://eliasfd3s.github.io/openslrr/). `npm run build` writes index.html,
// launcher/index.html, site/ and the public files there without emptying it: docs/ holds
// the documentation site and .nojekyll must stay.
export default defineConfig({
  plugins: [react()],
  base: "./",
  build: {
    outDir: "..",
    emptyOutDir: false,
    assetsDir: "site",
    rollupOptions: {
      input: {
        home: fileURLToPath(new URL("./index.html", import.meta.url)),
        launcher: fileURLToPath(new URL("./launcher/index.html", import.meta.url)),
      },
    },
  },
})

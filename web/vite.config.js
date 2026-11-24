import { defineConfig } from "vite"
import react from "@vitejs/plugin-react"

// The site is served from the root of the `pages` branch
// (https://eliasfd3s.github.io/openslrr/). `npm run build` writes index.html,
// site/ and the public files there, and never empties the folder: docs/ holds
// the documentation site and .nojekyll must stay.
export default defineConfig({
  plugins: [react()],
  base: "./",
  build: {
    outDir: "..",
    emptyOutDir: false,
    assetsDir: "site",
  },
})

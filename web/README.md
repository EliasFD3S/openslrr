# OpenSLRR site

The presentation page of https://eliasfd3s.github.io/openslrr/, in React (Vite).

- `npm install`, then `npm run dev` to work on it, `npm run build` to publish.
- The build writes `index.html`, `launcher/index.html`, `site/`, `img/` and
  `shots/` to the root of the `pages` branch, which GitHub Pages serves. It
  never empties that folder. Keep both generated HTML files and `site/` in sync
  when publishing; the launcher page is served at `/openslrr/launcher/`.
- `docs/` at the branch root holds the documentation site (a separate,
  prebuilt app with hash routes such as `docs/#/build`). The home page links
  to it.
- Screenshots in `public/shots/` are OpenSLRR captures at 2560×1440 using the
  game data of SLRR 2.2.1 MWM.

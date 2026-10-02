# Maintain and publish documentation

All public pages are English Markdown under root `docs/`. Package README files
remain with their code. The site tracks `main`; its displayed repository version
comes from root `VERSION`, not a separate docs version.

## Local workflow

Use Node.js 22.18 or newer:

```sh
npm ci --prefix docs
npm run dev --prefix docs
npm run build --prefix docs
npm run preview --prefix docs
```

Open the printed URL under `/simplex-cpp/`. Build fails on unresolved document
links. The post-build checker also validates generated local links, anchors,
assets, and the project-site base path. VitePress supplies local search.

Add new pages to `.vitepress/config.mts`. Use relative Markdown links for pages
inside this site. Link to full GitHub URLs for source and package README files
outside it. VitePress is pinned to the stable 1.6 line; its Vite dependency is overridden
to the patched 6.4.3 release. Keep the override and lockfile together, and verify
build, development server, preview, and search when updating them.

The `README.md` indexes and historical panel redesign notes are
excluded from the site. Keep historical files out of public navigation/search.

The homepage title uses the same `assets/simplex-logo-v4.svg` as the repository
README. The custom theme imports this source directly, so logo changes are
bundled into the site with the correct GitHub Pages base path; no separate
copy needs to be maintained.

The canonical worker reference is `core/worker-protocol.md`, titled **Simplex
Loop Worker Protocol**. Its Hub implementation examples are part of the same
page. Protocol drift tests read this source; do not create a second competing
worker contract. The Hub's browser protocol remains a separate document.

## GitHub Pages

The workflow `.github/workflows/docs.yml` builds on pull requests and pushes to
`main`. Pull requests validate and upload a preview artifact without publishing.
Production deployment runs only from `main`, including manual dispatch.

In repository **Settings → Pages → Build and deployment**, select **GitHub
Actions**. Ensure the `github-pages` environment permits deployment from `main`.
After merge, the workflow uploads the generated site and deploys to:

```text
https://hazer-bjtu.github.io/simplex-cpp/
```

The build job has read-only repository permissions. Only deployment receives
`pages: write` and `id-token: write`. No personal token or committed build output
is needed. Deployment concurrency avoids overlapping production publication.
See [GitHub's custom workflow guide](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages)
and [VitePress deployment guidance](https://vitepress.dev/guide/deploy).

## Release packaging and validation

Hub npm staging includes public Markdown pages from `docs/`. If those references change, update `hub/scripts/stage-release.mjs`
and validate an actual packed archive. Do not ship historical design notes.

Run the existing protocol tests after editing wire tables:

```sh
node --test hub/test/protocol-drift.test.js hub/test/panel-protocol-drift.test.js
```

Provider pages carry a manually reviewed **Last updated** date. Updating prose
or building the site must not silently claim a new provider verification date.
Model/network examples are never live-tested by the documentation workflow.

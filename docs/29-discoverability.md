# 29 — Discoverability: website, llms.txt, listings (2026-11-05)

Owner goal: make the project easier to find for players, search engines and AI assistants, after
pre-release 0.1.0 (docs/28-easy-install.md).

## Plan (owner-approved: parts 1 and 2)
1. **Repo basics**: README hero (screenshots, badges), repo description and topics in the words
   players search ("Moto Racer Mac / Steam Deck / Windows 11 / widescreen"), a social preview image
   (1280×640; uploaded by hand in Settings › Social preview, no API), Discussions on.
2. **Website + AI discoverability**: GitHub Pages site from `site/` (`.github/workflows/pages.yml`,
   deploys on pushes touching `site/`, `llms.txt`), with title / description / Open Graph tags,
   schema.org JSON-LD (`SoftwareApplication` based on the `VideoGame` Moto Racer, and an `FAQPage`
   with the questions players ask), `robots.txt`, `sitemap.xml` and `llms.txt` (also at the repo
   root: a plain summary of the project, install and links for language-model crawlers). Afterwards:
   submit the sitemap in Google Search Console (owner).
3. Later, with the owner: listings — PCGamingWiki (Moto Racer page, essential improvements), Mac
   Source Ports, a Lutris installer, the GOG forum, awesome lists.
4. Later, once Windows and the Deck are tested: launch posts (r/SteamDeck, r/macgaming,
   r/retrogaming, Show HN with a technical write-up, the N64Recomp community), a short video.

Rules: no Moto Racer logo or box art as project branding (screenshots only, "unofficial fan
project" wording kept); anything public (repo settings, posts, pull requests elsewhere) needs the
owner's go.

## Images
`tools/site/make_images.py` makes `site/img/race.jpg`, `race2.jpg`, `menu.jpg`, `setup.png` and
`social-preview.png` from E2E snapshots (`KEEP_FRAMES=1 tools/e2e_race.sh …`; the owner's saved
race mode is 16:10 2560×1600). Screenshots are pictures of the running game, not game files.

E2E fix found on the way: `tools/e2e_race.sh` now starts with `MOUSE:0.01,0.01`. Since the mouse
fix (issue #2) the host cursor over the window highlights menu items for real, and a cursor resting
over the Select Race page turned the scripted `UP UP UP ENTER` into Back.

Seen in the cockpit-view shots: the needles of the cockpit model's own dials are missing (the HUD
gauges are fine) — the open speedometer-needle report; the site uses chase-camera shots.

## Status (2026-11-05)
- Site live at https://segovia-no.github.io/MotoRacerRecomp/ (Pages from Actions, enabled by the
  owner: the API call was outside what the agent may change). `llms.txt`, `sitemap.xml` served.
- Repo: new description, homepage = the site, Discussions on, topics added (recompilation,
  retro-gaming, pc-port, gog, sdl3, steam-deck, racing-game).
- Owner to do: social preview upload (Settings › General), Google Search Console + sitemap.

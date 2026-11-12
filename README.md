# Web browser for KOReader on Kobo

A full-screen touch web browser for Kobo e-readers, opened from
[KOReader](https://github.com/koreader/koreader). Read the web, save articles as books,
and send links from your phone, all on e-ink.

<p align="center">
  <img src="docs/screenshots/reading.png" width="30%" alt="Reading a Wikipedia article">
  <img src="docs/screenshots/menu.png" width="30%" alt="The browser's Menu">
  <img src="docs/screenshots/reader-book.png" width="30%" alt="A web series open in KOReader as a book">
</p>

**Contents:** [Install](#install) · [What it can do](#what-it-can-do) ·
[How to use it](#how-to-use-it) · [Settings](#settings) ·
[Troubleshooting](#troubleshooting) · [Building](#building)

## Install

1. Download `kobo-webbrowser.koplugin-<version>.zip` from the [Releases](../../releases) page.
2. Connect the Kobo over USB and unzip it into `.adds/koreader/plugins/`, so that you have
   `.adds/koreader/plugins/kobo-webbrowser.koplugin/main.lua`.
3. Eject the Kobo and restart KOReader.

Then open it from **Tools → Web browser → Open web browser**. A long-press on
**Web browser** opens it straight away. You can also put it on a gesture or launcher
button with the **Launch web browser** action (Gesture manager → General).

Updating: unzip the new version over the old one. Your bookmarks, history and settings are
kept.

**AppStore or Storefront users:** search for `kobo-webbrowser`, open the latest release and
install the `kobo-webbrowser.koplugin-<version>.zip` asset. Don't use "Direct download from
repo" or "Source code": the browser program itself is only in the release zip.

**Needs:** a Kobo with firmware 4.x (it uses the Qt and WebKit libraries in Kobo's
firmware) and KOReader. Developed and tested on a Kobo Clara BW (firmware 4.46) with
KOReader v2026.07. Other recent MediaTek models (Clara Colour, Libra Colour) should work;
older NXP and Allwinner models are untested.

## What it can do

### Browse comfortably on e-ink

- A one-row toolbar (Back, Fwd, address, Menu). Everything else is in the **Menu**,
  grouped into Go to, This page and Settings.
- Flick up or down to turn a full screen (a small strip of overlap keeps your place; left
  and right flicks work too), or touch, wait a moment and drag to scroll just a little.
  Hold and drag sideways on a wide table to slide it. Pinch to zoom, and use **Text size**
  (a small bar along the bottom with smaller, bigger and Done) for a size that is remembered. Text never shrinks below a readable minimum.
- **Contents** jumps to any heading on a long page.
- **Page overview** (experimental, off by default): every screen of the page as a small
  picture, images shown as gray boxes; tap one to jump there.
- Typing uses KOReader's own keyboard, in any screen orientation.
- Screen updates are kept small and clean: only what changed is refreshed, and menus
  open and close in a single flash.

<p align="center">
  <img src="docs/screenshots/article-image.png" width="30%" alt="An article with a diagram, at a smaller text size">
  <img src="docs/screenshots/search.png" width="30%" alt="Search results">
  <img src="docs/screenshots/keyboard.png" width="30%" alt="KOReader's keyboard typing straight into the address bar">
</p>
<p align="center">
  <img src="docs/screenshots/contents.png" width="30%" alt="Contents: jump to a heading">
  <img src="docs/screenshots/text-size.png" width="30%" alt="The Text size bar along the bottom of the page">
</p>

### Save pages and read them in KOReader

- **Save** turns the article into an EPUB in your library (`<KOReader home>/Web`), with
  its pictures and without the clutter around it, and can open it right away.
- **Reader mode** opens the article in KOReader as a temporary book. When you close it, it's
  deleted, and KOReader asks whether to go back to the page.
- **Whole series:** on a page that lists the parts of a story or a guide, both Save and
  Reader mode can fetch every part into one book with a chapter list.

<p align="center">
  <img src="docs/screenshots/read-series.png" width="30%" alt="Reader mode: whole series or just this page">
  <img src="docs/screenshots/reader-contents.png" width="30%" alt="The series as one book, with a chapter list">
  <img src="docs/screenshots/save-series.png" width="30%" alt="Save: whole series or just this page">
</p>
<p align="center">
  <img src="docs/screenshots/reader-return.png" width="60%" alt="Closing the temporary book: go back to the browser or stay in KOReader">
</p>

### Send links from your phone

Open **Menu → From phone** and scan the QR code with your phone. On the page that opens,
paste a link and tap **Open on the Kobo**. Phone and Kobo need to be on the same Wi-Fi, and
the browser needs to be open. The phone link only runs once you open From phone:
**Minimize** keeps it ready while you browse, **Stop** turns it off.

The other way round, **Menu → Share** shows a QR code of the page you're on, so you can
open it on your phone.

<p align="center">
  <img src="docs/screenshots/phone.png" width="30%" alt="Send to Kobo: the QR code, with Minimize and Stop">
  <img src="docs/screenshots/share.png" width="30%" alt="Share: a QR code of the current page">
</p>

### Bookmarks, history and a home page

- **Add bookmark**, **Bookmarks**, **History** and **Set as home** are in the Menu.
- History can be switched off (Menu → History: on/off) and cleared.
- Bookmarks and History are also listed in KOReader's menu (Tools → Web browser), so you
  can open the browser straight onto a page.

<p align="center">
  <img src="docs/screenshots/bookmarks.png" width="30%" alt="Bookmarks">
  <img src="docs/screenshots/koreader-menu.png" width="30%" alt="Web browser in KOReader's Tools menu">
</p>

### Cleaner, better-looking pages

- **Ad blocking** with a built-in list of about 42,000 ad and tracker sites. You can allow
  ads on sites you want to support, or block extra sites.
- **Modern page styles:** the browser engine in Kobo's firmware dates from about 2013. Pages
  written for today's browsers are translated on the fly, so their menus and layouts
  display properly.
- A clear error page with **Try again** when a site can't be reached.

### Works with KOReader and your Kobo

- Swipe down on the toolbar to open KOReader's top menu over the page.
- The power button sleeps through KOReader, so you get your usual screensaver. On wake,
  the browser reopens on the same page. If Wi-Fi isn't back yet, you can read a saved copy.
- Frontlight: swipe up or down along the left edge for brightness, the right edge for
  warmth.

<p align="center">
  <img src="docs/screenshots/wake.png" width="30%" alt="After sleep: reconnecting Wi-Fi, with Close browser and Offline copy">
</p>

## How to use it

| To | Do this |
| --- | --- |
| Turn a page | Swipe up or down (hold and drag to scroll freely) |
| Go back / forward | **Back** / **Fwd**, or swipe in from the left / right edge |
| Type an address or search | Tap the address, type, press **Go** on the keyboard |
| Zoom | Pinch; or **Menu → Text size** for the text only |
| Quick action | Hold the Menu button (Reader mode unless changed in Settings) |
| Open KOReader's menu | Swipe down from the toolbar, or **Menu → KOReader menu** |
| Clear e-ink ghosting | Two-finger tap, or **Menu → Clean screen** |
| Step out to KOReader and come back | **Menu → Minimize**, then **Tools → Web browser → Resume web browser** (same page, scroll position and Back) |
| Exit | **Menu → Exit browser** |
| Force-quit a frozen page | Hold the power button for 5 seconds |

## Settings

In KOReader, under **Tools → Web browser**:

- **Open address…**, **History**, **Bookmarks**: open the browser on a page.
- **Settings**:
  - **Home page** (default `https://lite.duckduckgo.com/lite/`).
  - **Collection for saved articles** (default `Web`; leave empty to skip collections).
  - **Ad blocking**: on/off, sites where ads are allowed, extra sites to block.
  - **Fix modern page styles** (on by default): turn off if a site looks worse with it.
  - **Use the browser's own keyboard** (off by default): its built-in keys instead of KOReader's keyboard.
  - **Type in a bar above the keyboard** (off by default): the earlier one-line bar with shortcut buttons.
  - **Start phone link with the browser** (off by default): Send to Kobo is ready as soon as the browser opens.
  - **Keep the browser's orientation** (off by default): KOReader always turns with the browser while it's open, so its menu and keyboard match the page. With this on, KOReader stays that way after the browser closes.
  - **Hold Menu button**: what holding the browser's Menu button does (default: Reader mode).
- **Close web browser**: closes a browser left open behind KOReader's menu, or ends a minimized one.
- **Show log**: shows the end of the launcher log.

In the browser's **Menu**: History on/off, Ad blocking on/off and Page overview (beta) on/off.

Advanced options are environment variables read by `bin/run.sh` (for example
`KB_AUTOSLEEP_MIN`, `KB_LIGHT_EDGE`, `KB_ESCAPE_HOLD_SEC`); see
[docs/INTERNALS.md](docs/INTERNALS.md).

## Troubleshooting

- **A page looks broken:** try turning **Fix modern page styles** off for a moment, or open
  the page with **Reader mode**, which keeps just the text and pictures.
- **A site doesn't load or misses parts:** the engine is old, so heavy web apps may be slow
  or fail. If a site breaks only with ad blocking on, add it to the allowed sites.
- **"The browser program is missing":** install from the release zip, not from the
  repository's source code.
- **Leftover lines on the screen:** use Menu → Clean screen, or a two-finger tap.
- **Known issues:** Google image search doesn't work yet. Kobo only; on other devices the
  menu entry shows a message and does nothing.
- **Logs:** `kobo-webbrowser.koplugin/bin/launcher.log` (each launch and its exit code) and
  `kobo-webbrowser.koplugin/bin/browser.log` (the browser's own output).

**Coming from applauncher.koplugin?** Remove its "Web browser" entry from
`applauncher.koplugin/apps.lua` and the `launch_kobo_browser` action from its `main.lua`,
or remove that plugin. This plugin registers the same `launch_kobo_browser` action, so
your gestures and launcher buttons keep working. `.adds/kobo-browser/` can then be deleted.

## How it works

The plugin doesn't bring its own browser engine. Kobo's firmware already includes Qt 5.2.1
and QtWebKit (its own "beta" browser uses them), so the plugin carries a small program
that runs on those libraries and draws straight to the e-ink screen. KOReader stays in the
background and takes over again when you exit. Details are in
[docs/INTERNALS.md](docs/INTERNALS.md).

## Building

See [BUILDING.md](BUILDING.md). In short: `./build.sh` cross-compiles with NickelTC in
podman, and `./package.sh` makes the plugin folder and the release zip.

## License

AGPL-3.0-or-later (the same license as KOReader), see [LICENSE](LICENSE). Bundled and
runtime third-party pieces are listed in [THIRD_PARTY.md](THIRD_PARTY.md).

Not affiliated with Kobo or Rakuten.

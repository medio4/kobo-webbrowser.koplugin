# kobo-webbrowser.koplugin

A full-screen touch web browser for Kobo e-readers, started from [KOReader](https://github.com/koreader/koreader).

It doesn't bring its own browser engine. The Kobo firmware already ships Qt 5.2.1 and
QtWebKit (Nickel uses them for its own "beta" browser), so the plugin carries a small
program that runs on those libraries and draws straight to the e-ink panel. KOReader
pauses while the browser is open and comes back when you exit.

<p align="center">
  <img src="docs/screenshots/reading.png" width="30%" alt="Reading a Wikipedia article">
  <img src="docs/screenshots/article-image.png" width="30%" alt="An article with a diagram, at a smaller text size">
  <img src="docs/screenshots/menu.png" width="30%" alt="The Menu, grouped into Go to, This page and Settings">
</p>
<p align="center">
  <img src="docs/screenshots/search.png" width="30%" alt="Search results">
  <img src="docs/screenshots/keyboard.png" width="30%" alt="Built-in on-screen keyboard">
  <img src="docs/screenshots/contents.png" width="30%" alt="Contents: jump to a heading">
</p>
<p align="center">
  <img src="docs/screenshots/bookmarks.png" width="30%" alt="Bookmarks">
  <img src="docs/screenshots/phone.png" width="30%" alt="Send to Kobo: scan the QR code to send links from a phone">
</p>

## Features

- Real WebKit with JavaScript, HTTPS and cookies. Most sites that work in a 2014-era
  browser work here.
- One-row toolbar (Back, Fwd, address, Menu); everything else is in a Menu grouped into
  Go to, This page and Settings.
- Touch scrolling, swipe from the edges for back/forward, pinch and two-finger tap.
- On-screen keyboard that floats over the page; it only opens for a tap on a text box.
- E-ink aware refresh: only the area that changed is updated, the toolbar and keyboard use
  fast waveforms, and a pop-up opening or closing shows up in one clean flash.
- Readable text: a minimum font size, and Text bigger / smaller that is remembered.
- Bookmarks, a home page of your choice, and browsing History (can be turned off). Both
  lists are also in KOReader's menu, to open the browser straight onto a page.
- Ad blocking with a built-in list of ad and tracker hosts, plus your own allowed and
  blocked sites.
- Modern page styles: style sheets written for current browsers (CSS variables, flexbox,
  transforms, `calc()`, `clamp()`) are rewritten into the older forms the Kobo's 2013
  WebKit understands, so slide-in menus stay hidden and layouts hold together.
- Modern page styles: pages written for current browsers are translated on the fly for
  the firmware's 2013-era engine (prefixed transforms and flexbox, CSS variables,
  min()/max()), so off-screen menus stay hidden and layouts hold together. Can be turned off
  under Tools → Web browser → Fix modern page styles.
- **Send to Kobo**: scan the QR code (Menu → From phone) with your phone, paste a link on
  the page that opens, and it loads on the Kobo.
- **Save**: turns the current article into an EPUB (Mozilla Readability plus the article's
  images) in `<KOReader home>/Web`, adds it to a KOReader collection, and can open it in the
  reader right away. On a page that lists the parts of a series, **Save** can fetch every
  part into one book with a chapter list.
- **Read in KOReader**: opens the article (or a whole series) as a temporary book in
  KOReader. Closing it deletes it, and you can go straight back to the page.
- Table of contents for long pages.
- Power button sleeps through KOReader, so you get your normal screensaver; on wake the
  browser reopens on the same page.
- Frontlight: swipe up/down along the left edge for brightness, right edge for warmth.
- KOReader's menus over the page: swipe down from the toolbar to open KOReader's top menu
  without closing the browser.
- Offline copy: if the connection is gone after a wake, you can read the saved copy of
  the page and reload once you're back online.
- Optional: type with KOReader's own keyboard (Tools → Web browser → Use KOReader
  keyboard, experimental).
- Escape hatch: hold the power button to force-quit a frozen page.

## Reader mode and series

On a page that lists the parts of a series (here the Python tutorial), **Save** and
**Read in KOReader** offer the whole series as one book with a chapter list. Read in
KOReader opens it as a temporary book; closing it deletes it and asks whether to go back
to the page. History and Bookmarks are also in KOReader's own menu.

<p align="center">
  <img src="docs/screenshots/save-series.png" width="30%" alt="Save: whole series or just this page">
  <img src="docs/screenshots/read-series.png" width="30%" alt="Read in KOReader: whole series or just this page, removed when closed">
  <img src="docs/screenshots/reader-book.png" width="30%" alt="The series open in KOReader as a temporary book">
</p>
<p align="center">
  <img src="docs/screenshots/reader-contents.png" width="30%" alt="The temporary book's chapter list in KOReader">
  <img src="docs/screenshots/koreader-menu.png" width="30%" alt="Web browser in KOReader's Tools menu, with History and Bookmarks">
</p>
<p align="center">
  <img src="docs/screenshots/reader-return.png" width="60%" alt="Closing the temporary book: go back to the browser or stay in KOReader">
</p>

## Requirements

- A Kobo e-reader with firmware 4.x that includes Qt 5.2.1 and QtWebKit. Developed and
  tested on a **Kobo Clara BW (N365), firmware 4.46**. Other MediaTek models (Clara Colour,
  Libra Colour) are likely to work; NXP and Allwinner models are untested.
- KOReader (tested with v2026.07). The plugin uses KOReader's bundled `fbink`.

## Install

1. Download `kobo-webbrowser.koplugin-<version>.zip` from the
   [Releases](../../releases) page.
2. Connect the Kobo over USB and unzip it into `.adds/koreader/plugins/`, so you end up
   with `.adds/koreader/plugins/kobo-webbrowser.koplugin/main.lua`.
3. Eject, then restart KOReader.

### From AppStore or Storefront

If you use [AppStore](https://github.com/omer-faruq/appstore.koplugin) or
[Storefront](https://github.com/ultimatejimmy/storefront.koplugin), search for
`kobo-webbrowser`, open the latest release and install the
`kobo-webbrowser.koplugin-<version>.zip` asset. Don't use "Direct download from repo" or
"Source code": the repository holds the source, and the browser program itself is only in
the release zip.

Open it from **Tools → Web browser → Open web browser**, or long-press **Web browser** in
that menu to open it straight away. To put it on a gesture or a
launcher button, use the **Launch web browser** action (Gesture manager → General).

### Settings

Under **Tools → Web browser**:

- **Open address…** starts the browser on a URL you type.
- **History** and **Bookmarks** open the browser on a page you visited or saved.
- **Ad blocking**: on/off, sites where ads are allowed, extra sites to block.
- **Fix modern page styles** (on by default): turn off if a page looks worse with it.
- **Home page** (default `https://lite.duckduckgo.com/lite/`).
- **Collection for saved articles** (default `Web`; leave empty to skip collections).
- **Use KOReader keyboard (experimental)**.
- **Close web browser** closes a browser that is open behind KOReader's menu.
- **Show log** shows the end of the launcher log.

Advanced options are environment variables read by `bin/run.sh` (for example
`KB_AUTOSLEEP_MIN`, `KB_LIGHT_EDGE`, `KB_ESCAPE_HOLD_SEC`); see
[docs/INTERNALS.md](docs/INTERNALS.md).

### Logs

- `kobo-webbrowser.koplugin/bin/launcher.log`: each launch and its exit code.
- `kobo-webbrowser.koplugin/bin/browser.log`: the browser's own output.

## Known issues

- Google image search doesn't work yet.
- Kobo only. On other devices the menu entry shows a message and does nothing.
- The engine is the firmware's 2014-era QtWebKit: heavy modern web apps may be slow or
  fail to load.

## Coming from applauncher.koplugin

If you used the earlier setup (`.adds/kobo-browser/` launched by `applauncher.koplugin`),
remove the "Web browser" entry from `applauncher.koplugin/apps.lua` and the
`launch_kobo_browser` action from its `main.lua`, or remove that plugin entirely. This
plugin registers the same `launch_kobo_browser` action id, so existing gestures and
launcher buttons keep working. `.adds/kobo-browser/` can be deleted afterwards.

## Building

See [BUILDING.md](BUILDING.md). In short: `./build.sh` cross-compiles with NickelTC in
podman, and `./package.sh` makes the plugin folder and the release zip.

## License

AGPL-3.0-or-later (the same license as KOReader), see [LICENSE](LICENSE). Bundled and runtime third-party pieces are
listed in [THIRD_PARTY.md](THIRD_PARTY.md).

Not affiliated with Kobo or Rakuten.

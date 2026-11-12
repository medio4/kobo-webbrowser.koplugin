--[[--
Kobo web browser: a full-screen Qt WebKit browser for Kobo e-readers, started from KOReader.

The browser (bin/kobo-browser) uses the Qt 5.2.1 and QtWebKit libraries that ship in the
firmware. It runs in the background while KOReader keeps running (KB_KO_LIVE=1):
- The browser grabs the touchscreen (EVIOCGRAB), so taps never reach KOReader. KOReader
  keeps its input devices open and still gets the power button and the sleep cover.
- While the browser is alive, KOReader's drawing (UIManager _repaint, _refresh, setDirty)
  is turned into no-ops so nothing paints over the browser.
- A power press or cover close (Device:onPowerEvent "Power"/"Suspend") stops the browser
  with SIGTERM; it writes a sleep line (page, scroll, zoom) to the handoff file and quits.
  Then KOReader sleeps its own way (screensaver, Wi-Fi off) and reopens the page on wake.
- Holding the power button for ESCAPE_HOLD_SEC kills a frozen browser without sleeping
  (KOReader's own 2 s power-off hold still applies after that).
- A poll every second notices when the browser has exited, and meanwhile sends KOReader an
  InputEvent so its AutoSuspend stays quiet (the browser's own idle timer does auto-sleep).

The browser talks back through a handoff file (bin/epub-handoff.txt), one request per line:
  saved<TAB>/path/book.epub              add the saved article to a KOReader collection
  open<TAB>/path/book.epub               open it in the reader now
  sleep<TAB>url<TAB>scrollY<TAB>zoom     sleep (power button, cover or the browser's idle
                                         timer); reopen the page on wake
  read<TAB>/path/tmp.epub<TAB>url<TAB>scrollY<TAB>zoom
                                         reader mode: open the temporary EPUB (in bin/reader-tmp,
                                         never the library); when that book is closed, delete it
                                         and offer to reopen the browser on the same page

Text entry always uses KOReader's own keyboard: the
browser writes bin/kbd-request.txt (line 1: url|field|text, line 2: label, rest: current
text) and releases the touchscreen. We notice it within POLL_SEC, lift the draw guard, show
an InputDialog over the browser's frozen picture and answer in bin/kbd-reply.txt (line 1:
go|set|cancel, rest: text); the browser then takes the screen back and repaints itself.

KOReader's top menu over the browser: a swipe down that starts on the browser's toolbar
writes bin/menu-request.txt and freezes the browser the same way. We copy the frozen picture
into the white cover (so whatever KOReader repaints under its menus is the page, not the file
manager), lift the draw guard and open the menu. KOReader works normally until nothing of
ours is left above the cover; then bin/menu-reply.txt says "back" and the browser takes the
screen back on the same page (nothing is reloaded). If KOReader moved on instead (the file
browser / home icon, a book opened from History, ...), the reply is "quit" and the browser
just exits under KOReader's new screen.

@module koplugin.KoboWebBrowser
--]]

local DataStorage = require("datastorage")
local Device = require("device")
local Dispatcher = require("dispatcher")
local Event = require("ui/event")
local InfoMessage = require("ui/widget/infomessage")
local InputDialog = require("ui/widget/inputdialog")
local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")
local lfs = require("libs/libkoreader-lfs")
local logger = require("logger")
local _ = require("gettext")
local ffi = require("ffi")
local ffiutil = require("ffi/util")
local time = require("ui/time")
local T = ffiutil.template
local Blitbuffer = require("ffi/blitbuffer")
local BottomContainer = require("ui/widget/container/bottomcontainer")
local Button = require("ui/widget/button")
local CenterContainer = require("ui/widget/container/centercontainer")
local Font = require("ui/font")
local FrameContainer = require("ui/widget/container/framecontainer")
local Geom = require("ui/geometry")
local GestureRange = require("ui/gesturerange")
local HorizontalGroup = require("ui/widget/horizontalgroup")
local HorizontalSpan = require("ui/widget/horizontalspan")
local ImageWidget = require("ui/widget/imagewidget")
local InputContainer = require("ui/widget/container/inputcontainer")
local OverlapGroup = require("ui/widget/overlapgroup")
local ProgressWidget = require("ui/widget/progresswidget")
local Size = require("ui/size")
local TextBoxWidget = require("ui/widget/textboxwidget")
local VerticalGroup = require("ui/widget/verticalgroup")
local VerticalSpan = require("ui/widget/verticalspan")

local LOG_MAX_BYTES = 256 * 1024
local DEFAULT_HOME = "https://lite.duckduckgo.com/lite/"
local DEFAULT_COLLECTION = "Web"

local function shellQuote(s)
    return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

pcall(ffi.cdef, "int kill(int pid, int sig);")   -- may already be declared
local SIGTERM, SIGKILL = 15, 9
local POLL_SEC = 0.25        -- how often we check that the browser is still running / wants text
local ACTIVITY_EVERY = 4     -- polls between the InputEvents that keep AutoSuspend quiet
-- SIGTERM grace before SIGKILL when going to sleep. The browser saves the screen (PNG)
-- and the page before it writes its sleep line, which can take a second or two.
local STOP_WAIT_SEC = 4
-- A resume this soon after KOReader's Suspend event, with no power key / sleep cover
-- event since the sleep request and no kernel suspend in between, is a bounced suspend:
-- suspend again (once). Any real wake is never re-suspended.
local BOUNCE_SEC = 3
-- More kernel suspend time than this since the sleep request: the device really slept.
local KERNEL_SLEPT_SEC = 1
-- Events that mean the user woke the device.
local WAKE_EVENTS = { "PowerPress", "PowerRelease", "SleepCoverOpened" }
-- Power held this long while the browser runs: kill it without sleeping. Must stay under
-- the 2 s after which KOReader's PowerPress handler powers the device off.
local ESCAPE_HOLD_SEC = 1.5

-- The UIManager functions everything KOReader draws goes through; they do nothing while
-- the browser owns the screen.
local DRAW_FUNCS = { "_repaint", "_refresh", "setDirty" }
-- Widgets that bypass UIManager (home-screen clocks that paintTo(Screen.bb) and call a
-- Screen:refresh* method from their own timer) are stopped at the framebuffer: every
-- refresh method becomes a no-op and Screen.bb is swapped for an offscreen buffer.
local SCREEN_FUNCS = { "refreshFull", "refreshPartial", "refreshNoMergePartial", "refreshFlashPartial",
    "refreshUI", "refreshNoMergeUI", "refreshFlashUI", "refreshFast", "refreshA2", "refreshWaitForLast" }
local function nop() end

-- How long the wake screen waits for Wi-Fi before offering only Exit / Cancel.
local WAKE_TIMEOUT_SEC = 30
local WAKE_POLL_SEC = 0.5

-- Plain white full-screen widget, shown for as long as a browser session lasts (including
-- its sleeps). Whatever KOReader repaints in the gaps (an aborted suspend, the moment
-- after waking) is then white instead of the home screen.
local WhiteCover = InputContainer:extend{
    name = "kobowebbrowser_cover",
    covers_fullscreen = true,
}
function WhiteCover:init()
    local Screen = Device.screen
    self.dimen = Geom:new{ x = 0, y = 0, w = Screen:getWidth(), h = Screen:getHeight() }
end
function WhiteCover:paintTo(bb, x, y)
    bb:paintRect(x, y, self.dimen.w, self.dimen.h, Blitbuffer.COLOR_WHITE)
end

-- Wake screen: the browser's last screen (sleep-shot.png) with a box at the bottom that
-- shows the Wi-Fi reconnection, and Exit / Cancel buttons.
local WakeScreen = InputContainer:extend{
    name = "kobowebbrowser_wake",
    covers_fullscreen = true,
    image_file = nil,
    on_exit = nil,       -- leave the browser
    on_cancel = nil,     -- stop waiting, open the saved copy offline
}
function WakeScreen:init()
    local Screen = Device.screen
    local sw, sh = Screen:getWidth(), Screen:getHeight()
    self.dimen = Geom:new{ x = 0, y = 0, w = sw, h = sh }
    local box_w = math.floor(sw * 0.86)
    local inner_w = box_w - 2 * Size.padding.large - 2 * Size.border.window
    self.text = TextBoxWidget:new{
        text = _("Reconnecting Wi-Fi…"),
        face = Font:getFace("cfont", 24),
        width = inner_w,
        alignment = "center",
    }
    self.progress = ProgressWidget:new{
        width = inner_w,
        height = Screen:scaleBySize(16),
        percentage = 0,
        ticks = nil,
        last = nil,
    }
    local btn_w = math.floor((inner_w - Size.span.horizontal_default) / 2)
    local buttons = HorizontalGroup:new{
        Button:new{
            text = _("Close browser"),
            width = btn_w,
            callback = function() if self.on_exit then self.on_exit() end end,
        },
        HorizontalSpan:new{ width = Size.span.horizontal_default },
        Button:new{
            text = _("Offline copy"),
            width = btn_w,
            callback = function() if self.on_cancel then self.on_cancel() end end,
        },
    }
    local hint = TextBoxWidget:new{
        text = _("Offline copy: this page as saved before sleep, no Wi-Fi needed."),
        face = Font:getFace("cfont", 18),
        width = inner_w,
        alignment = "center",
    }
    self.box = FrameContainer:new{
        background = Blitbuffer.COLOR_WHITE,
        bordersize = Size.border.window,
        radius = Size.radius.window,
        padding = Size.padding.large,
        VerticalGroup:new{
            align = "center",
            self.text,
            VerticalSpan:new{ width = Size.span.vertical_large },
            self.progress,
            VerticalSpan:new{ width = Size.span.vertical_large },
            buttons,
            VerticalSpan:new{ width = Size.span.vertical_default },
            hint,
        },
    }
    local layers = OverlapGroup:new{ dimen = self.dimen:copy() }
    -- Without the screenshot the box still shows, on white.
    if not self.image_file then
        logger.warn("KoboWebBrowser: wake screen: no screenshot file given")
    elseif lfs.attributes(self.image_file, "mode") ~= "file" then
        logger.warn("KoboWebBrowser: wake screen: no screenshot at", self.image_file, "(the browser didn't save one)")
    else
        local ok, img = pcall(function()
            local w = ImageWidget:new{
                file = self.image_file,
                width = sw,
                height = sh,
                scale_factor = 0,      -- fit
                file_do_cache = false,
            }
            w:_render()                -- load now, so a bad PNG fails here and not in paintTo
            return w
        end)
        if ok and img then
            table.insert(layers, CenterContainer:new{ dimen = self.dimen:copy(), img })
        else
            logger.warn("KoboWebBrowser: wake screen: can't load", self.image_file, img)
        end
    end
    local box_size = self.box:getSize()
    -- OverlapGroup ignores a child's dimen.y; overlap_offset is how it places one lower down.
    local bottom = CenterContainer:new{
        dimen = Geom:new{ w = sw, h = box_size.h },
        self.box,
    }
    bottom.overlap_offset = { 0, sh - box_size.h - Screen:scaleBySize(40) }
    table.insert(layers, bottom)
    self.layers = layers
    self[1] = layers
end
function WakeScreen:paintTo(bb, x, y)
    bb:paintRect(x, y, self.dimen.w, self.dimen.h, Blitbuffer.COLOR_WHITE)
    self.layers:paintTo(bb, x, y)
end
function WakeScreen:setStatus(text, percentage)
    if text then self.text:setText(text) end
    if percentage then self.progress:setPercentage(percentage) end
    UIManager:setDirty(self, function() return "ui", self.box.dimen end)
end
function WakeScreen:onCloseWidget()
    if self.layers then self.layers:free() end
end

local function alive(pid)
    return pid ~= nil and ffi.C.kill(pid, 0) == 0
end

-- Busy-wait (we're inside an event handler) until pid has exited; true if it did.
local function waitGone(pid, seconds)
    local steps = math.ceil(seconds / 0.05)
    for _ = 1, steps do
        if not alive(pid) then return true end
        ffiutil.usleep(50000)
    end
    return not alive(pid)
end

-- The plugin instance whose browser is running (plugins are re-created when switching
-- between file manager and reader; the browser outlives neither, but be safe).
local active = nil

-- Reader mode: the temporary EPUB being read in KOReader and the page to go back to
-- ({ file, url, y, zoom }). Module-level like `active`: the ReaderUI showing the book has
-- its own plugin instance.
local reading = nil
-- Remote launch (for testing over SSH): a file /tmp/kobo-webbrowser-launch makes the
-- current KOReader screen's instance open the browser, on the URL in the file if any.
-- One cheap file check every few seconds; nothing else runs.
local LAUNCH_REQUEST = "/tmp/kobo-webbrowser-launch"
local latest_instance = nil
local launch_poll_started = false

local function lastLines(path, n)
    local f = io.open(path, "r")
    if not f then return "" end
    local size = f:seek("end")
    f:seek("set", math.max(0, size - 4096))
    local lines = {}
    for line in f:lines() do lines[#lines + 1] = line end
    f:close()
    return table.concat(lines, "\n", math.max(1, #lines - n + 1))
end

local KoboWebBrowser = WidgetContainer:extend{
    name = "kobowebbrowser",
    is_doc_only = false,
}

-- KOReader's own directory as an absolute path. DataStorage:getDataDir() is "." when
-- KOReader runs from its folder, and run.sh does a cd before it needs fbink/fbdepth.
local function koreaderDir()
    local d = DataStorage:getDataDir() or "."
    if not d:match("^/") then
        local cwd = lfs.currentdir() or "/mnt/onboard/.adds/koreader"
        d = (d == "." or d == "") and cwd or (cwd .. "/" .. d)
    end
    return d
end

function KoboWebBrowser:init()
    -- KOReader hands plugins a path relative to its own directory ("plugins/x.koplugin");
    -- our launch command does a `cd`, so every path we use has to be absolute.
    if not self.path:match("^/") then
        self.path = (lfs.currentdir() or ".") .. "/" .. self.path
    end
    self.bin = self.path .. "/bin"
    self.script = self.bin .. "/run.sh"
    self.handoff = self.bin .. "/epub-handoff.txt"
    self.log_file = self.bin .. "/launcher.log"
    self.rc_file = self.bin .. "/exit-code"
    self.kbd_request = self.bin .. "/kbd-request.txt"
    self.kbd_reply = self.bin .. "/kbd-reply.txt"
    self.menu_request = self.bin .. "/menu-request.txt"
    self.menu_reply = self.bin .. "/menu-reply.txt"
    self.sleep_shot = self.bin .. "/sleep-shot.png"
    self.sleep_page = self.bin .. "/sleep-page.html"
    self.reader_dir = self.bin .. "/reader-tmp"
    self.settings = G_reader_settings:readSetting("kobo_webbrowser") or {}
    -- e.g. after reinstalling the plugin (bin/ is new); not while a browser runs, whose
    -- Menu may have just changed the on/off file
    if not active then pcall(self.writeAdblockFiles, self) end
    -- Reader-mode copies left over from a crash or a restart while reading.
    if not reading then self:clearReaderDir() end
    self:onDispatcherRegisterActions()
    self.ui.menu:registerToMainMenu(self)
    latest_instance = self
    if not launch_poll_started then
        launch_poll_started = true
        local function poll()
            if lfs.attributes(LAUNCH_REQUEST, "mode") == "file" then
                local f = io.open(LAUNCH_REQUEST, "r")
                local url = f and (f:read("*l") or "") or ""
                if f then f:close() end
                os.remove(LAUNCH_REQUEST)
                local me = latest_instance
                if me and not (active and active.job) then
                    logger.info("KoboWebBrowser: remote launch request", url)
                    local extra
                    if url:match("^https?://") then extra = { args = { url }, env = {} } end
                    me:launch(extra)
                end
            end
            UIManager:scheduleIn(3, poll)
        end
        UIManager:scheduleIn(3, poll)
    end
end

function KoboWebBrowser:saveSettings()
    G_reader_settings:saveSetting("kobo_webbrowser", self.settings)
end

function KoboWebBrowser:onDispatcherRegisterActions()
    -- Same id as the old applauncher.koplugin action, so existing gestures and
    -- ZenOS launcher buttons keep working after switching to this plugin.
    Dispatcher:registerAction("launch_kobo_browser", {
        category = "none",
        event = "LaunchKoboBrowser",
        title = _("Launch web browser"),
        general = true,
    })
end

function KoboWebBrowser:addToMainMenu(menu_items)
    menu_items.kobo_webbrowser = {
        text = _("Web browser"),
        sorting_hint = "tools",
        help_text = _("Tap for the browser's options. Long-press to open the browser right away."),
        -- Long-press opens the browser straight away (like holding the SSH entry).
        hold_callback = function(touchmenu_instance)
            if self.menu_session then return end   -- already open over KOReader
            if touchmenu_instance then touchmenu_instance:closeMenu() end
            UIManager:nextTick(function() self:onLaunchKoboBrowser() end)
        end,
        sub_item_table = {
            {
                text_func = function()
                    if self.menu_session then return _("Back to the web browser") end
                    local m = self.settings.minimized
                    if m then
                        local name = (m.title and m.title ~= "") and m.title or m.url or ""
                        if #name > 40 then name = name:sub(1, 38) .. "…" end
                        return T(_("Resume web browser: %1"), name)
                    end
                    return _("Open web browser")
                end,
                callback = function()
                    -- Opened over a running browser: closing the menu goes back to it.
                    if not self.menu_session then self:onLaunchKoboBrowser() end
                end,
            },
            {
                text = _("Close web browser"),
                help_text = _("Closes a browser left open behind this menu, or ends a minimized one so the next start opens the home page."),
                enabled_func = function() return self.menu_session ~= nil or self.settings.minimized ~= nil end,
                keep_menu_open = true,
                callback = function(touchmenu_instance)
                    if self.menu_session then
                        self:closeFromMenu()
                        return
                    end
                    -- Minimized: forget the page and its Back/Fwd, so nothing is resumed.
                    self.settings.minimized = nil
                    self:saveSettings()
                    os.remove(self.bin .. "/session-history.dat")
                    logger.info("KoboWebBrowser: minimized session closed")
                    if touchmenu_instance then touchmenu_instance:updateItems() end
                    UIManager:show(InfoMessage:new{ text = _("Web browser closed."), timeout = 2 })
                end,
                separator = true,
            },
            {
                text = _("Open address…"),
                keep_menu_open = true,
                callback = function() self:askUrl() end,
            },
            {
                text = _("History"),
                help_text = _("Pages opened in the browser, newest first. Tap one to open the browser on it. The browser's own Menu can turn History off or clear it."),
                enabled_func = function() return self.menu_session == nil end,
                sub_item_table_func = function() return self:pageListMenu("history.txt", _("No history yet, or History is turned off in the browser's Menu.")) end,
            },
            {
                text = _("Bookmarks"),
                enabled_func = function() return self.menu_session == nil end,
                sub_item_table_func = function() return self:pageListMenu("bookmarks.txt", _("No bookmarks yet. In the browser: Menu > Add bookmark.")) end,
            },
            {
                text = _("Settings"),
                separator = true,
                sub_item_table = {
                    {
                        text_func = function()
                            return T(_("Home page: %1"), self.settings.home or DEFAULT_HOME)
                        end,
                        keep_menu_open = true,
                        separator = true,
                        callback = function(touchmenu_instance)
                            self:askSetting("home", _("Home page"), DEFAULT_HOME, touchmenu_instance)
                        end,
                    },
                    {
                        text_func = function()
                            return T(_("Collection for saved articles: %1"), self.settings.collection or DEFAULT_COLLECTION)
                        end,
                        keep_menu_open = true,
                        callback = function(touchmenu_instance)
                            self:askSetting("collection", _("Collection for saved articles"), DEFAULT_COLLECTION, touchmenu_instance)
                        end,
                    },
                    {
                        text = _("Ad blocking"),
                        sub_item_table = self:adblockMenu(),
                    },
                    {
                        text = _("Fix modern page styles"),
                        help_text = _("Rewrites style sheets that use newer CSS (variables, flexbox, transforms, calc) into the older forms this browser understands, so menus and layouts look as intended. Turn it off if a page looks worse with it. Applies from the next page you open."),
                        checked_func = function() return self.settings.css_compat_off ~= true end,
                        callback = function()
                            self.settings.css_compat_off = (self.settings.css_compat_off ~= true) or nil
                            self:saveSettings()
                            self:writeAdblockFiles()
                        end,
                    },
                    {
                        text = _("Use the browser's own keyboard"),
                        help_text = _("Type with the browser's built-in keys instead of KOReader's keyboard. Off by default; a fallback in case a KOReader update changes its keyboard. Takes effect the next time the browser opens."),
                        checked_func = function() return self.settings.builtin_keyboard == true end,
                        callback = function()
                            self.settings.builtin_keyboard = (self.settings.builtin_keyboard ~= true) or nil
                            self:saveSettings()
                        end,
                    },
                    {
                        text = _("Type in a bar above the keyboard"),
                        help_text = _("KOReader's keyboard with a one-line text bar and shortcut buttons, instead of typing straight into the page. Off by default."),
                        enabled_func = function() return self.settings.builtin_keyboard ~= true end,
                        checked_func = function() return self.settings.kbd_dialog == true end,
                        callback = function()
                            self.settings.kbd_dialog = (self.settings.kbd_dialog ~= true) or nil
                            self:saveSettings()
                        end,
                    },
                    {
                        text = _("Start phone link with the browser"),
                        help_text = _("Turn on Send to Kobo (Menu > From phone) as soon as the browser opens, instead of when you open From phone. Off by default."),
                        checked_func = function() return self.settings.phone_autostart == true end,
                        callback = function()
                            self.settings.phone_autostart = (self.settings.phone_autostart ~= true) or nil
                            self:saveSettings()
                        end,
                    },
                    {
                        text = _("Keep the browser's orientation"),
                        help_text = _("While the browser is open, KOReader turns with it, so its menu and keyboard match the page. When the browser closes or is minimized, this keeps that orientation. Off: KOReader goes back to its own orientation."),
                        checked_func = function() return self.settings.keep_rotation == true end,
                        callback = function()
                            self.settings.keep_rotation = (self.settings.keep_rotation ~= true) or nil
                            self:saveSettings()
                        end,
                    },
                    {
                        text_func = function()
                            return T(_("Hold Menu button: %1"), self:holdMenuLabel())
                        end,
                        help_text = _("What holding the browser's Menu button does. A short tap still opens the Menu."),
                        sub_item_table_func = function() return self:holdMenuChoices() end,
                    },
                },
            },
            {
                text = _("Show log"),
                keep_menu_open = true,
                callback = function()
                    local text = lastLines(self.log_file, 25)
                    UIManager:show(InfoMessage:new{
                        text = text ~= "" and text or _("The log is empty."),
                        monospace_font = true,
                    })
                end,
            },
        },
    }
end

-- Quick actions for holding the browser's Menu button (bin/hold-menu.txt holds the key).
local HOLD_ACTIONS = {
    { "reader", _("Reader mode") },
    { "menu", _("Open the Menu") },
    { "textsize", _("Text size") },
    { "contents", _("Contents") },
    { "bookmarks", _("Bookmarks") },
    { "history", _("History") },
    { "addbookmark", _("Add bookmark") },
    { "save", _("Save") },
    { "home", _("Home") },
    { "reload", _("Reload") },
    { "clean", _("Clean screen") },
    { "rotate", _("Rotate screen") },
    { "keyboard", _("Keyboard") },
    { "phone", _("From phone") },
    { "screenshot", _("Screenshot") },
    { "koreader", _("KOReader menu") },
    { "minimize", _("Minimize") },
}

function KoboWebBrowser:holdMenuLabel()
    local key = self.settings.hold_menu or "reader"
    for _i, a in ipairs(HOLD_ACTIONS) do
        if a[1] == key then return a[2] end
    end
    return HOLD_ACTIONS[1][2]
end

function KoboWebBrowser:holdMenuChoices()
    local items = {}
    for _i, a in ipairs(HOLD_ACTIONS) do
        table.insert(items, {
            text = a[2],
            radio = true,
            checked_func = function() return (self.settings.hold_menu or "reader") == a[1] end,
            callback = function()
                self.settings.hold_menu = (a[1] ~= "reader") and a[1] or nil
                self:saveSettings()
                self:writeAdblockFiles()
            end,
        })
    end
    return items
end

function KoboWebBrowser:askSetting(key, title, default, touchmenu_instance)
    local dialog
    dialog = InputDialog:new{
        title = title,
        input = self.settings[key] or default,
        buttons = {{
            {
                text = _("Default"),
                callback = function()
                    self.settings[key] = nil
                    self:saveSettings()
                    UIManager:close(dialog)
                    if touchmenu_instance then touchmenu_instance:updateItems() end
                end,
            },
            {
                text = _("Cancel"),
                id = "close",
                callback = function() UIManager:close(dialog) end,
            },
            {
                text = _("Save"),
                is_enter_default = true,
                callback = function()
                    local v = dialog:getInputText()
                    self.settings[key] = (v ~= "" and v ~= default) and v or nil
                    self:saveSettings()
                    UIManager:close(dialog)
                    if touchmenu_instance then touchmenu_instance:updateItems() end
                end,
            },
        }},
    }
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

-- Ad blocking settings live in KOReader's settings (they survive reinstalling the plugin)
-- and are copied into bin/ for the browser, which re-reads them at each page load.
-- The browser's own Menu toggle writes bin/adblock-off.txt too, so for on/off that file
-- is what counts while it exists; we copy it back into the settings when the browser exits.
--   adblock_off = true       blocking off
--   adblock_allow = "a\nb"   sites where nothing is blocked
--   adblock_block = "a\nb"   extra sites to block
function KoboWebBrowser:adblockMenu()
    local function listCount(key)
        local n = 0
        for line in (self.settings[key] or ""):gmatch("[^\n]+") do
            if line:match("%S") and not line:match("^%s*#") then n = n + 1 end
        end
        return n
    end
    return {
        {
            text = _("Block ads and trackers"),
            help_text = _("Stops pages from loading ads and trackers from a list of about 42,000 known ad and tracking sites. Pages load faster and with less clutter. A page you open yourself is never blocked. Changes apply from the next page you load."),
            checked_func = function() return not self:adblockOff() end,
            callback = function()
                self.settings.adblock_off = (not self:adblockOff()) or nil
                self:saveSettings()
                self:writeAdblockFiles()
            end,
            separator = true,
        },
        {
            text_func = function()
                return T(_("Sites where ads are allowed: %1"), listCount("adblock_allow"))
            end,
            help_text = _("Nothing is blocked on these sites, and they are never blocked on other sites. Use this for a site that breaks with blocking on. One domain per line, for example example.com (its subdomains are included)."),
            keep_menu_open = true,
            callback = function(touchmenu_instance)
                self:editAdblockList("adblock_allow", _("Sites where ads are allowed"), touchmenu_instance)
            end,
        },
        {
            text_func = function()
                return T(_("Extra sites to block: %1"), listCount("adblock_block"))
            end,
            help_text = _("Your own additions to the block list. One domain per line, for example ads.example.com (its subdomains are included)."),
            keep_menu_open = true,
            callback = function(touchmenu_instance)
                self:editAdblockList("adblock_block", _("Extra sites to block"), touchmenu_instance)
            end,
        },
    }
end

function KoboWebBrowser:editAdblockList(key, title, touchmenu_instance)
    local dialog
    dialog = InputDialog:new{
        title = title,
        description = _("One domain per line."),
        input = self.settings[key] or "",
        allow_newline = true,
        fullscreen = true,
        buttons = {{
            {
                text = _("Cancel"),
                id = "close",
                callback = function() UIManager:close(dialog) end,
            },
            {
                text = _("Save"),
                callback = function()
                    local v = dialog:getInputText() or ""
                    self.settings[key] = v:match("%S") and v or nil
                    self:saveSettings()
                    self:writeAdblockFiles()
                    UIManager:close(dialog)
                    if touchmenu_instance then touchmenu_instance:updateItems() end
                end,
            },
        }},
    }
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

function KoboWebBrowser:adblockOff()
    return lfs.attributes(self.bin .. "/adblock-off.txt", "mode") == "file"
end

-- Copy the ad blocking settings into the files the browser reads.
function KoboWebBrowser:writeAdblockFiles()
    local function put(name, text)
        local path = self.bin .. "/" .. name
        if not text then
            os.remove(path)
            return
        end
        local f = io.open(path .. ".tmp", "w")
        if not f then return end
        f:write(text, "\n")
        f:close()
        os.rename(path .. ".tmp", path)
    end
    put("adblock-off.txt", self.settings.adblock_off == true and "ad blocking turned off in KOReader's menu" or nil)
    put("adblock-allow.txt", self.settings.adblock_allow)
    put("adblock-extra.txt", self.settings.adblock_block)
    -- Not ad blocking, but read the same way (at each page the browser opens).
    put("hold-menu.txt", self.settings.hold_menu or "reader")
    put("css-compat-off.txt", self.settings.css_compat_off == true and "modern page style fixes turned off in KOReader's menu" or nil)
end

-- Menu entries for a page list the browser keeps in bin/ (history.txt, bookmarks.txt:
-- "url<TAB>title" lines). Tapping one launches the browser on that page.
function KoboWebBrowser:pageListMenu(name, empty_text)
    local items = {}
    local f = io.open(self.bin .. "/" .. name, "r")
    if f then
        for line in f:lines() do
            local url, title = line:match("^([^\t]+)\t?(.*)$")
            if url and url:match("^https?://") then
                if not title or title == "" then title = url end
                table.insert(items, {
                    text = title,
                    callback = function() self:launch({ args = { url } }) end,
                })
                if #items >= 100 then break end
            end
        end
        f:close()
    end
    if #items == 0 then
        table.insert(items, { text = empty_text, enabled_func = function() return false end })
    end
    return items
end

function KoboWebBrowser:askUrl()
    local dialog
    dialog = InputDialog:new{
        title = _("Open address"),
        input = "https://",
        buttons = {{
            {
                text = _("Cancel"),
                id = "close",
                callback = function() UIManager:close(dialog) end,
            },
            {
                text = _("Open"),
                is_enter_default = true,
                callback = function()
                    local url = dialog:getInputText()
                    UIManager:close(dialog)
                    if not url:match("^%a[%w+.-]*://") then url = "https://" .. url end
                    self:launch({ args = { url } })
                end,
            },
        }},
    }
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

function KoboWebBrowser:onLaunchKoboBrowser()
    self:launch()
    return true
end

function KoboWebBrowser:launch(extra)
    if not Device:isKobo() then
        UIManager:show(InfoMessage:new{ text = _("The web browser only runs on Kobo e-readers.") })
        return
    end
    if active and active.job then
        logger.warn("KoboWebBrowser: already running")
        return
    end
    if lfs.attributes(self.bin .. "/kobo-browser", "mode") ~= "file" then
        UIManager:show(InfoMessage:new{
            text = T(_("The browser program is missing.\n\nExpected: %1\n\nInstall the plugin from the release zip (kobo-webbrowser.koplugin-<version>.zip), not from the repository's source code."), self.bin .. "/kobo-browser"),
        })
        return
    end
    -- Minimized earlier: reopen on that page (with Back/Fwd), unless asked for another one.
    local m = self.settings.minimized
    if m then
        self.settings.minimized = nil
        self:saveSettings()
        if not extra and m.url and m.url:match("^https?://") then
            extra = { args = { m.url }, env = {} }
            if tonumber(m.y) then extra.env.KB_RESTORE_Y = m.y end
            if tonumber(m.zoom) then extra.env.KB_RESTORE_ZOOM = m.zoom end
            logger.info("KoboWebBrowser: resuming minimized browser on", m.url)
        end
    end
    -- A fresh start: drop anything left over from an earlier sleep/wake, including a
    -- hand-off file a browser wrote just before KOReader restarted (its "sleep" line would
    -- otherwise put the device to sleep when this new session ends).
    os.remove(self.handoff)
    self.resume_launch = nil
    self:deleteSleepFiles()
    if not reading then self:clearReaderDir() end
    -- Let the menu close and the screen settle before the browser takes over.
    UIManager:scheduleIn(0.3, function() self:run(extra) end)
end

function KoboWebBrowser:trimLog()
    local size = lfs.attributes(self.log_file, "size")
    if size and size > LOG_MAX_BYTES then
        os.rename(self.log_file, self.log_file .. ".old")
    end
end

-- extra (optional): { args = { ... }, env = { NAME = value } } for this launch only
-- (used to open an address, or to reopen the browser on the same page after a sleep).
function KoboWebBrowser:run(extra)
    local Screen = Device.screen
    self:trimLog()
    UIManager:forceRePaint()

    -- KB_KO_LIVE=1: SIGTERM writes the sleep handoff line. KB_POWER_DEV=none and
    -- KB_NO_ESCAPE=1: the power button is KOReader's. KB_KO_SLEEP=1: the browser's idle
    -- timer (KB_AUTOSLEEP_MIN, from KOReader's auto-suspend setting) hands sleep to us.
    local env = {
        KB_KO_LIVE = "1",
        KB_KO_SLEEP = "1",
        KB_POWER_DEV = "none",
        KB_NO_ESCAPE = "1",
        KB_HANDOFF_FILE = self.handoff,
        KB_CACHE_DIR = self.bin .. "/cache",
        KB_SLEEP_SHOT_FILE = self.sleep_shot,
        KB_SLEEP_PAGE_FILE = self.sleep_page,
        KB_READER_DIR = self.reader_dir,
    }
    if self.settings.home then env.KB_HOME = self.settings.home end
    self:writeAdblockFiles()
    os.remove(self.kbd_request); os.remove(self.kbd_reply)
    os.remove(self.menu_request); os.remove(self.menu_reply)
    env.KB_MENU_FILE = self.menu_request
    env.KB_MENU_REPLY = self.menu_reply
    if self.settings.phone_autostart then env.KB_PHONE_AUTOSTART = "1" end
    -- Our own pid: the browser stops itself if KOReader goes away (restart, crash).
    local stat = io.open("/proc/self/stat", "r")
    local my_pid = stat and stat:read("*l"):match("^(%d+)")
    if stat then stat:close() end
    if my_pid then env.KB_KOREADER_PID = my_pid end
    -- Typing uses KOReader's keyboard (KB_NATIVE_KBD: older browsers ask for it), unless
    -- Settings > "Use the browser's own keyboard" (a fallback if a KOReader update breaks it).
    if self.settings.builtin_keyboard then
        env.KB_NO_NATIVE_KBD = "1"
    else
        env.KB_NATIVE_KBD = "1"
        env.KB_KBD_FILE = self.kbd_request
        env.KB_KBD_REPLY = self.kbd_reply
    end
    local timeout = G_reader_settings:readSetting("auto_suspend_timeout_seconds")
    if type(timeout) == "number" then
        env.KB_AUTOSLEEP_MIN = tostring(timeout > 0 and math.ceil(timeout / 60) or 0)
    end
    for k, v in pairs(extra and extra.env or {}) do env[k] = v end
    local env_parts, arg_parts = {}, {}
    for k, v in pairs(env) do
        env_parts[#env_parts + 1] = " " .. k .. "=" .. shellQuote(v)
    end
    for _, a in ipairs(extra and extra.args or {}) do
        arg_parts[#arg_parts + 1] = " " .. shellQuote(a)
    end
    -- A background subshell starts run.sh, prints its PID on fd 3 (our pipe) and closes
    -- it, so io.popen returns at once; when run.sh ends it logs and saves the exit code.
    local cmd = table.concat({
        "cd " .. shellQuote(self.bin) .. " || exit 1\n",
        "rm -f " .. shellQuote(self.rc_file) .. "\n",
        "(",
        " echo; echo \"=== $(date '+%F %T') start\";",
        " env KOREADER_DIR=" .. shellQuote(koreaderDir()),
        " SCREEN_W=" .. Screen:getWidth() .. " SCREEN_H=" .. Screen:getHeight(),
        table.concat(env_parts),
        " sh " .. shellQuote(self.script) .. table.concat(arg_parts) .. " </dev/null 3>&- &",
        " bp=$!; echo $bp >&3; exec 3>&-;",
        " wait $bp; rc=$?;",
        " echo \"=== $(date '+%F %T') exit code $rc\"; echo $rc >" .. shellQuote(self.rc_file),
        " ) 3>&1 >>" .. shellQuote(self.log_file) .. " 2>&1 </dev/null &\n",
    })

    logger.info("KoboWebBrowser: starting", self.script)
    local pid
    local f = io.popen(cmd, "r")
    if f then
        pid = tonumber(f:read("*l"))
        f:close()
    end
    if not pid then
        UIManager:show(InfoMessage:new{
            text = T(_("The web browser could not be started.\n\n%1"), lastLines(self.log_file, 8)),
            monospace_font = true,
        })
        return
    end
    logger.info("KoboWebBrowser: browser running, pid", pid)
    self.job = { pid = pid }
    active = self
    self:showCover()
    self:installHooks()
    UIManager:preventStandby()
    self.poll_action = self.poll_action or function() self:poll() end
    UIManager:scheduleIn(POLL_SEC, self.poll_action)
end

function KoboWebBrowser:isRunning()
    return self.job ~= nil and alive(self.job.pid)
end

function KoboWebBrowser:poll()
    if not self.job then return end
    if self.stopping_for_sleep then   -- stopForSleep takes it from here
        UIManager:scheduleIn(POLL_SEC, self.poll_action)
        return
    end
    if alive(self.job.pid) then
        self.poll_tick = (self.poll_tick or 0) + 1
        if self.poll_tick % ACTIVITY_EVERY == 0 then
            -- Counts as user activity, so AutoSuspend doesn't sleep under the browser.
            UIManager:broadcastEvent(Event:new("InputEvent"))
        end
        if not self.menu_session and not self.kbd_dialog then
            -- cheap: one small file read; turns KOReader only when the browser turned
            local ok, err = pcall(self.syncRotation, self)
            if not ok then logger.warn("KoboWebBrowser: rotation sync failed:", err) end
        end
        if self.menu_session then
            local ok, err = pcall(self.checkMenuSession, self)
            if not ok then
                logger.warn("KoboWebBrowser: menu session check failed:", err)
                pcall(self.endMenuSession, self, "back")
            end
        elseif not self.kbd_dialog and lfs.attributes(self.kbd_request, "mode") == "file" then
            local ok, err = pcall(self.onKeyboardRequest, self)
            if not ok then logger.warn("KoboWebBrowser: keyboard request failed:", err) end
        elseif not self.kbd_dialog and lfs.attributes(self.menu_request, "mode") == "file" then
            local ok, err = pcall(self.onMenuRequest, self)
            if not ok then
                logger.warn("KoboWebBrowser: menu request failed:", err)
                pcall(self.endMenuSession, self, "back")
            end
        end
        UIManager:scheduleIn(POLL_SEC, self.poll_action)
    else
        self:onBrowserGone("exit")
    end
end

-- ---- Text entry for the browser with KOReader's keyboard -----------------------------

-- Let KOReader draw again (only widgets marked dirty from now on get painted, i.e. our
-- dialog and its keyboard; the file manager / reader underneath stay as they are).
function KoboWebBrowser:liftDrawGuard()
    local saved = self.hooks
    if not saved or self.guard_lifted then return end
    self.guard_lifted = true
    -- The white cover must not be painted over the page while KOReader's keyboard is up.
    if self.cover then self.cover.paintTo = nop end
    for _, name in ipairs(DRAW_FUNCS) do UIManager[name] = saved[name] end
    local screen = Device.screen
    if saved.screen then
        for _, name in ipairs(SCREEN_FUNCS) do screen[name] = saved.screen[name] end
    end
    if saved.bb then
        self.scratch_bb = screen.bb
        -- KOReader may have turned (with the browser) while it drew into the scratch
        -- buffer: give the real one the same turn, or its menus come up sideways.
        pcall(function() saved.bb:setRotation(screen.bb:getRotation()) end)
        screen.bb = saved.bb
    end
end

function KoboWebBrowser:restoreDrawGuard()
    if not self.guard_lifted then return end
    self.guard_lifted = nil
    if self.cover then self.cover.paintTo = nil end   -- back to WhiteCover:paintTo
    if not self.hooks then return end   -- hooks already removed (browser gone)
    for _, name in ipairs(DRAW_FUNCS) do UIManager[name] = nop end
    local screen = Device.screen
    for _, name in ipairs(SCREEN_FUNCS) do screen[name] = nop end
    if self.scratch_bb then
        screen.bb = self.scratch_bb
        self.scratch_bb = nil
    end
end

local function writeAtomically(path, data)
    local f = io.open(path .. ".tmp", "w")
    if not f then return false end
    f:write(data)
    f:close()
    os.remove(path)
    return os.rename(path .. ".tmp", path)
end

-- Address typing: KOReader's keyboard with a slim one-line field right above it,
-- instead of a dialog box with a title.
local AddressBarDialog = InputDialog:extend{}
function AddressBarDialog:init()
    InputDialog.init(self)
    local vg = self.vgroup
    local found = false
    if vg and self.title_bar then
        for i = 1, #vg do
            if vg[i] == self.title_bar then
                vg[i] = VerticalSpan:new{ width = 0 }
                found = true
            end
        end
        if found and vg.resetLayout then vg:resetLayout() end
    end
    -- Sit right on top of the keyboard (InputDialog centres itself in the space above it).
    local frame = self.movable or self.dialog_frame
    if frame then
        local kb_h = self.keyboard_visible and self._input_widget:getKeyboardDimen().h or 0
        self[1] = BottomContainer:new{
            dimen = Geom:new{ w = self.screen_width, h = self.screen_height - kb_h },
            frame,
        }
    end
end

function KoboWebBrowser:startLiveKeyboard(kind, VirtualKeyboard)
    local keys_path = self.kbd_reply .. ".keys"
    os.remove(keys_path)
    local function send(line)
        local f = io.open(keys_path, "a")
        if f then f:write(line, "\n"); f:close() end
    end
    local session = { live = true }
    local kbd
    local function finish(action)
        if self.kbd_dialog ~= session then return end
        self.kbd_dialog = nil
        -- Guard back up before closing, as for the dialog (nothing repaints over the page).
        self:restoreDrawGuard()
        UIManager:close(kbd)
        if not writeAtomically(self.kbd_reply, action .. "\n") then
            logger.warn("KoboWebBrowser: cannot write", self.kbd_reply)
        end
        pcall(Device.input.resetState, Device.input)
    end
    -- What KOReader's keyboard calls on its text box, forwarded to the browser.
    local box = {}
    function box:addChars(chars)
        if chars == "\n" then
            if kind == "text" then send("add\t\\n") else finish("go") end
            return
        end
        send("add\t" .. chars:gsub("\\", "\\\\"):gsub("\n", "\\n"):gsub("\t", " "))
    end
    function box:delChar() send("del") end
    function box:delWord() send("delword") end
    function box:delToStartOfLine() send("dellinestart") end
    function box:leftChar() send("left") end
    function box:rightChar() send("right") end
    function box:upLine() send("up") end
    function box:downLine() send("down") end
    function box:goToStartOfLine() send("home") end
    function box:goToEndOfLine() send("end") end
    function box:scrollUp() end
    function box:scrollDown() end
    function box:onSwitchingKeyboardLayout() end
    box.onSwitchingKeyboardLayout = nil   -- (only called when present)

    self:liftDrawGuard()
    kbd = VirtualKeyboard:new{ inputbox = box }
    -- In the browser the bottom row gets web keys: ⌥ . / space .com Enter (KOReader's
    -- row, or a home screen's trimmed one, may have no Enter at all). This keyboard only:
    -- it gets its own copy of the rows, then is laid out again.
    pcall(function()
        local rows = kbd.KEYS
        local row = rows and rows[5]
        if not row then return end
        local space, enter
        for _i, k in ipairs(row) do
            if k[2] == " " then space = k end
            if k[2] == "\n" then enter = k end
        end
        if not space then return end
        local function key(text, width) return { label = text, text, text, text, text, width = width } end
        local wide_space = {}
        for name, v in pairs(space) do wide_space[name] = v end
        wide_space.width = 3.5
        local copy = {}
        for i, r in ipairs(rows) do copy[i] = r end
        copy[5] = { row[1], key("."), key("/"), wide_space, key(".com", 1.5),
                    enter or { label = "⮠", "\n", "\n", "\n", "\n", width = 1.5 } }
        kbd.KEYS = copy
        kbd:initLayer(kbd.keyboard_layer)
    end)
    -- KOReader gives a tap only to the topmost window: the keyboard itself ends typing
    -- when a tap lands above it (the text is already in the field).
    local Screen = Device.screen
    local kb_h = kbd.dimen and kbd.dimen.h or kbd.height or 0
    local orig_gesture = kbd.onGesture
    kbd.onGesture = function(w, ev)
        if orig_gesture and orig_gesture(w, ev) then return true end
        if ev and ev.ges == "tap" and ev.pos and ev.pos.y < Screen:getHeight() - kb_h then
            finish("done")
            return true
        end
    end
    self.kbd_dialog = session
    UIManager:show(kbd)
    send("live\t" .. tostring(kb_h))
    logger.info("KoboWebBrowser: live keyboard,", kind, "height", kb_h)
end

function KoboWebBrowser:onKeyboardRequest()
    local f = io.open(self.kbd_request, "r")
    if not f then return end
    local data = f:read("*a") or ""
    f:close()
    os.remove(self.kbd_request)
    local kind, label, text = data:match("^([^\n]*)\n([^\n]*)\n?(.*)$")
    kind = kind or "url"
    label = label or ""
    text = text or ""
    logger.info("KoboWebBrowser: keyboard request", kind, label)

    -- Live typing: KOReader's own keyboard alone at the bottom of the screen; every key goes
    -- straight into the browser's field (bin/kbd-reply.txt.keys, one line per key) and the
    -- browser keeps drawing the page above the keyboard. Enter = go; a tap above = done.
    local ok_vk, VirtualKeyboard = pcall(require, "ui/widget/virtualkeyboard")
    if ok_vk and VirtualKeyboard and not self.settings.kbd_dialog then
        local ok_live, err = pcall(self.startLiveKeyboard, self, kind, VirtualKeyboard)
        if ok_live then return end
        logger.warn("KoboWebBrowser: live keyboard failed, using the dialog:", err)
        self:restoreDrawGuard()
    end

    local dialog
    local function finish(action, value)
        if not self.kbd_dialog then return end
        self.kbd_dialog = nil
        -- Re-arm the guard BEFORE closing: UIManager:close marks every widget underneath
        -- dirty and queues a repaint; with the guard up those are no-ops, so the file
        -- manager / reader never gets painted over the browser (now or at the next lift).
        -- The dialog's pixels stay on screen until the browser repaints everything.
        self:restoreDrawGuard()
        UIManager:close(dialog)
        if not writeAtomically(self.kbd_reply, action .. "\n" .. (value or "")) then
            logger.warn("KoboWebBrowser: cannot write", self.kbd_reply)
        end
        pcall(Device.input.resetState, Device.input)
    end
    local buttons = {
        { text = _("Cancel"), id = "close", callback = function() finish("cancel") end },
        { text = _("Clear"), callback = function()
            dialog:setInputText("")
        end },
    }
    if kind == "url" then
        -- Web shortcuts the keyboard has no key for.
        for _i, t in ipairs({ "/", ".", ".com", "www." }) do
            buttons[#buttons + 1] = { text = t, callback = function() dialog:addTextToInput(t) end }
        end
    end
    if kind ~= "url" then
        buttons[#buttons + 1] = { text = _("Set"), callback = function() finish("set", dialog:getInputText()) end }
    end
    buttons[#buttons + 1] = {
        text = kind == "url" and _("Go") or _("Enter"),
        is_enter_default = true,
        callback = function() finish("go", dialog:getInputText()) end,
    }
    local title
    if kind == "url" then
        title = _("Address or search")
    elseif label ~= "" then
        title = label
    else
        title = _("Text field")
    end
    self:liftDrawGuard()
    dialog = (kind == "url" and AddressBarDialog or InputDialog):new{
        title = title,
        input = text,
        input_hint = kind == "url" and _("https://example.com or search words") or nil,
        allow_newline = kind == "text",
        cursor_at_end = true,
        buttons = { buttons },
    }
    self.kbd_dialog = dialog
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

-- ---- KOReader's menus over the browser ------------------------------------------------

-- The UI the menu was opened in (FileManager or ReaderUI), and whether it's still shown.
-- (FileManager has no name of its own: compare with the class / its instance.)
local function isKoreaderUI(w)
    if not w then return false end
    local FileManager = package.loaded["apps/filemanager/filemanager"]
    local ReaderUI = package.loaded["apps/reader/readerui"]
    if FileManager and (w == FileManager.instance or getmetatable(w) == FileManager) then return true end
    if ReaderUI and (w == ReaderUI.instance or getmetatable(w) == ReaderUI) then return true end
    return w.name == "ReaderUI"
end
local function uiName(w)
    local ReaderUI = package.loaded["apps/reader/readerui"]
    if ReaderUI and (w == ReaderUI.instance or getmetatable(w) == ReaderUI) then return "reader" end
    return w and "file manager" or "nothing"
end

-- The KOReader screen under our cover (topmost FileManager / ReaderUI below it). Not
-- self.ui: this plugin instance may belong to a screen that has been replaced since.
function KoboWebBrowser:uiUnderCover()
    local stack, ui = UIManager._window_stack, nil
    for _, win in ipairs(stack) do
        if win.widget == self.cover then break end
        if isKoreaderUI(win.widget) then ui = win.widget end
    end
    return ui or self.ui
end

-- The browser froze and handed us the touchscreen: open KOReader's top menu over it.
function KoboWebBrowser:onMenuRequest()
    os.remove(self.menu_request)
    local ui = self:uiUnderCover()
    local menu = ui and ui.menu
    if not (menu and menu.onShowMenu and self.hooks and self.cover) then
        logger.warn("KoboWebBrowser: menu request, but no menu to show")
        writeAtomically(self.menu_reply, "back\n")
        return
    end
    logger.info("KoboWebBrowser: opening KOReader's menu over the browser, in the", uiName(ui))
    -- Copy the browser's frozen picture off the real framebuffer: the cover paints it, so
    -- whatever KOReader repaints under its menus (a menu getting shorter, a dialog closing)
    -- shows the page instead of stale menu pixels or the file manager.
    -- Read the page in KOReader's current turn (it turns with the browser).
    if self.hooks.bb then pcall(function() self.hooks.bb:setRotation(Device.screen.bb:getRotation()) end) end
    local real = self.hooks.bb or Device.screen.bb
    local snap
    local ok, err = pcall(function()
        snap = Blitbuffer.new(real:getWidth(), real:getHeight(), real:getType())
        snap:blitFrom(real, 0, 0, 0, 0, real:getWidth(), real:getHeight())
    end)
    if ok then
        -- The browser's toolbar (top ~220 px) always has black text and borders. No dark
        -- pixel there means we're not reading what the browser drew (wrong buffer/depth).
        local dark, n = 0, 0
        pcall(function()
            local w = snap:getWidth()
            local h = math.min(220, snap:getHeight())
            for yy = 0, h - 1, 3 do
                for xx = 0, w - 1, 5 do
                    n = n + 1
                    if snap:getPixel(xx, yy):getColor8().a < 0x80 then dark = dark + 1 end
                end
            end
        end)
        logger.info("KoboWebBrowser: page copy", snap:getWidth(), "x", snap:getHeight(), "type", snap:getType(),
            "-", dark, "of", n, "toolbar samples dark")
        if n > 0 and dark == 0 then
            -- Keep KOReader's hands off the page instead (the keyboard's way: the cover
            -- paints nothing; stale menu pixels may linger until the browser repaints).
            logger.warn("KoboWebBrowser: page copy is blank, not painting it")
            pcall(snap.free, snap)
            snap = nil
        end
    end
    if not ok then
        logger.warn("KoboWebBrowser: could not copy the browser's picture:", err)
        if snap and snap.free then pcall(snap.free, snap) end
        snap = nil
    end
    self.menu_session = { snap = snap, ui = ui, empty_polls = 0 }
    self:liftDrawGuard()
    local cover = self.cover
    if snap then
        cover.paintTo = function(_, bb, x, y)
            bb:blitFrom(snap, x, y, 0, 0, snap:getWidth(), snap:getHeight())
        end
        -- Repaint the cover (the page) under anything KOReader repaints above it, so areas
        -- a menu no longer covers get the page back. UIManager only paints from the lowest
        -- dirty widget up, and stops at the cover (it covers the full screen).
        -- Only widgets ABOVE the cover count: the file manager / reader under it stay in
        -- _dirty forever (they're never painted, so never cleared), and counting them made
        -- every UI tick repaint and refresh the whole screen (endless flashing, no input).
        local repaint = UIManager._repaint
        UIManager._repaint = function(uim)
            local stack, dirty = uim._window_stack, uim._dirty
            for i = #stack, 1, -1 do
                local w = stack[i].widget
                if w == cover then break end
                if dirty[w] then dirty[cover] = true; break end
            end
            return repaint(uim)
        end
    end
    -- KOReader exiting or restarting from the menu: take the browser down with it.
    local quit = UIManager.quit
    self.menu_session.quit = quit
    UIManager.quit = function(...)
        if self.job and alive(self.job.pid) then
            logger.info("KoboWebBrowser: KOReader is quitting, stopping the browser")
            pcall(self.stopBrowser, self, SIGKILL)
        end
        return quit(...)
    end
    pcall(Device.input.resetState, Device.input)
    menu:onShowMenu()
end

-- While the menu session lasts (every POLL_SEC): back to the browser once nothing is shown
-- above the cover any more; quit it if KOReader went to another screen.
function KoboWebBrowser:checkMenuSession()
    local m = self.menu_session
    if m.quitting then
        if os.time() - m.quitting > 3 and self.job then
            logger.warn("KoboWebBrowser: browser didn't quit after the menu, killing it")
            self:stopBrowser(SIGKILL)
            self:onBrowserGone("escape")
        end
        return
    end
    local stack = UIManager._window_stack
    local ci, ui_shown
    for i, win in ipairs(stack) do
        if win.widget == self.cover then ci = i end
        if win.widget == m.ui then ui_shown = true end
    end
    local above = 0
    if ci then
        for i = ci + 1, #stack do
            local w = stack[i].widget
            if isKoreaderUI(w) then
                return self:endMenuSession("quit", "KOReader opened the " .. uiName(w))
            end
            if not w.toast then above = above + 1 end
        end
    end
    if not ci or not ui_shown then
        return self:endMenuSession("quit", ci and "the menu's screen was closed" or "the cover is gone")
    end
    if above > 0 then
        m.empty_polls = 0
        return
    end
    -- Two empty polls in a row: a menu item may close the menu and open its dialog a tick later.
    m.empty_polls = m.empty_polls + 1
    if m.empty_polls >= 2 then self:endMenuSession("back") end
end

-- action: "back" (take the browser back, same page) or "quit" (KOReader went elsewhere).
function KoboWebBrowser:endMenuSession(action, why)
    local m = self.menu_session
    if not m or m.quitting then return end
    logger.info("KoboWebBrowser: menu closed:", action, why or "")
    if m.quit and UIManager.quit ~= m.quit then UIManager.quit = m.quit end
    m.quit = nil
    if action == "quit" then
        -- KOReader owns the screen now: leave the guard lifted while the browser exits
        -- (onBrowserGone cleans up), so its new screen keeps drawing.
        m.quitting = os.time()
    else
        -- Guard back up before the browser draws again (also drops our _repaint wrapper).
        self:restoreDrawGuard()
        self:freeMenuSnapshot()
        self.menu_session = nil
    end
    if not writeAtomically(self.menu_reply, action .. "\n") then
        logger.warn("KoboWebBrowser: cannot write", self.menu_reply)
    end
    pcall(Device.input.resetState, Device.input)
end

function KoboWebBrowser:freeMenuSnapshot()
    local m = self.menu_session
    if not m or not m.snap then return end
    local snap = m.snap
    m.snap = nil
    if self.cover then self.cover.paintTo = self.guard_lifted and nop or nil end
    pcall(function() if snap.free then snap:free() end end)
end

-- "Close web browser" in the plugin's menu while the menu is up over the browser.
function KoboWebBrowser:closeFromMenu()
    if not self.menu_session then return end
    local ui = self.menu_session.ui
    local menu = ui and ui.menu
    local close = menu and (menu.onCloseFileManagerMenu or menu.onCloseReaderMenu)
    if close then pcall(close, menu) end
    self:endMenuSession("quit", "Close web browser")
end

-- The browser went away during a menu session (sleep, escape, crash): undo it.
function KoboWebBrowser:dropMenuSession()
    local m = self.menu_session
    if not m then return end
    if m.quit and UIManager.quit ~= m.quit then UIManager.quit = m.quit end
    if not m.quitting then
        -- The menu is still open over the page: close it (sleep reopens the browser).
        local menu = m.ui and m.ui.menu
        local close = menu and menu.menu_container and (menu.onCloseFileManagerMenu or menu.onCloseReaderMenu)
        if close then pcall(close, menu) end
    end
    self:freeMenuSnapshot()
    self.menu_session = nil
end

-- SIGTERM run.sh (it passes the signal on to the browser); if the browser doesn't quit in
-- time, SIGKILL it and then run.sh.
function KoboWebBrowser:stopBrowser(sig, wait)
    local pid = self.job.pid
    if sig == SIGTERM then
        ffi.C.kill(pid, SIGTERM)
        if waitGone(pid, wait) then return end
        logger.warn("KoboWebBrowser: browser ignored SIGTERM, killing it")
    end
    os.execute("kill -KILL $(pidof kobo-browser) 2>/dev/null")
    if not waitGone(pid, 1) then
        ffi.C.kill(pid, SIGKILL)
        waitGone(pid, 0.5)
    end
end

-- Stop the browser for sleep without blocking KOReader's input loop (so a late key event
-- is handled now, not after the screensaver is up): SIGTERM, poll until it has quit
-- (SIGKILL after STOP_WAIT_SEC), then sleep the KOReader way.
function KoboWebBrowser:stopForSleep(dev, ev, orig_power)
    local pid = self.job.pid
    self.stopping_for_sleep = { dev = dev, ev = ev, t = os.time() }
    ffi.C.kill(pid, SIGTERM)
    local ticks = 0
    local function check()
        ticks = ticks + 1
        if alive(pid) and ticks * 0.05 < STOP_WAIT_SEC then
            UIManager:scheduleIn(0.05, check)
            return
        end
        if alive(pid) then
            logger.warn("KoboWebBrowser: browser ignored SIGTERM, killing it")
            self:stopBrowser(SIGKILL)
        end
        self.stopping_for_sleep = nil
        self:onBrowserGone("sleep")
        -- After onBrowserGone: removeHooks puts the original power handlers back.
        self:watchForWake()
        orig_power(dev, ev)
    end
    UIManager:scheduleIn(0.05, check)
end

-- Draw guard, sleep hook and power-hold escape, active while the browser runs.
function KoboWebBrowser:installHooks()
    local saved = {}
    for _, name in ipairs(DRAW_FUNCS) do
        saved[name] = rawget(UIManager, name)
        UIManager[name] = nop
    end
    local screen = Device.screen
    saved.screen = {}
    for _, name in ipairs(SCREEN_FUNCS) do
        saved.screen[name] = rawget(screen, name)
        screen[name] = nop
    end
    -- Paint into a scratch buffer instead of the real framebuffer while the browser owns it.
    local ok_bb, Blitbuffer = pcall(require, "ffi/blitbuffer")
    if ok_bb and screen.bb then
        local ok_new, scratch = pcall(Blitbuffer.new, screen.bb:getWidth(), screen.bb:getHeight(), screen.bb:getType())
        if ok_new and scratch then
            saved.bb = screen.bb
            screen.bb = scratch
        else
            logger.warn("KoboWebBrowser: could not allocate a scratch framebuffer:", scratch)
        end
    end

    -- Power button / sleep cover / AutoSuspend: stop the browser (it saves its page),
    -- then let KOReader do its normal sleep (screensaver, Wi-Fi off, suspend).
    saved.onPowerEvent = rawget(Device, "onPowerEvent")
    local orig_power = Device.onPowerEvent
    Device.onPowerEvent = function(dev, ev)
        if (ev == "Power" or ev == "Suspend") and self.stopping_for_sleep then
            logger.info("KoboWebBrowser: ignoring", ev, "while the browser stops for sleep")
            return
        end
        if (ev == "Power" or ev == "Suspend") and not dev.screen_saver_mode and self:isRunning() then
            logger.info("KoboWebBrowser: sleep requested, stopping the browser")
            UIManager:unschedule(self.escape_action)
            self:stopForSleep(dev, ev, orig_power)
            return
        end
        return orig_power(dev, ev)
    end

    -- Long power hold: kill a frozen browser without sleeping. KOReader's PowerPress
    -- handler (2 s hold = power off) runs unchanged.
    local handlers = UIManager.event_handlers
    saved.PowerPress, saved.PowerRelease = handlers.PowerPress, handlers.PowerRelease
    self.escape_action = self.escape_action or function()
        if not self:isRunning() or self.stopping_for_sleep then return end
        logger.warn("KoboWebBrowser: power held, killing the browser")
        self.escaped_hold = true
        -- KOReader's PowerPress already scheduled its power-off (fires at ~2 s of hold);
        -- cancel it now, or holding a little longer reboots the device.
        if UIManager.poweroff_action then UIManager:unschedule(UIManager.poweroff_action) end
        -- Instant feedback: kill the browser (so it can't draw again) and flash white,
        -- before stopBrowser's wait for run.sh (up to 1.5 s) and the full repaint.
        os.execute("kill -KILL $(pidof kobo-browser) 2>/dev/null")
        self:flashWhite()
        self:stopBrowser(SIGKILL)
        self:onBrowserGone("escape")
    end
    handlers.PowerPress = function(...)
        UIManager:unschedule(self.escape_action)
        if self:isRunning() then UIManager:scheduleIn(ESCAPE_HOLD_SEC, self.escape_action) end
        if saved.PowerPress then return saved.PowerPress(...) end
    end
    handlers.PowerRelease = function(...)
        UIManager:unschedule(self.escape_action)
        if self.escaped_hold then
            -- The end of the hold that killed the browser: don't sleep. (Still cancel
            -- the pending power-off, as KOReader's own release would.)
            self.escaped_hold = nil
            if not UIManager._entered_poweroff_stage then
                UIManager:unschedule(UIManager.poweroff_action)
            end
            if not self.job then handlers.PowerRelease = saved.PowerRelease end
            return
        end
        if saved.PowerRelease then return saved.PowerRelease(...) end
    end
    self.hooks = saved
end

-- Paint the real framebuffer white and flash it, bypassing the draw guard for a moment.
function KoboWebBrowser:flashWhite()
    local saved, screen = self.hooks, Device.screen
    if not saved then return end
    local scratch, guard = screen.bb, screen.refreshFull
    if saved.bb then screen.bb = saved.bb end
    screen.refreshFull = saved.screen.refreshFull   -- nil: the class method
    local ok, err = pcall(function()
        screen.bb:fill(Blitbuffer.COLOR_WHITE)
        -- fb:refreshFull needs the rectangle (UIManager always passes one); without it
        -- getBoundedRect compares nil and the flash never happens.
        screen:refreshFull(0, 0, screen:getWidth(), screen:getHeight())
    end)
    if not ok then logger.warn("KoboWebBrowser: white flash failed:", err) end
    screen.bb, screen.refreshFull = scratch, guard
end

function KoboWebBrowser:removeHooks()
    local saved = self.hooks
    if not saved then return end
    self.hooks = nil
    if self.guard_lifted then
        -- The keyboard dialog had the real screen: only the scratch buffer is left to free.
        self.guard_lifted = nil
        local scratch = self.scratch_bb
        self.scratch_bb = nil
        pcall(function() if scratch and scratch.free then scratch:free() end end)
        saved.bb = nil
    end
    for _, name in ipairs(DRAW_FUNCS) do
        UIManager[name] = saved[name]
    end
    if saved.screen then
        for _, name in ipairs(SCREEN_FUNCS) do
            Device.screen[name] = saved.screen[name]   -- nil: back to the class method
        end
    end
    if saved.bb then
        local scratch = Device.screen.bb
        pcall(function() saved.bb:setRotation(scratch:getRotation()) end)   -- KOReader's current turn
        Device.screen.bb = saved.bb
        pcall(function() if scratch and scratch.free then scratch:free() end end)
    end
    Device.onPowerEvent = saved.onPowerEvent   -- nil: back to the class method
    UIManager:unschedule(self.escape_action)
    local handlers = UIManager.event_handlers
    handlers.PowerPress = saved.PowerPress
    -- After an escape, the release of that hold is still to come; its wrapper puts the
    -- original handler back.
    if not self.escaped_hold then handlers.PowerRelease = saved.PowerRelease end
end

-- The browser is gone. reason: "exit" (it quit by itself: Exit button, crash, or its idle
-- timer asking for sleep), "sleep" (we stopped it for a power/cover/suspend event; the
-- caller sleeps next) or "escape" (killed by a long power hold).
function KoboWebBrowser:onBrowserGone(reason)
    if not self.job then return end
    self.job = nil
    -- Keep an on/off choice made in the browser's own Menu.
    if (self.settings.adblock_off == true) ~= self:adblockOff() then
        self.settings.adblock_off = self:adblockOff() or nil
        self:saveSettings()
    end
    if active == self then active = nil end
    UIManager:unschedule(self.poll_action)
    pcall(self.dropMenuSession, self)
    if self.kbd_dialog then
        local d = self.kbd_dialog
        self.kbd_dialog = nil
        pcall(UIManager.close, UIManager, d)
    end
    self:removeHooks()
    UIManager:allowStandby()
    pcall(Device.input.resetState, Device.input)

    local code
    for _ = 1, 10 do   -- the subshell writes it right after run.sh exits
        local f = io.open(self.rc_file, "r")
        if f then
            code = tonumber(f:read("*l"))
            f:close()
        end
        if code or reason ~= "exit" then break end
        ffiutil.usleep(50000)
    end
    logger.info("KoboWebBrowser: browser gone:", reason, "exit code", code)

    local h = self:readHandoff()
    -- Use a sleep line only when we stopped the browser for sleep, or when it asked
    -- for sleep itself (idle timer). Not after an escape.
    local sleep_req = h.sleep
    if reason == "escape" or h.to_open or h.read or h.minimized then sleep_req = nil end
    if h.minimized and reason == "exit" then
        -- Minimized: remember where it was, so the next launch resumes there.
        self.settings.minimized = h.minimized
        self:saveSettings()
        logger.info("KoboWebBrowser: minimized on", h.minimized.url)
        UIManager:scheduleIn(1.6, function()
            UIManager:show(InfoMessage:new{
                text = _("Web browser minimized. Resume it from Tools → Web browser, or by long-pressing Web browser."),
                timeout = 3,
            })
        end)
    end
    local sleeping = reason == "sleep" or (reason == "exit" and sleep_req ~= nil)

    if sleeping then
        -- KOReader's screensaver goes up next (over the white cover): mark everything for
        -- repaint under it, but don't flash KOReader's UI first.
        UIManager:setDirty("all")
    else
        -- The session is over: no white cover, no temp files.
        self:closeCover()
        self:deleteSleepFiles()
        -- Settings > "Keep the browser's orientation": turn KOReader the way the browser was.
        pcall(self.finishRotation, self)
        -- The browser drew over everything. Repaint KOReader's UI first, then flash the
        -- whole screen (flashing before repainting would just re-show the browser's last
        -- image), and do it again a moment later in case the browser left a refresh in
        -- flight or the e-ink controller merged ours into it.
        local Screen = Device.screen
        UIManager:setDirty("all", "full")
        UIManager:forceRePaint()
        pcall(function() Screen:refreshFull(0, 0, Screen:getWidth(), Screen:getHeight()) end)
        UIManager:scheduleIn(0.6, function()
            UIManager:setDirty("all", "full")
            UIManager:forceRePaint()
        end)
        UIManager:scheduleIn(1.5, function() UIManager:setDirty("all", "full") end)
        -- Counts as user activity, so AutoSuspend doesn't sleep the moment we're back.
        UIManager:broadcastEvent(Event:new("InputEvent"))
    end

    if reason == "exit" and not sleeping and code and code ~= 0 then
        UIManager:show(InfoMessage:new{
            text = T(_("The web browser stopped with an error (code %1).\n\n%2"), code, lastLines(self.log_file, 8)),
            monospace_font = true,
        })
    end
    local ok_h, err = pcall(self.processHandoff, self, h, sleep_req, reason)
    if not ok_h then logger.warn("KoboWebBrowser: handoff failed:", err) end
end

-- The browser's screen angle (bin/rotation.txt: 0, 90, 180 or 270) as a KOReader
-- rotation mode. The browser turns its picture clockwise; KOReader's modes count the
-- device's turn, the other way round: 90 = mode 3 (CCW), 270 = mode 1 (CW).
function KoboWebBrowser:browserRotationMode()
    local f = io.open(self.bin .. "/rotation.txt", "r")
    local angle = f and tonumber(f:read("*l")) or 0
    if f then f:close() end
    return ({ [0] = 0, [90] = 3, [180] = 2, [270] = 1 })[angle] or 0
end

function KoboWebBrowser:setKoreaderRotation(mode, why)
    if not mode or mode == Device.screen:getRotationMode() then return end
    logger.info("KoboWebBrowser: turning KOReader to mode", mode, why or "")
    UIManager:broadcastEvent(Event:new("SetRotationMode", mode))
    -- Our full-screen cover was sized for the old orientation.
    if self.cover and self.cover.dimen then
        self.cover.dimen.w, self.cover.dimen.h = Device.screen:getWidth(), Device.screen:getHeight()
    end
    -- Rotating rebuilds KOReader's screens (a home screen may close and reopen itself),
    -- which can drop our cover or put a screen above it. Put it back on top while the
    -- browser runs, now and once the rebuild has settled.
    if self.job then
        self:keepCoverOnTop()
        UIManager:scheduleIn(0.5, function() self:keepCoverOnTop() end)
    end
end

function KoboWebBrowser:keepCoverOnTop()
    if not (self.job and self.cover) or self.menu_session or self.kbd_dialog then return end
    local stack = UIManager._window_stack
    local top = stack[#stack] and stack[#stack].widget
    if top == self.cover then return end
    local present = false
    for _, win in ipairs(stack) do if win.widget == self.cover then present = true end end
    logger.info("KoboWebBrowser: cover", present and "was under another screen" or "was gone", "after rotating; putting it back on top")
    if present then UIManager:close(self.cover) end
    UIManager:show(self.cover)
end

-- While the browser runs, KOReader (behind it) is turned the same way, so its menu,
-- keyboard and sleep screen match the page. Turned once per browser rotation.
function KoboWebBrowser:syncRotation()
    if self.orig_rotation == nil then self.orig_rotation = Device.screen:getRotationMode() end
    self:setKoreaderRotation(self:browserRotationMode(), "to match the browser")
end

-- The browser is gone for good: keep its orientation or go back to KOReader's own.
function KoboWebBrowser:finishRotation()
    local orig = self.orig_rotation
    self.orig_rotation = nil
    if self.settings.keep_rotation then return end
    if orig ~= nil then self:setKoreaderRotation(orig, "back to KOReader's own orientation") end
end

function KoboWebBrowser:readHandoff()
    local h = { saved = {} }
    local f = io.open(self.handoff, "r")
    if not f then return h end
    for line in f:lines() do
        local kind, rest = line:match("^(%a+)\t(.+)$")
        if kind == "saved" then
            h.saved[#h.saved + 1] = rest
        elseif kind == "open" then
            h.to_open = rest
        elseif kind == "read" then
            local file, url, y, zoom = rest:match("^([^\t]+)\t([^\t]*)\t?([^\t]*)\t?([^\t]*)$")
            if file then h.read = { file = file, url = url, y = y, zoom = zoom } end
        elseif kind == "minimized" then
            local url, y, zoom, title = rest:match("^([^\t]*)\t?([^\t]*)\t?([^\t]*)\t?(.*)$")
            if url then h.minimized = { url = url, y = y, zoom = zoom, title = title } end
        elseif kind == "sleep" then
            local url, y, zoom = rest:match("^([^\t]*)\t?([^\t]*)\t?([^\t]*)$")
            h.sleep = { url = url, y = y, zoom = zoom }
        end
    end
    f:close()
    os.remove(self.handoff)
    return h
end

function KoboWebBrowser:processHandoff(h, sleep_req, reason)
    local saved, to_open = h.saved, h.to_open
    logger.info("KoboWebBrowser: handoff:", #saved, "saved,", to_open and ("open " .. to_open) or "nothing to open")
    local collection = self.settings.collection or DEFAULT_COLLECTION
    if collection ~= "" and #saved > 0 then
        local ReadCollection = require("readcollection")
        if not ReadCollection.coll[collection] then
            ReadCollection:addCollection(collection)
        end
        local added = 0
        for _, file in ipairs(saved) do
            if lfs.attributes(file, "mode") == "file"
                    and not ReadCollection:isFileInCollection(file, collection) then
                ReadCollection:addItem(file, collection)
                added = added + 1
            end
        end
        ReadCollection:write({ [collection] = true })
        logger.info("KoboWebBrowser: added", added, "file(s) to collection", collection)
    end

    if sleep_req then
        -- onResume reopens the browser on the same page.
        self.resume_launch = sleep_req
        logger.info("KoboWebBrowser: sleeping; will relaunch on", sleep_req.url)
        -- The browser's idle timer asked for sleep: sleep the KOReader way (screensaver,
        -- Wi-Fi off, suspend). For "sleep", KOReader is already on its way there.
        if reason == "exit" then
            UIManager:scheduleIn(0.1, function()
                self:watchForWake()
                UIManager:suspend()
            end)
        end
    end

    if h.read and not to_open and reason ~= "escape" then
        local r = h.read
        if lfs.attributes(r.file, "mode") == "file" then
            r.file = ffiutil.realpath(r.file) or r.file
            reading = r
            logger.info("KoboWebBrowser: reader mode:", r.file, "back to", r.url, "y", r.y)
            UIManager:scheduleIn(0.8, function()
                require("apps/reader/readerui"):showReader(r.file)
            end)
        else
            UIManager:show(InfoMessage:new{ text = T(_("Reader mode file not found:\n%1"), r.file) })
        end
    end

    if to_open then
        if lfs.attributes(to_open, "mode") == "file" then
            -- Let the repaint above settle, then open (showReader closes the current FM/Reader itself).
            UIManager:scheduleIn(0.8, function()
                require("apps/reader/readerui"):showReader(to_open)
            end)
        else
            UIManager:show(InfoMessage:new{ text = T(_("Saved file not found:\n%1"), to_open) })
        end
    end
end

-- ---- Reader mode: going back to the browser when the temporary book is closed

local function isReadingCopy(ui)
    local file = ui and ui.document and ui.document.file
    return reading ~= nil and file ~= nil and (ffiutil.realpath(file) or file) == reading.file
end

-- Another book is about to replace ours (History, Open last, a link): no way back then.
function KoboWebBrowser:onShowingReader()
    if isReadingCopy(self.ui) then reading.switching = true end
end

function KoboWebBrowser:onCloseDocument()
    if not isReadingCopy(self.ui) then return end
    local r = reading
    reading = nil
    -- After ReaderUI has finished closing (the file is released, the file manager is up).
    UIManager:scheduleIn(0.7, function() self:returnFromReader(r) end)
end

function KoboWebBrowser:returnFromReader(r)
    self:deleteReaderCopy(r.file)
    local ReaderUI = require("apps/reader/readerui")
    if r.switching or ReaderUI.instance then
        logger.info("KoboWebBrowser: reader mode: another book was opened, not going back")
        return
    end
    -- The file manager would otherwise sit in our temp folder once the browser exits.
    local FileManager = require("apps/filemanager/filemanager")
    local fm = FileManager.instance
    pcall(function()
        local home = G_reader_settings:readSetting("home_dir")
        if fm and home then fm.file_chooser:changeToPath(home) end
    end)
    -- This instance belonged to the closed ReaderUI: start the browser from the file manager's.
    local plugin = fm and fm.kobowebbrowser or self
    if active and active.job then return end
    -- Ask first: going back is the usual case, but the reader may be done with the web.
    local ConfirmBox = require("ui/widget/confirmbox")
    UIManager:show(ConfirmBox:new{
        text = _("Go back to the web page in the browser?"),
        ok_text = _("Back to the browser"),
        cancel_text = _("Stay in KOReader"),
        ok_callback = function()
            if active and active.job then return end
            local extra = { args = {}, env = {} }
            if r.url and r.url:match("^https?://") then extra.args[1] = r.url end
            if tonumber(r.y) then extra.env.KB_RESTORE_Y = r.y end
            if tonumber(r.zoom) then extra.env.KB_RESTORE_ZOOM = r.zoom end
            logger.info("KoboWebBrowser: reader mode: back to", r.url, "y", r.y)
            plugin:launch(extra)
        end,
        cancel_callback = function()
            logger.info("KoboWebBrowser: reader mode: staying in KOReader")
        end,
    })
end

-- Delete a reader-mode copy and everything KOReader keeps about it (book settings,
-- history entry, cover cache), so it leaves no trace in History or "Open last".
function KoboWebBrowser:deleteReaderCopy(file)
    file = ffiutil.realpath(file) or file   -- as History stores it; nil once deleted
    -- Where its .sdr is (needs the file itself when book settings are kept by hash).
    local sdr
    pcall(function() sdr = require("docsettings"):getSidecarDir(file) end)
    pcall(function() require("docsettings").updateLocation(file) end)
    os.remove(file)
    pcall(function() require("ui/widget/booklist").resetBookInfoCache(file) end)
    pcall(function()
        local ReadHistory = require("readhistory")
        ReadHistory:removeItemByPath(file)
        ReadHistory:ensureLastFile()
    end)
    -- KOReader can still write the book settings after CloseDocument (the reader saves
    -- them again as it closes), which brings the .sdr back: remove it now and once more
    -- a moment later.
    if sdr and sdr:match("%.sdr$") then
        local function purge()
            if lfs.attributes(sdr, "mode") == "directory" then ffiutil.purgeDir(sdr) end
        end
        purge()
        UIManager:scheduleIn(3, purge)
    end
end

function KoboWebBrowser:clearReaderDir()
    if lfs.attributes(self.reader_dir, "mode") ~= "directory" then return end
    for name in lfs.dir(self.reader_dir) do
        local path = self.reader_dir .. "/" .. name
        if name:match("%.epub$") or name:match("%.cbz$") then
            self:deleteReaderCopy(path)
        elseif name:match("%.sdr$") and lfs.attributes(path, "mode") == "directory" then
            ffiutil.purgeDir(path)   -- book settings left from an earlier copy
        end
    end
end

function KoboWebBrowser:showCover()
    if self.cover then return end
    self.cover = WhiteCover:new{}
    UIManager:show(self.cover)
end

function KoboWebBrowser:closeCover()
    if not self.cover then return end
    UIManager:close(self.cover)
    self.cover = nil
end

function KoboWebBrowser:deleteSleepFiles()
    os.remove(self.sleep_shot)
    os.remove(self.sleep_page)
end

-- From a sleep request until onResume: note power key / sleep cover events (the user
-- waking the device) and the kernel's suspend time, so onResume can tell a real wake from
-- a bounced suspend. Independent of the browser hooks, which are gone by then.
function KoboWebBrowser:watchForWake()
    self:unwatchWake()
    self.bounce_retried = false
    self.user_wake_at = nil
    self.suspend_event_at = nil
    self.sleep_requested_at = time.boottime_or_realtime_coarse()
    self.suspend_time_at_request = Device.total_suspend_time
    local handlers = UIManager.event_handlers
    local saved, wrappers = {}, {}
    for _, name in ipairs(WAKE_EVENTS) do
        local orig = handlers[name]
        saved[name] = orig
        wrappers[name] = function(...)
            -- Before the original: KOReader's handler may broadcast Resume right away.
            self.user_wake_at = time.boottime_or_realtime_coarse()
            if orig then return orig(...) end
        end
        handlers[name] = wrappers[name]
    end
    self.wake_watch = { saved = saved, wrappers = wrappers }
end

function KoboWebBrowser:unwatchWake()
    local w = self.wake_watch
    if not w then return end
    self.wake_watch = nil
    local handlers = UIManager.event_handlers
    for _, name in ipairs(WAKE_EVENTS) do
        -- Only if nobody replaced our wrapper in the meantime.
        if handlers[name] == w.wrappers[name] then handlers[name] = w.saved[name] end
    end
end

-- KOReader's Suspend broadcast (just before the screensaver and the kernel suspend).
function KoboWebBrowser:onSuspend()
    self.suspend_event_at = time.boottime_or_realtime_coarse()
end

-- Is this resume a bounced suspend (the kernel refused or woke straight back up, without
-- the user)? Logs the decision.
function KoboWebBrowser:isBouncedResume()
    if not self.wake_watch then return false end
    local now = time.boottime_or_realtime_coarse()
    local since = self.suspend_event_at and time.to_s(now - self.suspend_event_at)
    local slept
    if self.suspend_time_at_request and Device.total_suspend_time then
        slept = time.to_s(Device.total_suspend_time - self.suspend_time_at_request)
    end
    local user = self.user_wake_at ~= nil
    local bounced = not self.bounce_retried and not user
        and since ~= nil and since < BOUNCE_SEC
        and not (slept and slept > KERNEL_SLEPT_SEC)
    logger.info("KoboWebBrowser: resume:", since and string.format("%.2f s", since) or "no", "after Suspend,",
        "user wake:", user, "kernel slept:", slept and string.format("%.2f s", slept) or "unknown",
        "retried:", self.bounce_retried == true, "->", bounced and "bounce, suspending again" or "real wake")
    return bounced
end

-- Woken up after a sleep that the browser handed to us: show the wake screen (the
-- browser's last screen with the Wi-Fi progress) until KOReader's Wi-Fi restore is
-- connected, then reopen the browser on the same page. Exit leaves the browser, Cancel
-- opens the copy of the page saved at sleep time without waiting for the connection.
function KoboWebBrowser:onResume()
    local req = self.resume_launch
    if not req or self.job or self.wake then
        self:unwatchWake()
        return
    end
    local bounced = self:isBouncedResume()
    self:unwatchWake()
    if bounced then
        -- The white cover stays up, so the gap until the next suspend is white.
        self.bounce_retried = true
        UIManager:scheduleIn(1, function() UIManager:suspend() end)
        return
    end
    self.resume_launch = nil
    self:showCover()
    local NetworkMgr = require("ui/network/manager")
    local extra = { args = {}, env = {} }
    if req.url and req.url:match("^https?://") then extra.args[1] = req.url end
    if tonumber(req.y) then extra.env.KB_RESTORE_Y = req.y end
    if tonumber(req.zoom) then extra.env.KB_RESTORE_ZOOM = req.zoom end

    local wake
    local started = os.time()
    local tick
    local function finish()
        UIManager:unschedule(tick)
        if self.wake == wake then self.wake = nil end
        UIManager:close(wake)
    end
    wake = WakeScreen:new{
        image_file = self.sleep_shot,
        on_exit = function()
            logger.info("KoboWebBrowser: wake screen: Exit")
            finish()
            self:closeCover()
            self:deleteSleepFiles()
            UIManager:setDirty("all", "full")
        end,
        on_cancel = function()
            logger.info("KoboWebBrowser: wake screen: Offline copy, opening the saved page")
            if lfs.attributes(self.sleep_page, "mode") == "file" then
                extra.env.KB_RESTORE_FILE = self.sleep_page
            else
                logger.warn("KoboWebBrowser: no saved page at", self.sleep_page, "- reloading online")
            end
            -- Keep this screen up (no white flash) until the browser has the page drawn:
            -- it skips its start-up clear and draws nothing before the page has loaded.
            extra.env.KB_NO_CLEAR_AT_START = "1"
            extra.env.KB_HOLD_UNTIL_LOADED = "1"
            UIManager:unschedule(tick)
            wake:setStatus(_("Loading the offline copy…"), 100)
            UIManager:forceRePaint()
            -- The page file is read at start-up; it's deleted when this session ends.
            self:run(extra)
            -- The draw guard is up now: closing this screen paints nothing over the browser.
            finish()
        end,
    }
    self.wake = wake
    UIManager:show(wake, "full")
    -- Flash it again once it's all drawn (the first refresh can go out while the
    -- screensaver is still on the panel and leave it ghosting through).
    UIManager:scheduleIn(0.5, function()
        if self.wake == wake then UIManager:setDirty(wake, "full") end
    end)

    local asked_wifi = false
    tick = function()
        if self.wake ~= wake then return end
        local connected, wifi_on = false, false
        pcall(function() connected = NetworkMgr:isConnected() end)
        pcall(function() wifi_on = NetworkMgr:isWifiOn() end)
        local elapsed = os.time() - started
        if connected then
            logger.info("KoboWebBrowser: connected after", elapsed, "s, relaunching")
            wake:setStatus(_("Connected. Opening the page…"), 1)
            UIManager:forceRePaint()
            finish()
            self:run(extra)
            -- Reloading online: the saved copy isn't needed once the browser is up.
            if self.job then self:deleteSleepFiles() end
            return
        end
        -- KOReader restores Wi-Fi by itself when it was on (auto_restore_wifi); if it
        -- hasn't started after a few seconds, turn it on ourselves.
        if not wifi_on and not asked_wifi and elapsed >= 3 then
            asked_wifi = true
            pcall(function() NetworkMgr:turnOnWifi(nil, false) end)
        end
        if elapsed >= WAKE_TIMEOUT_SEC then
            wake:setStatus(_("Wi-Fi didn't connect. Exit, or Cancel to read the saved copy."), 1)
            return
        end
        wake:setStatus(wifi_on and _("Connecting to the network…") or _("Turning on Wi-Fi…"),
            math.min(0.95, elapsed / WAKE_TIMEOUT_SEC))
        UIManager:scheduleIn(WAKE_POLL_SEC, tick)
    end
    UIManager:scheduleIn(WAKE_POLL_SEC, tick)
end

return KoboWebBrowser

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

Text entry uses KOReader's own keyboard (setting "Use KOReader keyboard", default on): the
browser writes bin/kbd-request.txt (line 1: url|field|text, line 2: label, rest: current
text) and releases the touchscreen. We notice it within POLL_SEC, lift the draw guard, show
an InputDialog over the browser's frozen picture and answer in bin/kbd-reply.txt (line 1:
go|set|cancel, rest: text); the browser then takes the screen back and repaints itself.

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
local T = ffiutil.template
local Blitbuffer = require("ffi/blitbuffer")
local Button = require("ui/widget/button")
local CenterContainer = require("ui/widget/container/centercontainer")
local Font = require("ui/font")
local FrameContainer = require("ui/widget/container/framecontainer")
local Geom = require("ui/geometry")
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
-- A resume this soon after a sleep request is a bounced suspend: suspend again (once).
local BOUNCE_SEC = 8
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
            text = _("Exit"),
            width = btn_w,
            callback = function() if self.on_exit then self.on_exit() end end,
        },
        HorizontalSpan:new{ width = Size.span.horizontal_default },
        Button:new{
            text = _("Cancel"),
            width = btn_w,
            callback = function() if self.on_cancel then self.on_cancel() end end,
        },
    }
    local hint = TextBoxWidget:new{
        text = _("Exit closes the browser. Cancel opens the saved copy of this page without a connection."),
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
    table.insert(layers, CenterContainer:new{
        dimen = Geom:new{ x = 0, y = sh - box_size.h - Screen:scaleBySize(40), w = sw, h = box_size.h },
        self.box,
    })
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
    self.sleep_shot = self.bin .. "/sleep-shot.png"
    self.sleep_page = self.bin .. "/sleep-page.html"
    self.settings = G_reader_settings:readSetting("kobo_webbrowser") or {}
    self:onDispatcherRegisterActions()
    self.ui.menu:registerToMainMenu(self)
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
        sub_item_table = {
            {
                text = _("Open web browser"),
                callback = function() self:onLaunchKoboBrowser() end,
            },
            {
                text = _("Open address…"),
                keep_menu_open = true,
                callback = function() self:askUrl() end,
            },
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
                text = _("Use KOReader keyboard"),
                help_text = _("Type addresses and form fields with KOReader's keyboard instead of the browser's own. Takes effect the next time the browser starts."),
                checked_func = function() return self.settings.native_keyboard ~= false end,
                callback = function()
                    self.settings.native_keyboard = (self.settings.native_keyboard == false) and nil or false
                    self:saveSettings()
                end,
                separator = true,
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
            text = T(_("The browser program is missing.\n\nExpected: %1"), self.bin .. "/kobo-browser"),
        })
        return
    end
    -- A fresh start: drop anything left over from an earlier sleep/wake.
    self.resume_launch = nil
    self:deleteSleepFiles()
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
    }
    if self.settings.home then env.KB_HOME = self.settings.home end
    os.remove(self.kbd_request); os.remove(self.kbd_reply)
    if self.settings.native_keyboard ~= false then
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
        " env KOREADER_DIR=" .. shellQuote(DataStorage:getDataDir()),
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
        if not self.kbd_dialog and lfs.attributes(self.kbd_request, "mode") == "file" then
            local ok, err = pcall(self.onKeyboardRequest, self)
            if not ok then logger.warn("KoboWebBrowser: keyboard request failed:", err) end
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
    }
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
    dialog = InputDialog:new{
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
    self.bounce_retried = false
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
        self.sleep_requested_at = os.time()
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
        screen:refreshFull()
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
    if active == self then active = nil end
    UIManager:unschedule(self.poll_action)
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
    if reason == "escape" or h.to_open then sleep_req = nil end
    local sleeping = reason == "sleep" or (reason == "exit" and sleep_req ~= nil)

    if sleeping then
        -- KOReader's screensaver goes up next (over the white cover): mark everything for
        -- repaint under it, but don't flash KOReader's UI first.
        UIManager:setDirty("all")
    else
        -- The session is over: no white cover, no temp files.
        self:closeCover()
        self:deleteSleepFiles()
        -- The browser drew over everything. Repaint KOReader's UI first, then flash the
        -- whole screen (flashing before repainting would just re-show the browser's last
        -- image), and do it again a moment later in case the browser left a refresh in
        -- flight or the e-ink controller merged ours into it.
        local Screen = Device.screen
        UIManager:setDirty("all", "full")
        UIManager:forceRePaint()
        pcall(function() Screen:refreshFull() end)
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
            self.bounce_retried = false
            UIManager:scheduleIn(0.1, function()
                self.sleep_requested_at = os.time()
                UIManager:suspend()
            end)
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

-- Woken up after a sleep that the browser handed to us: show the wake screen (the
-- browser's last screen with the Wi-Fi progress) until KOReader's Wi-Fi restore is
-- connected, then reopen the browser on the same page. Exit leaves the browser, Cancel
-- opens the copy of the page saved at sleep time without waiting for the connection.
function KoboWebBrowser:onResume()
    local req = self.resume_launch
    if not req or self.job or self.wake then return end
    local since = self.sleep_requested_at and os.time() - self.sleep_requested_at
    if not self.bounce_retried and since and since <= BOUNCE_SEC then
        -- The white cover stays up, so the gap until the next suspend is white.
        logger.info("KoboWebBrowser: resume", since, "s after the sleep request, suspend bounced; suspending again")
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
            logger.info("KoboWebBrowser: wake screen: Cancel, opening the saved copy")
            finish()
            if lfs.attributes(self.sleep_page, "mode") == "file" then
                extra.env.KB_RESTORE_FILE = self.sleep_page
            else
                logger.warn("KoboWebBrowser: no saved page at", self.sleep_page, "- reloading online")
            end
            -- The page file is read at start-up; it's deleted when this session ends.
            self:run(extra)
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

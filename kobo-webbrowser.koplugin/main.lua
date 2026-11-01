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

local LOG_MAX_BYTES = 256 * 1024
local DEFAULT_HOME = "https://lite.duckduckgo.com/lite/"
local DEFAULT_COLLECTION = "Web"

local function shellQuote(s)
    return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

pcall(ffi.cdef, "int kill(int pid, int sig);")   -- may already be declared
local SIGTERM, SIGKILL = 15, 9
local POLL_SEC = 1           -- how often we check that the browser is still running
local STOP_WAIT_SEC = 1.5    -- SIGTERM grace before SIGKILL when going to sleep
-- Power held this long while the browser runs: kill it without sleeping. Must stay under
-- the 2 s after which KOReader's PowerPress handler powers the device off.
local ESCAPE_HOLD_SEC = 1.5

-- The UIManager functions everything KOReader draws goes through; they do nothing while
-- the browser owns the screen.
local DRAW_FUNCS = { "_repaint", "_refresh", "setDirty" }
local function nop() end

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
    self.bin = self.path .. "/bin"
    self.script = self.bin .. "/run.sh"
    self.handoff = self.bin .. "/epub-handoff.txt"
    self.log_file = self.bin .. "/launcher.log"
    self.rc_file = self.bin .. "/exit-code"
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
    }
    if self.settings.home then env.KB_HOME = self.settings.home end
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
    if alive(self.job.pid) then
        -- Counts as user activity, so AutoSuspend doesn't sleep under the browser.
        UIManager:broadcastEvent(Event:new("InputEvent"))
        UIManager:scheduleIn(POLL_SEC, self.poll_action)
    else
        self:onBrowserGone("exit")
    end
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

-- Draw guard, sleep hook and power-hold escape, active while the browser runs.
function KoboWebBrowser:installHooks()
    local saved = {}
    for _, name in ipairs(DRAW_FUNCS) do
        saved[name] = rawget(UIManager, name)
        UIManager[name] = nop
    end

    -- Power button / sleep cover / AutoSuspend: stop the browser (it saves its page),
    -- then let KOReader do its normal sleep (screensaver, Wi-Fi off, suspend).
    saved.onPowerEvent = rawget(Device, "onPowerEvent")
    local orig_power = Device.onPowerEvent
    Device.onPowerEvent = function(dev, ev)
        if (ev == "Power" or ev == "Suspend") and not dev.screen_saver_mode and self:isRunning() then
            logger.info("KoboWebBrowser: sleep requested, stopping the browser")
            UIManager:unschedule(self.escape_action)
            self:stopBrowser(SIGTERM, STOP_WAIT_SEC)
            self:onBrowserGone("sleep")
        end
        return orig_power(dev, ev)
    end

    -- Long power hold: kill a frozen browser without sleeping. KOReader's PowerPress
    -- handler (2 s hold = power off) runs unchanged.
    local handlers = UIManager.event_handlers
    saved.PowerPress, saved.PowerRelease = handlers.PowerPress, handlers.PowerRelease
    self.escape_action = self.escape_action or function()
        if not self:isRunning() then return end
        logger.warn("KoboWebBrowser: power held, killing the browser")
        self.escaped_hold = true
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

function KoboWebBrowser:removeHooks()
    local saved = self.hooks
    if not saved then return end
    self.hooks = nil
    for _, name in ipairs(DRAW_FUNCS) do
        UIManager[name] = saved[name]
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
        -- KOReader's screensaver goes up next: mark everything for repaint under it, but
        -- don't flash KOReader's UI first.
        UIManager:setDirty("all")
    else
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
            UIManager:scheduleIn(0.1, function() UIManager:suspend() end)
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

-- Woken up after a sleep that the browser handed to us: reopen it where it was. Wait a
-- little for KOReader's own Wi-Fi restore (auto_restore_wifi) so the page can load.
function KoboWebBrowser:onResume()
    local req = self.resume_launch
    if not req or self.job then return end
    self.resume_launch = nil
    local NetworkMgr = require("ui/network/manager")
    local extra = { args = {}, env = {} }
    if req.url and req.url:match("^https?://") then extra.args[1] = req.url end
    if tonumber(req.y) then extra.env.KB_RESTORE_Y = req.y end
    if tonumber(req.zoom) then extra.env.KB_RESTORE_ZOOM = req.zoom end
    local tries = 0
    local function launch()
        tries = tries + 1
        local connected = false
        pcall(function() connected = NetworkMgr:isConnected() end)
        if connected or tries > 15 then
            logger.info("KoboWebBrowser: relaunching after wake, connected:", connected)
            self:run(extra)
        else
            UIManager:scheduleIn(1, launch)
        end
    end
    UIManager:scheduleIn(1, launch)
end

return KoboWebBrowser

--[[--
Kobo web browser: a full-screen Qt WebKit browser for Kobo e-readers, started from KOReader.

The browser (bin/kobo-browser) uses the Qt 5.2.1 and QtWebKit libraries that ship in the
Kobo firmware. KOReader is single-threaded, so while the blocking os.execute() runs it
neither draws nor reads input. Before starting the browser we close KOReader's
/dev/input/event* handles so the browser gets the touchscreen and buttons to itself;
afterwards we reopen them, reset the gesture state and repaint fully.

The browser talks back through a handoff file (bin/epub-handoff.txt), one request per line:
  saved<TAB>/path/book.epub              add the saved article to a KOReader collection
  open<TAB>/path/book.epub               open it in the reader now
  sleep<TAB>url<TAB>scrollY<TAB>zoom     power button pressed: sleep the KOReader way
                                         (screensaver, Wi-Fi off), reopen the page on wake

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
local T = require("ffi/util").template

local LOG_MAX_BYTES = 256 * 1024
local DEFAULT_HOME = "https://lite.duckduckgo.com/lite/"
local DEFAULT_COLLECTION = "Web"

local function shellQuote(s)
    return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

-- os.execute returns a status number on Lua 5.1, or (ok, "exit", code) with 5.2 compat.
local function exitCode(a, b, c)
    if type(a) == "number" then
        if a > 255 then return math.floor(a / 256) end
        return a
    end
    if b == "exit" or b == "signal" then return c or 0 end
    return a and 0 or 1
end

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
    if lfs.attributes(self.bin .. "/kobo-browser", "mode") ~= "file" then
        UIManager:show(InfoMessage:new{
            text = T(_("The browser program is missing.\n\nExpected: %1"), self.bin .. "/kobo-browser"),
        })
        return
    end
    -- Let the menu close and the screen settle before the browser takes over.
    UIManager:scheduleIn(0.3, function() self:run(extra) end)
end

-- Close KOReader's evdev handles; returns what's needed to reopen them.
function KoboWebBrowser:releaseInput()
    local input = Device.input
    local closed = {}
    local ntx_path
    for path, fd in pairs(input.opened_devices or {}) do
        -- fake_events is KOReader's own USB/charger uevent pipe, not a device.
        if type(path) == "string" and path:match("^/dev/input/") then
            closed[#closed + 1] = path
            if Device.ntx_fd and fd == Device.ntx_fd then ntx_path = path end
        end
    end
    for _, path in ipairs(closed) do
        local ok, err = pcall(input.close, input, path)
        if not ok then logger.warn("KoboWebBrowser: closing", path, "failed:", err) end
    end
    logger.info("KoboWebBrowser: released input devices:", table.concat(closed, " "))
    return closed, ntx_path
end

function KoboWebBrowser:restoreInput(closed, ntx_path)
    local input = Device.input
    for _, path in ipairs(closed) do
        local ok, fd = pcall(input.open, input, path)
        if not ok then
            logger.warn("KoboWebBrowser: reopening", path, "failed:", fd)
        elseif path == ntx_path and fd then
            -- Device keeps this fd for key-repeat ioctls (sleep cover, page buttons).
            Device.ntx_fd = fd
        end
    end
    pcall(input.resetState, input)
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

    -- KB_KO_SLEEP=1: the browser hands sleep back to us (handoff "sleep" line), so the
    -- power button shows KOReader's screensaver. KB_AUTOSLEEP_MIN follows KOReader's
    -- auto-suspend setting.
    local env = {
        KB_KO_SLEEP = "1",
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
    local cmd = table.concat({
        "{",
        " echo; echo \"=== $(date '+%F %T') start\";",
        " cd " .. shellQuote(self.bin) .. " &&",
        " env KOREADER_DIR=" .. shellQuote(DataStorage:getDataDir()),
        " SCREEN_W=" .. Screen:getWidth() .. " SCREEN_H=" .. Screen:getHeight(),
        table.concat(env_parts),
        " sh " .. shellQuote(self.script) .. table.concat(arg_parts) .. ";",
        " rc=$?; echo \"=== $(date '+%F %T') exit code $rc\"; exit $rc;",
        " } >>" .. shellQuote(self.log_file) .. " 2>&1 </dev/null",
    })

    local closed, ntx_path = self:releaseInput()
    logger.info("KoboWebBrowser: running", self.script)
    local ok, a, b, c = pcall(os.execute, cmd)
    -- Whatever happened, give KOReader its input and screen back.
    self:restoreInput(closed, ntx_path)
    local code = ok and exitCode(a, b, c) or -1
    logger.info("KoboWebBrowser: browser returned", code, ok and "" or a)

    -- The browser drew over everything. Repaint KOReader's UI first, then flash the
    -- whole screen (flashing before repainting would just re-show the browser's last
    -- image), and do it again a moment later in case the browser left a refresh in flight
    -- or the e-ink controller merged ours into it.
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

    if code ~= 0 then
        UIManager:show(InfoMessage:new{
            text = T(_("The web browser stopped with an error (code %1).\n\n%2"), code, lastLines(self.log_file, 8)),
            monospace_font = true,
        })
    end
    local ok_h, err = pcall(self.processHandoff, self)
    if not ok_h then logger.warn("KoboWebBrowser: handoff failed:", err) end
end

function KoboWebBrowser:processHandoff()
    local f = io.open(self.handoff, "r")
    if not f then return end
    local saved, to_open, sleep_req = {}, nil, nil
    for line in f:lines() do
        local kind, rest = line:match("^(%a+)\t(.+)$")
        if kind == "saved" then
            saved[#saved + 1] = rest
        elseif kind == "open" then
            to_open = rest
        elseif kind == "sleep" then
            local url, y, zoom = rest:match("^([^\t]*)\t?([^\t]*)\t?([^\t]*)$")
            sleep_req = { url = url, y = y, zoom = zoom }
        end
    end
    f:close()
    os.remove(self.handoff)
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

    if sleep_req and not to_open then
        -- Sleep the KOReader way (screensaver, Wi-Fi off, suspend); onResume reopens the
        -- browser on the same page.
        self.resume_launch = sleep_req
        logger.info("KoboWebBrowser: browser asked for sleep; will relaunch on", sleep_req.url)
        UIManager:scheduleIn(0.1, function() UIManager:suspend() end)
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
    if not req then return end
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

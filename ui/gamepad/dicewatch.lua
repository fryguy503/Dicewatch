-- SPDX-License-Identifier: MIT

local views = { { "ranked", "Ranked" }, { "log", "Roll log" }, { "both", "Both" } }
local duplicates = { { "first", "First roll" }, { "best", "Best roll" }, { "last", "Last roll" } }
local orders = { { "high", "Highest roll" }, { "low", "Lowest roll" } }
local ranges = {
  { "100", "0–100" }, { "500", "0–500" }, { "1000", "0–1000" }, { "5000", "0–5000" }, { "custom", "Custom" },
}
local timer_presets = { 0, 10, 30, 60 }
local page_size = 8

local panel = "round"
local draft, active_id, history_id, pending_start
local timer_edit, timer_text
local pages = { ranked = 1, log = 1 }
local snapshot, refresh_at = nil, 0

local function act(plugin, ...)
  refresh_at = 0
  return plugin.action(...)
end

local function tint(color, text)
  ui.TextColored(color[1], color[2], color[3], 1, text)
end

local function wrap_tint(color, text)
  ui.PushTextWrapPos(0)
  tint(color, text)
  ui.PopTextWrapPos()
end

local accent = { 0.95, 0.79, 0.48 }
local good = { 0.62, 0.87, 0.68 }
local warning = { 0.98, 0.62, 0.55 }

local function action_button(label, enabled)
  if not enabled then ui.BeginDisabled() end
  local pressed = ui.Button(label)
  if not enabled then ui.EndDisabled() end
  return enabled and pressed
end

local function basic_combo(id, current, choices, width)
  local preview = tostring(current or "")
  for _, choice in ipairs(choices) do
    if choice[1] == current then
      preview = choice[2]
      break
    end
  end
  ui.SetNextItemWidth(width)
  if ui.BeginCombo(id, preview) then
    for _, choice in ipairs(choices) do
      if ui.Selectable(choice[2], current == choice[1]) then current = choice[1] end
    end
    ui.EndCombo()
  end
  return current
end

local function time_text(seconds)
  seconds = math.max(0, seconds or 0)
  return string.format("%02d:%02d", seconds // 60, seconds % 60)
end

local function range_text(round)
  return string.format("%d–%d", round.low, round.high)
end

local function seconds_input(id, text, width)
  ui.SetNextItemWidth(width)
  return ui.InputText(id, text, 5, ui.InputTextFlags.CharsDecimal)
end

local function duration_of(text)
  local seconds = tonumber(text)
  if seconds and seconds == math.floor(seconds) and seconds >= 0 and seconds <= 3600 then
    return seconds
  end
end

local function open_form(kind, defaults)
  panel = kind
  draft = {
    label = "", range = "100", range_choice = "100",
    duplicate = defaults.duplicate or "first", order = defaults.order or "high",
    duration = tostring(defaults.duration or 0),
  }
end

local function toolbar(plugin, state)
  if ui.Button("New round") then open_form("new", state.defaults or {}) end
  ui.SameLine()
  if ui.Button("History") then panel = "history" end
  ui.SameLine()
  if ui.Button("Defaults") then open_form("defaults", state.defaults or {}) end
  ui.SameLine()
  if ui.Button(state.mini and "Expand" or "Mini") then
    act(plugin, "layout", state.view, not state.mini)
  end
end

local function round_picker(plugin, state, scale)
  local selected = state.current
  local rounds = state.rounds or {}
  local active = 0
  for _, round in ipairs(rounds) do
    if not round.closed then active = active + 1 end
  end
  if active == 0 and not selected then return end

  local count_text = string.format("%d open", active)
  ui.SetNextItemWidth(math.max(108 * scale, ui.GetContentRegionAvail() - ui.CalcTextSize(count_text) - 10 * scale))
  local preview = "Select a round"
  if selected then
    preview = range_text(selected) .. " · " .. (state.mini and selected.label or selected.status)
  end
  if ui.BeginCombo("##active-round", preview) then
    for _, round in ipairs(rounds) do
      if not round.closed or round.id == state.selected then
        local label = tostring(round.label or ""):gsub("#", "＃")
        local detail = round.closed and round.status or round.remaining >= 0 and time_text(round.remaining) or "Rolling"
        local entry = string.format("%s · %s · %s##round%d", range_text(round), label, detail, round.id)
        if ui.Selectable(entry, round.id == state.selected) and round.id ~= state.selected then
          act(plugin, "select", round.id)
        end
      end
    end
    ui.EndCombo()
  end
  ui.SameLine()
  ui.TextDisabled(count_text)
end

local function round_controls(plugin, round, scale)
  if round.closed then
    if type(round.summary) == "string" and round.summary ~= "" and ui.Button("Results") then
      panel = "results"
    end
    return
  end

  local preview = round.remaining >= 0 and ("Auto " .. time_text(round.remaining)) or "Auto end"
  ui.SetNextItemWidth(126 * scale)
  if ui.BeginCombo("##auto-end", preview) then
    for _, seconds in ipairs(timer_presets) do
      if ui.Selectable(seconds == 0 and "Off" or (seconds .. " seconds"), seconds == 0 and round.remaining < 0) then
        timer_edit = nil
        act(plugin, "timer", round.id, seconds)
      end
    end
    if ui.Selectable("Custom…") then
      timer_edit = round.id
      timer_text = tostring(round.remaining > 0 and round.remaining or 30)
    end
    ui.EndCombo()
  end
  ui.SameLine()
  if ui.Button("End") then
    timer_edit = nil
    act(plugin, "end", round.id)
  end

  if timer_edit == round.id then
    timer_text = seconds_input("##timer-seconds", timer_text, 76 * scale)
    ui.SameLine()
    local seconds = duration_of(timer_text)
    if action_button("Set seconds", seconds ~= nil) then
      timer_edit = nil
      act(plugin, "timer", round.id, seconds)
    end
    ui.SameLine()
    if ui.Button("Cancel") then timer_edit = nil end
  end
end

local function roll_table(key, entries, show_heading, scale)
  local ranked = key == "ranked"
  if show_heading then ui.Text(ranked and "Ranked" or "Roll log") end
  if #entries == 0 then
    ui.TextDisabled(ranked and "No eligible rolls yet" or "No rolls yet")
    return
  end

  local max_page = math.ceil(#entries / page_size)
  local page = math.min(pages[key], max_page)
  local flags = ui.TableFlags.RowBg | ui.TableFlags.BordersInnerH | ui.TableFlags.SizingStretchProp
  if ui.BeginTable("##" .. key, ranked and 3 or 4, flags) then
    ui.TableSetupColumn(ranked and "Rank" or "Time", ui.TableColumnFlags.WidthFixed, 44 * scale)
    ui.TableSetupColumn("Player", ui.TableColumnFlags.WidthStretch)
    ui.TableSetupColumn("Roll", ui.TableColumnFlags.WidthFixed, 43 * scale)
    if not ranked then ui.TableSetupColumn("Status", ui.TableColumnFlags.WidthFixed, 87 * scale) end
    ui.TableHeadersRow()
    for index = (page - 1) * page_size + 1, math.min(page * page_size, #entries) do
      local row = entries[index]
      ui.TableNextRow()
      ui.TableNextColumn()
      ui.Text(ranked and tostring(row.rank) or time_text(row.time))
      ui.TableNextColumn()
      ui.TextWrapped(row.player)
      ui.TableNextColumn()
      if ranked and row.rank == 1 then tint(accent, tostring(row.value)) else ui.Text(tostring(row.value)) end
      if not ranked then
        ui.TableNextColumn()
        if row.status == "Counted" then ui.Text(row.status) else ui.TextDisabled(row.status or "") end
      end
    end
    ui.EndTable()
  end
  if max_page > 1 then
    if action_button("Prev##" .. key, page > 1) then page = page - 1 end
    ui.SameLine()
    ui.TextDisabled(string.format("%d/%d", page, max_page))
    ui.SameLine()
    if action_button("Next##" .. key, page < max_page) then page = page + 1 end
  end
  pages[key] = page
end

local function current_round(plugin, state, scale)
  toolbar(plugin, state)
  round_picker(plugin, state, scale)
  local round = state.current
  if not round then
    ui.TextDisabled(state.ready and "No round selected" or "Roll tracking is unavailable")
    return
  end

  ui.Separator()
  if not state.mini then ui.TextWrapped(round.label) end
  if round.closed then ui.TextDisabled(round.status) else tint(good, round.status) end
  ui.SameLine()
  ui.TextDisabled(string.format("%d counted / %d rolls", round.counted, round.total))
  if (round.dropped or 0) > 0 then
    tint(warning, string.format("%d beyond the roll limit", round.dropped))
  end
  if round.leader and round.leader ~= "" then wrap_tint(accent, round.leader) end
  round_controls(plugin, round, scale)
  if state.mini then return end

  ui.Separator()
  local view = basic_combo("##view", state.view or "ranked", views, 118 * scale)
  if view ~= state.view then act(plugin, "layout", view, false) end
  local show_both = view == "both"
  if view == "ranked" or show_both then roll_table("ranked", round.ranked or {}, show_both, scale) end
  if view == "log" or show_both then roll_table("log", round.log or {}, show_both, scale) end
end

local function back(title)
  if ui.Button("Back") then
    pending_start = nil
    panel = "round"
  end
  ui.SameLine()
  ui.Text(title)
  ui.Separator()
end

local function rules_form(plugin, state, scale)
  local is_new = panel == "new"
  back(is_new and "New round" or "Round defaults")
  if panel == "round" then return end
  if is_new then
    ui.SetNextItemWidth(-1)
    draft.label = ui.InputTextWithHint("##label", "Item or round name (optional)", draft.label, 161)
    ui.TextDisabled("Roll range")
    local picked = basic_combo("##range-preset", draft.range_choice, ranges, 128 * scale)
    if picked ~= draft.range_choice then
      draft.range = picked == "custom" and ("0-" .. draft.range_choice) or picked
      draft.range_choice = picked
    end
    if picked == "custom" then
      ui.SameLine()
      ui.SetNextItemWidth(-1)
      draft.range = ui.InputTextWithHint("##range", "Max or min-max", draft.range, 16)
    end
  end
  draft.duplicate = basic_combo("Count per player##duplicates", draft.duplicate, duplicates, 131 * scale)
  draft.order = basic_combo("Winner##order", draft.order, orders, 131 * scale)
  draft.duration = seconds_input("Auto end (seconds, 0 off)##duration", draft.duration, 64 * scale)
  local seconds = duration_of(draft.duration)
  if seconds == nil then ui.TextDisabled("Enter 0 to 3600 seconds") end
  if is_new then
    if action_button("Start round", state.ready and seconds ~= nil and draft.range:find("%S") ~= nil) then
      pending_start = state.selected or 0
      act(plugin, "start", draft.range, draft.label, draft.duplicate, draft.order, seconds)
    end
  elseif action_button("Save defaults", state.ready and seconds ~= nil) then
    act(plugin, "defaults", draft.duplicate, draft.order, seconds)
    panel = "round"
  end
end

local function results_text(round, scale)
  if not round then return end
  ui.TextWrapped(round.label)
  if round.status then ui.TextDisabled(round.status) end
  if round.leader and round.leader ~= "" then wrap_tint(accent, round.leader) end
  local summary = round.summary
  if type(summary) == "string" and summary ~= "" then
    ui.InputTextMultiline("##summary", summary, #summary + 1, -1, 122 * scale,
      ui.InputTextFlags.ReadOnly | ui.InputTextFlags.AutoSelectAll)
  end
end

local function history(state, scale)
  back("Round history")
  if panel ~= "history" then return end
  local rounds = state.history or {}
  if #rounds == 0 then
    ui.TextDisabled("No finished rounds yet")
    return
  end
  local selected = rounds[1]
  for _, round in ipairs(rounds) do
    if round.id == history_id then selected = round break end
  end
  ui.SetNextItemWidth(-1)
  if ui.BeginCombo("##history-round", range_text(selected) .. " · " .. selected.label) then
    for _, round in ipairs(rounds) do
      local label = tostring(round.label or ""):gsub("#", "＃")
      local result = tostring(round.leader or round.status or ""):gsub("#", "＃")
      local entry = string.format("%s · %s · %s##history%d", range_text(round), label, result, round.id)
      if ui.Selectable(entry, selected.id == round.id) then
        history_id = round.id
        selected = round
      end
    end
    ui.EndCombo()
  end
  results_text(selected, scale)
end

ui.window("Dicewatch", function()
  local plugin = eq.Plugin("dicewatch")
  if not plugin then
    snapshot, refresh_at = nil, 0
    return
  end
  local now = ui.GetTime()
  if not snapshot or now >= refresh_at then
    snapshot = plugin.state()
    refresh_at = now + 0.1
  end
  local state = snapshot
  if type(state) ~= "table" or not state.open then
    panel = "round"
    timer_edit, pending_start = nil, nil
    return
  end
  local selected = state.selected or 0
  if active_id ~= selected then
    active_id, timer_edit = selected, nil
    pages.ranked, pages.log = 1, 1
  end
  if pending_start ~= nil and selected ~= pending_start then
    panel, pending_start = "round", nil
  end

  local io = ui.GetIO()
  local scale = math.max(0.65, io.uiScale or 1)
  local compact = state.mini and panel == "round"
  local width = (compact and 338 or 432) * scale
  ui.SetNextWindowSize(math.min(width, (io.width or 1280) - 24), 0, ui.Cond.Always)
  local shown, open = ui.Begin("Dicewatch", true, ui.WindowFlags.NoCollapse | ui.WindowFlags.AlwaysAutoResize)
  if not open then act(plugin, "hide") end
  if shown and open then
    if panel == "new" or panel == "defaults" then
      rules_form(plugin, state, scale)
    elseif panel == "history" then
      history(state, scale)
    elseif panel == "results" then
      back("Round results")
      if panel == "results" then results_text(state.current, scale) end
    else
      current_round(plugin, state, scale)
    end
    if type(state.message) == "string" and state.message ~= "" and (state.error or not compact) then
      if state.error then wrap_tint(warning, state.message) else ui.TextWrapped(state.message) end
    end
  end
  ui.End()
end)

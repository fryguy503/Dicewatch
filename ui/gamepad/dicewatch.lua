-- SPDX-License-Identifier: MIT

local views = { { "ranked", "Ranked" }, { "log", "Roll log" }, { "both", "Both" } }
local duplicates = { { "first", "First roll" }, { "best", "Best roll" }, { "last", "Last roll" } }
local orders = { { "high", "Highest roll" }, { "low", "Lowest roll" } }
local ranges = {
  { "100", "0–100" }, { "500", "0–500" }, { "1000", "0–1000" }, { "5000", "0–5000" }, { "custom", "Custom" },
}
local announcement_modes = { { "off", "Off" }, { "copy", "Copy" }, { "send", "Auto-send" } }
local announcement_channels = { { "raid", "Raid say" }, { "group", "Group" }, { "guild", "Guild" }, { "say", "Say" } }
local chat_commands = { raid = "/rs", group = "/g", guild = "/gu", say = "/say" }
local settings_tabs = { { "announce", "Announcements" }, { "defaults", "Round defaults" } }
local timer_presets = { 0, 10, 30, 60 }
local page_size = 8

local panel = "round"
local draft, active_id, pending_start
local timer_edit, timer_text
local pages = { ranked = 1, log = 1 }
local snapshot, refresh_at = nil, 0
local ledger = { filter = "", view = "ranked", pages = { ranked = 1, log = 1 }, refresh_at = 0 }
local discord = { page = 1 }
local settings_tab = "announce"
local announcement_draft, announcement_feedback
local award_feedback
local notice_view = {}

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

local function rgb(hex)
  return { (hex >> 16 & 255) / 255, (hex >> 8 & 255) / 255, (hex & 255) / 255 }
end

local background, title, control = rgb(0x1e1e23), rgb(0x27262d), rgb(0x2a2930)
local border, rule, hover = rgb(0x46424b), rgb(0x35333b), rgb(0x37353d)
local ink, muted, accent = rgb(0xf0ece6), rgb(0xb2a8b4), rgb(0xf3ce81)
local emphasis, amber, dark_ink = rgb(0x362e22), rgb(0xebc982), rgb(0x2c210f)
local good, warning = rgb(0xa1d9b2), rgb(0xf4a596)
local halo, amber_hover = rgb(0x493d2a), rgb(0xf5d899)
local col = ui.Col

local function color_list(entries)
  local list = {}
  for id, color in pairs(entries) do
    list[#list + 1] = id
    list[#list + 1] = color[1]
    list[#list + 1] = color[2]
    list[#list + 1] = color[3]
    list[#list + 1] = color[4] or 1
  end
  return list
end

local window_colors = color_list({
  [col.WindowBg] = background, [col.ChildBg] = background, [col.PopupBg] = title,
  [col.Text] = ink, [col.TextDisabled] = muted, [col.Border] = border, [col.Separator] = rule,
  [col.FrameBg] = control, [col.FrameBgHovered] = hover, [col.FrameBgActive] = emphasis,
  [col.Button] = control, [col.ButtonHovered] = hover, [col.ButtonActive] = emphasis,
  [col.Header] = emphasis, [col.HeaderHovered] = hover, [col.HeaderActive] = halo,
  [col.NavCursor] = accent,
})
local selected_colors = color_list({
  [col.Text] = accent, [col.Button] = emphasis, [col.ButtonHovered] = halo,
  [col.ButtonActive] = halo, [col.Border] = accent,
})
local primary_colors = color_list({
  [col.Text] = dark_ink, [col.Button] = amber, [col.ButtonHovered] = amber_hover,
  [col.ButtonActive] = accent, [col.Border] = amber,
})
local flat_colors = color_list({ [col.Button] = {0, 0, 0, 0}, [col.Border] = {0, 0, 0, 0} })

local function rectangle(x, y, width, height, shade, radius)
  ui.DrawRectFilled(x, y, x + width, y + height, shade[1], shade[2], shade[3], 1, radius or 0)
end

local function draw_text(x, y, shade, text)
  ui.DrawText(x, y, shade[1], shade[2], shade[3], 1, text)
end

local function clipped_text(x, y, width, line, shade, text)
  if width <= 0 then return end
  local dots = ui.CalcTextSize("…")
  if ui.CalcTextSize(text) <= width then
    draw_text(x, y, shade, text)
  elseif width > dots + 3 then
    ui.PushClipRect(x, y, x + width - dots, y + line, true)
    draw_text(x, y, shade, text)
    ui.PopClipRect()
    draw_text(x + width - dots, y, shade, "…")
  end
end

local function styled_button(label, width, height, colors)
  local count = ui.PushStyleColors(colors)
  local clicked = ui.Button(label, width, height)
  ui.PopStyleColor(count)
  return clicked
end

local function action_button(label, enabled, colors)
  if not enabled then ui.BeginDisabled() end
  local pressed
  if colors then pressed = styled_button(label, 0, 0, colors) else pressed = ui.Button(label) end
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
      local label = choice[2]:gsub("#", "＃") .. "##" .. choice[1]
      if ui.Selectable(label, current == choice[1]) then current = choice[1] end
    end
    ui.EndCombo()
  end
  return current
end

local function setting_buttons(id, current, choices, scale)
  local picked = current
  for index, choice in ipairs(choices) do
    if index > 1 then ui.SameLine(0, 4 * scale) end
    local label = choice[2] .. "##" .. id
    local width = ui.CalcTextSize(choice[2]) + 16 * scale
    local clicked
    if choice[1] == current then clicked = styled_button(label, width, 0, selected_colors)
    else clicked = ui.Button(label, width) end
    if clicked then picked = choice[1] end
  end
  return picked
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

local function round_draft(defaults)
  return {
    label = "", range = "100", range_choice = "100",
    duplicate = defaults.duplicate or "first", order = defaults.order or "high",
    duration = tostring(defaults.duration or 0),
  }
end

local function open_form(kind, defaults)
  panel, draft = kind, round_draft(defaults)
end

local function open_settings(state)
  local saved = state.announcements or {}
  panel, pending_start = "settings", nil
  draft = round_draft(state.defaults or {})
  announcement_draft = {
    start_mode = saved.start_mode or "off", start_channel = saved.start_channel or "raid",
    award_mode = saved.award_mode or "off", award_channel = saved.award_channel or "raid",
  }
  announcement_feedback = nil
end

local function open_ledger(key)
  panel, ledger.refresh_at, pending_start = "ledger", 0, nil
  if key then ledger.key, ledger.filter = key, "" end
end

local function round_picker(plugin, state, width)
  local selected = state.current
  ui.SetNextItemWidth(width)
  local preview = "Select a round"
  if selected then
    preview = range_text(selected) .. " · " .. selected.label
  end
  if ui.BeginCombo("##active-round", preview) then
    for _, round in ipairs(state.rounds or {}) do
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
end

local function title_bar(plugin, state, scale, compact)
  local width = ui.GetContentRegionAvail()
  local gap = 6 * scale
  local brand = 94 * scale
  local nav_width = compact and 96 * scale or 220 * scale
  local picker = width - brand - nav_width - 2 * gap
  local inline_picker = compact and picker >= 92 * scale
  local nav_on_row = width >= brand + nav_width + gap
  local x, y = ui.GetCursorScreenPos()
  local _, line = ui.CalcTextSize("Dicewatch")
  local height = math.max(29 * scale, line + 8 * scale)
  rectangle(x, y, width, height, title, 4 * scale)

  local die, icon_x, icon_y = 13 * scale, x + 7 * scale, y + (height - 13 * scale) / 2
  ui.DrawRect(icon_x, icon_y, icon_x + die, icon_y + die, accent[1], accent[2], accent[3], 1, 3 * scale, scale)
  for i = 1, 3 do
    rectangle(icon_x + (1 + 3 * i) * scale, icon_y + (10 - 3 * (i - 1)) * scale,
      1.5 * scale, 1.5 * scale, accent, scale)
  end
  draw_text(icon_x + die + 6 * scale, y + (height - line) / 2, ink, "Dicewatch")
  ui.Dummy(inline_picker and brand or (nav_on_row and width - nav_width - gap or width), height)
  if inline_picker then
    ui.SameLine(0, gap)
    round_picker(plugin, state, picker)
  end
  if nav_on_row then ui.SameLine(0, gap) end
  if not compact then
    if styled_button("Ledger", 62 * scale, height, flat_colors) then open_ledger() end
    ui.SameLine(0, gap)
    if styled_button("Settings", 69 * scale, height, flat_colors) then open_settings(state) end
    ui.SameLine(0, gap)
  end
  if ui.Button(compact and "Expand" or "Mini", (compact and 62 or 42) * scale, height) then
    act(plugin, "layout", state.view, not state.mini)
    panel = "round"
  end
  ui.SameLine(0, gap)
  if styled_button("×##close", 25 * scale, height, flat_colors) then act(plugin, "hide") end
  if ui.IsItemHovered() then ui.SetTooltip("Close Dicewatch") end
  if compact and not inline_picker then round_picker(plugin, state, width) end
  ui.Separator()
end

local function round_card(round, id, selected, width, height, scale, detail, tooltip_detail)
  local x, y = ui.GetCursorScreenPos()
  local pressed
  if selected then pressed = styled_button(id, width, height, selected_colors)
  else pressed = ui.Button(id, width, height) end
  local inset = 7 * scale
  local _, line = ui.CalcTextSize("Ag")
  if not detail then
    if round.closed then
      detail = round.status
    elseif round.remaining >= 0 then
      detail = time_text(round.remaining)
      if ui.CalcTextSize(detail .. " left") <= width - 2 * inset then detail = detail .. " left" end
    else
      detail = string.format("%d %s", round.total, round.total == 1 and "roll" or "rolls")
    end
  end
  clipped_text(x + inset, y + 5 * scale, width - 2 * inset, line, selected and accent or ink, range_text(round))
  clipped_text(x + inset, y + line + 7 * scale, width - 2 * inset, line, ink, round.label)
  clipped_text(x + inset, y + 2 * line + 8 * scale, width - 2 * inset, line, muted, detail)
  if ui.IsItemHovered() then
    local tooltip = round.label .. " · " .. range_text(round) .. " · " .. (tooltip_detail or detail)
    if round.date and round.date ~= "" then tooltip = tooltip .. " · " .. round.date end
    ui.SetTooltip(tooltip)
  end
  return pressed
end

local function card_layout(available, scale)
  local columns = available >= 416 * scale and 4 or available >= 208 * scale and 2 or 1
  local _, line = ui.CalcTextSize("Ag")
  return { columns = columns, height = 3 * line + 13 * scale, gap = 6 * scale, scale = scale }
end

local function card_grid(rounds, layout, first, last, draw)
  local available = ui.GetContentRegionAvail()
  local width = #rounds == 1 and math.min(available, 228 * layout.scale)
    or (available - (layout.columns - 1) * layout.gap) / layout.columns
  for index = first, last do
    if (index - first) % layout.columns ~= 0 then ui.SameLine(0, layout.gap) end
    draw(rounds[index], width, layout.height)
  end
end

local function round_cards(plugin, state, scale)
  local available = ui.GetContentRegionAvail()
  local left = ui.GetCursorPos()
  ui.TextDisabled("Loot rounds")
  local button_width = 69 * scale
  if available >= 159 * scale then ui.SameLine(left + available - button_width) end
  if styled_button("+ Round", button_width, 0, flat_colors) then open_form("new", state.defaults or {}) end

  local rounds = {}
  for _, round in ipairs(state.rounds or {}) do
    if not round.closed or round.id == state.selected then rounds[#rounds + 1] = round end
  end
  if #rounds == 0 then
    ui.TextDisabled("No open rounds")
    return
  end

  local layout = card_layout(available, scale)
  local scrolling = #rounds > layout.columns * 2
  if scrolling then ui.BeginChild("##round-cards", 0, 2 * layout.height + layout.gap, false) end
  card_grid(rounds, layout, 1, #rounds, function(round, width, height)
    local selected = round.id == state.selected
    if round_card(round, "##round" .. round.id, selected, width, height, scale) and not selected then
      act(plugin, "select", round.id)
    end
  end)
  if scrolling then ui.EndChild() end
end

local function round_controls(plugin, round, scale, compact)
  if round.closed then
    if compact and type(round.summary) == "string" and round.summary ~= "" and ui.Button("Results") then
      panel = "results"
    end
    return
  end

  local preview = round.remaining >= 0 and ("Auto " .. time_text(round.remaining)) or "Auto end"
  ui.SetNextItemWidth(111 * scale)
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
  ui.SameLine(0, 6 * scale)
  if styled_button("End", 43 * scale, 0, primary_colors) then
    timer_edit = nil
    act(plugin, "end", round.id)
  end
end

local function custom_timer(plugin, round, scale)
  if timer_edit == round.id and not round.closed then
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

local function save_award(plugin, receipt, player)
  local result = act(plugin, "award", receipt.key, player)
  local ok = type(result) == "table" and result.ok == true
  local message = type(result) == "table" and result.message or ""
  if type(message) ~= "string" or message == "" then
    message = ok and "Award saved." or "Could not save the award recipient."
  end
  award_feedback = { key = receipt.key, ok = ok, message = message }
  if ok then
    ledger.refresh_at = 0
    if ledger.award_key == receipt.key then ledger.award_draft = player end
  end
end

local function award_status(round)
  local recipient = round.recipient or ""
  ui.TextWrapped("Awarded to  " .. (recipient == "" and "Unawarded" or recipient))
  if award_feedback and award_feedback.key == round.key then
    wrap_tint(award_feedback.ok and muted or warning, award_feedback.message)
  end
end

local function roll_table(plugin, round, key, show_heading, scale, width, pagination)
  local ranked = key == "ranked"
  local ranked_entries = round.ranked or {}
  local entries = ranked and ranked_entries or (round.log or {})
  local leader_roll = ranked_entries[1] and ranked_entries[1].value
  local can_award = round.closed and type(round.key) == "string" and round.key ~= ""
  if show_heading then ui.Text(ranked and "Ranked" or "Roll log") end
  if #entries == 0 then
    ui.TextDisabled(ranked and "No eligible rolls yet" or "No rolls yet")
    if show_heading then ui.Dummy(width, 0) end
    return
  end

  local max_page = math.ceil(#entries / page_size)
  pagination = pagination or pages
  local page = math.min(pagination[key], max_page)
  local _, line = ui.CalcTextSize("Ag")
  local inset = 6 * scale
  local first_col = 47 * scale
  local roll_col = 45 * scale
  local status_col = ranked and 0 or math.min(width * 0.32, 87 * scale)
  local row_height = line + 8 * scale
  local function row(first, player, value, status, selected, heading)
    local x, y = ui.GetCursorScreenPos()
    local text_y = y + (row_height - line) / 2
    if selected then rectangle(x, y, width, row_height, emphasis) end
    local right = x + width - inset - status_col
    local value_width = ui.CalcTextSize(value)
    clipped_text(x + inset, text_y, first_col - inset, line, muted, first)
    clipped_text(x + first_col, text_y, width - first_col - roll_col - status_col - 2 * inset,
      line, heading and muted or ink, player)
    draw_text(right - value_width, text_y, heading and muted or (selected and accent or ink), value)
    if not ranked then clipped_text(right + inset * 2, text_y, status_col - inset, line, muted, status) end
    rectangle(x, y + row_height - 1, width, 1, rule)
    ui.Dummy(width, row_height - 4 * scale)
  end
  row(ranked and "Rank" or "Time", "Player", "Roll", "Status", false, true)
  for index = (page - 1) * page_size + 1, math.min(page * page_size, #entries) do
    local entry = entries[index]
    local leading = ranked and entry.rank == 1 or (entry.status == "Counted" and entry.value == leader_roll)
    row(ranked and tostring(entry.rank) or time_text(entry.time), entry.player, tostring(entry.value),
      entry.status or "", leading, false)
    if can_award and ui.BeginPopupContextItem("##award-" .. round.key .. "-" .. key .. "-" .. index) then
      ui.TextDisabled(entry.player)
      if ui.MenuItem("Award", false, entry.player ~= round.recipient) then save_award(plugin, round, entry.player) end
      ui.EndPopup()
    end
  end
  if max_page > 1 then
    if action_button("Prev##" .. key, page > 1) then page = page - 1 end
    ui.SameLine()
    ui.TextDisabled(string.format("%d/%d", page, max_page))
    ui.SameLine()
    if action_button("Next##" .. key, page < max_page) then page = page + 1 end
  end
  pagination[key] = page
end

local function round_heading(plugin, round, scale, compact)
  local width = ui.GetContentRegionAvail()
  local actions_width = (round.closed and 82 or 160) * scale
  local gap = 8 * scale
  local beside = width >= actions_width + (compact and 130 or 165) * scale and (not round.closed or compact)
  local info_width = beside and width - actions_width - gap or width
  local _, line = ui.CalcTextSize("Ag")
  local function info(shade, text)
    local x, y = ui.GetCursorScreenPos()
    clipped_text(x, y, info_width, line, shade, text)
    ui.Dummy(info_width, line)
  end
  ui.BeginGroup()
  info(round.closed and muted or good, "●  " .. round.status:upper())
  if compact then
    if round.leader and round.leader ~= "" then info(accent, round.leader) end
  else
    info(ink, round.label)
    local rule_text = range_text(round)
    if round.order == "high" then rule_text = rule_text .. " · High wins" end
    if round.order == "low" then rule_text = rule_text .. " · Low wins" end
    if round.linked then rule_text = rule_text .. " · Linked item" end
    info(muted, rule_text)
  end
  ui.EndGroup()
  if beside then ui.SameLine(0, gap) end
  round_controls(plugin, round, scale, compact)
  custom_timer(plugin, round, scale)
end

local function round_views(plugin, state, round, scale)
  local view = state.view or "ranked"
  local width = ui.GetContentRegionAvail()
  local excluded = round.total - round.counted
  local excluded_text = excluded > 0 and string.format("%d excluded", excluded) or ""
  local left = ui.GetCursorPos()
  for index, choice in ipairs(views) do
    if index > 1 then ui.SameLine(0, 4 * scale) end
    local button_width = ui.CalcTextSize(choice[2]) + 16 * scale
    local clicked
    if choice[1] == view then clicked = styled_button(choice[2], button_width, 0, selected_colors)
    else clicked = ui.Button(choice[2], button_width) end
    if clicked and choice[1] ~= view then act(plugin, "layout", choice[1], false) end
  end
  if excluded_text ~= "" then
    local text_width = ui.CalcTextSize(excluded_text)
    if width >= 175 * scale + text_width then ui.SameLine(left + width - text_width) end
    ui.TextDisabled(excluded_text)
  end

  if view == "ranked" then
    roll_table(plugin, round, "ranked", false, scale, width)
  elseif view == "log" then
    roll_table(plugin, round, "log", false, scale, width)
  else
    local beside = width >= 536 * scale
    local gap = 15 * scale
    local rank_width = beside and math.floor((width - gap) * 0.40) or width
    local log_width = beside and width - gap - rank_width or width
    ui.BeginGroup()
    roll_table(plugin, round, "ranked", true, scale, rank_width)
    ui.EndGroup()
    local x, y, _, bottom = ui.GetItemRect()
    if beside then ui.SameLine(0, gap) else ui.Spacing() end
    ui.BeginGroup()
    roll_table(plugin, round, "log", true, scale, log_width)
    ui.EndGroup()
    if beside then
      local _, _, _, other_bottom = ui.GetItemRect()
      rectangle(x + rank_width + gap / 2, y, 1, math.max(bottom, other_bottom) - y, rule)
    end
  end
end

local function round_footer(round, scale)
  ui.Separator()
  local available = ui.GetContentRegionAvail()
  local left = ui.GetCursorPos()
  local has_result = round.closed and type(round.summary) == "string" and round.summary ~= ""
  local button_width = 68 * scale
  local summary = round.closed and round.leader and round.leader ~= "" and round.leader
    or string.format("%d %s · %d %s", round.counted, round.counted == 1 and "player" or "players",
      round.total, round.total == 1 and "roll" or "rolls")
  if has_result and ui.CalcTextSize(summary) + button_width + 10 * scale <= available then
    ui.TextDisabled(summary)
    ui.SameLine(left + available - button_width)
  else
    wrap_tint(muted, summary)
  end
  if has_result and styled_button("Results", button_width, 0, flat_colors) then panel = "results" end
end

local function current_round(plugin, state, scale, compact)
  if not compact then
    round_cards(plugin, state, scale)
    ui.Separator()
  end
  local round = state.current
  if not round then
    ui.TextDisabled(state.ready and "No round selected" or "Roll tracking is unavailable")
    if compact and ui.Button("+ Round") then open_form("new", state.defaults or {}) end
    return
  end

  round_heading(plugin, round, scale, compact)
  if (round.dropped or 0) > 0 then
    tint(warning, string.format("%d beyond the roll limit", round.dropped))
  end
  if compact then return end

  ui.Separator()
  round_views(plugin, state, round, scale)
  round_footer(round, scale)
  if round.closed and round.key then award_status(round) end
end

local function back(title, destination)
  if ui.Button("Back") then
    pending_start = nil
    panel = destination or "round"
  end
  ui.SameLine()
  ui.Text(title)
  ui.Separator()
end

local function rules_form(plugin, state, scale)
  local is_new = panel == "new"
  if is_new then
    back("New round")
    if panel == "round" then return end
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
    if action_button("Start round", state.ready and seconds ~= nil and draft.range:find("%S") ~= nil, primary_colors) then
      pending_start = state.selected or 0
      act(plugin, "start", draft.range, draft.label, draft.duplicate, draft.order, seconds)
    end
  elseif action_button("Save defaults", state.ready and seconds ~= nil, primary_colors) then
    act(plugin, "defaults", draft.duplicate, draft.order, seconds)
    panel = "round"
  end
end

local function read_only_text(id, text, height)
  ui.InputTextMultiline(id, text, #text + 1, -1, height,
    ui.InputTextFlags.ReadOnly | ui.InputTextFlags.AutoSelectAll)
end

local function announcement_group(label, event, prefs, scale)
  local mode_key, channel_key = event .. "_mode", event .. "_channel"
  local width = ui.GetContentRegionAvail()
  ui.Text(label)
  prefs[mode_key] = setting_buttons(event .. "-mode", prefs[mode_key], announcement_modes, scale)
  local mode = prefs[mode_key]
  if mode == "send" then
    if width >= 332 * scale then ui.SameLine(0, 9 * scale) end
    prefs[channel_key] = basic_combo("Channel##" .. event, prefs[channel_key], announcement_channels, 105 * scale)
  end
  if mode == "off" then
    ui.TextDisabled("Announcements off")
    return
  end
  ui.TextDisabled(mode == "copy" and "Clipboard preview" or ("Chat preview · " .. (chat_commands[prefs[channel_key]] or "")))
  local preview = prefs[event .. "_preview"]
  if type(preview) == "string" and preview ~= "" then ui.TextWrapped(preview) end
end

local function settings_panel(plugin, state, scale)
  back("Settings")
  if panel ~= "settings" then return end
  settings_tab = setting_buttons("settings", settings_tab, settings_tabs, scale)
  ui.Separator()
  if settings_tab == "defaults" then
    rules_form(plugin, state, scale)
    return
  end

  local saved, prefs = state.announcements, announcement_draft
  if type(saved) ~= "table" then
    ui.TextDisabled("Announcement settings are unavailable.")
    return
  end
  prefs.start_preview, prefs.award_preview = saved.start_preview, saved.award_preview
  announcement_group("When a round opens", "start", prefs, scale)
  ui.Separator()
  announcement_group("When an award is saved", "award", prefs, scale)
  local sending = prefs.start_mode == "send" or prefs.award_mode == "send"
  local copying = prefs.start_mode == "copy" or prefs.award_mode == "copy"
  if sending and saved.chat_allowed == false then
    wrap_tint(warning, "Chat permission is unavailable here. Auto-send will keep the announcement available to copy.")
  elseif copying and saved.clipboard_available == false then
    ui.TextWrapped("Automatic clipboard access is unavailable here. You can select and copy the announcement text.")
  end

  ui.Separator()
  local changed = prefs.start_mode ~= saved.start_mode or prefs.start_channel ~= saved.start_channel or
    prefs.award_mode ~= saved.award_mode or prefs.award_channel ~= saved.award_channel
  if action_button("Save announcements", changed, primary_colors) then
    local result = act(plugin, "announcements", prefs.start_mode, prefs.start_channel, prefs.award_mode, prefs.award_channel)
    announcement_feedback = type(result) == "table" and result or { ok = false, message = "Could not save announcements." }
  end
  if announcement_feedback and type(announcement_feedback.message) == "string" and announcement_feedback.message ~= "" then
    wrap_tint(announcement_feedback.ok and muted or warning, announcement_feedback.message)
  end
end

local function announcement_notice(plugin, notice, scale, compact)
  if type(notice) ~= "table" or type(notice.message) ~= "string" or notice.message == "" then
    notice_view = {}
    return
  end
  local blocked = notice.kind == "blocked"
  if notice_view.revision ~= notice.revision or notice_view.message ~= notice.message then
    notice_view = { revision = notice.revision, message = notice.message, expanded = blocked and not compact }
  end
  ui.Separator()
  local event = notice.event == "award" and "Award saved" or "Round opened"
  local status = type(notice.status) == "string" and notice.status ~= "" and notice.status or "Announcement available"
  local label = notice_view.expanded and "Hide text" or "Show text"
  local line = event .. " · " .. status
  local shade = blocked and warning or (notice.kind == "sent" and good or accent)
  local button_width = ui.CalcTextSize(label) + ui.CalcTextSize("Dismiss") + 46 * scale
  local inline = ui.CalcTextSize(line) + button_width <= ui.GetContentRegionAvail()
  if inline then tint(shade, line) else wrap_tint(shade, line) end
  if inline then ui.SameLine() end
  if ui.Button(label .. "##announcement") then
    notice_view.expanded = not notice_view.expanded
  end
  ui.SameLine()
  if ui.Button("Dismiss##announcement") then
    act(plugin, "announcement_dismiss", notice.revision)
    return
  end
  if not notice_view.expanded then return end
  read_only_text("##announcement-text-" .. tostring(notice.revision), notice.message, 61 * scale)
  if ui.Button("Copy text##announcement") then
    local result = act(plugin, "announcement_copy", notice.revision)
    if type(result) == "table" and type(result.message) == "string" then
      notice_view.copy_feedback = result
    else
      notice_view.copy_feedback = { ok = false, message = "Could not copy automatically." }
    end
  end
  if notice_view.copy_feedback and notice_view.copy_feedback.message ~= "" then
    wrap_tint(notice_view.copy_feedback.ok and muted or warning, notice_view.copy_feedback.message)
  end
  ui.TextWrapped("Click the text, then press Ctrl+A and Ctrl+C to copy.")
end

local function results_text(round, scale)
  if not round then return end
  ui.TextWrapped(round.label)
  if round.status then ui.TextDisabled(round.status) end
  if round.leader and round.leader ~= "" then wrap_tint(accent, round.leader) end
  local summary = round.summary
  if type(summary) == "string" and summary ~= "" then
    read_only_text("##summary", summary, 122 * scale)
  end
end

local function load_ledger(plugin, state)
  local now = ui.GetTime()
  local revision, session = state.ledger_revision, state.ledger_session
  local changed = revision ~= ledger.revision or session ~= ledger.session
  if now >= ledger.refresh_at or changed then
    local data
    if plugin.ledger then
      if ledger.key then data = plugin.ledger(ledger.key) else data = plugin.ledger() end
    end
    if type(data) ~= "table" then data = { error = "Loot Ledger is unavailable." } end
    if ledger.data and type(data.current_session) == "string" and data.current_session ~= "" and
      ledger.data.current_session ~= data.current_session then
      ledger.key, ledger.filter, ledger.award_key = nil, "", nil
      discord = { page = 1 }
    elseif changed then
      discord.pages = nil
    end
    ledger.revision, ledger.session = revision, session
    ledger.data, ledger.refresh_at = data, revision == nil and now + 0.5 or math.huge
  end
  return ledger.data
end

local function session_options(data, all_sessions)
  local options = all_sessions and { { "", "All sessions" } } or {}
  for _, session in ipairs(data.sessions or {}) do
    options[#options + 1] = { session.key, session.label }
  end
  return options
end

local function session_exists(options, key)
  for _, option in ipairs(options) do
    if option[1] == key then return true end
  end
  return false
end

local function record_cards(data, scale)
  local entries, selected_index = {}, 1
  for _, entry in ipairs(data.entries or {}) do
    if ledger.filter == "" or entry.session == ledger.filter then
      entries[#entries + 1] = entry
      if entry.key == ledger.key then selected_index = #entries end
    end
  end
  if #entries == 0 then
    ui.TextDisabled("No completed rounds for this selection.")
    return nil
  end

  local layout = card_layout(ui.GetContentRegionAvail(), scale)
  local per_page = layout.columns * 2
  local page = (selected_index - 1) // per_page + 1
  local page_count = math.ceil(#entries / per_page)
  local first = (page - 1) * per_page + 1
  local chosen = entries[selected_index]
  local selected_key = chosen.key
  card_grid(entries, layout, first, math.min(first + per_page - 1, #entries), function(entry, width, height)
    local recipient = entry.recipient or ""
    local award = recipient ~= "" and ("Awarded to " .. recipient) or "Unawarded"
    local detail = award
    if entry.date and entry.date ~= "" then
      local date = entry.date:match("^%d%d%d%d%-(%d%d%-%d%d %d%d:%d%d)$") or entry.date
      local full = date .. " · " .. award
      detail = ui.CalcTextSize(full) <= width - 14 * scale and full or date
    end
    if round_card(entry, "##ledger-round-" .. entry.key, entry.key == selected_key, width, height, scale, detail, award) then
      chosen = entry
    end
  end)
  if page_count > 1 then
    local previous = action_button("Prev##ledger-rounds-page", page > 1)
    ui.SameLine()
    ui.TextDisabled(string.format("Page %d of %d", page, page_count))
    ui.SameLine()
    local next_page = action_button("Next##ledger-rounds-page", page < page_count)
    if previous then chosen = entries[first - per_page] end
    if next_page then chosen = entries[first + per_page] end
  end
  return chosen
end

local function award_editor(plugin, entry, scale)
  local recipient = entry.recipient or ""
  if ledger.award_key ~= entry.key then
    ledger.award_key, ledger.award_draft, ledger.saved_recipient = entry.key, recipient, recipient
  elseif recipient ~= ledger.saved_recipient then
    if ledger.award_draft == ledger.saved_recipient then ledger.award_draft = recipient end
    ledger.saved_recipient = recipient
  end

  award_status(entry)
  local save_width = 56 * scale
  local clear_width = recipient ~= "" and 58 * scale or 0
  local gap = 6 * scale
  local available = ui.GetContentRegionAvail()
  local clear_inline = available >= 160 * scale + save_width + clear_width + 2 * gap
  ui.SetNextItemWidth(math.max(82 * scale, available - save_width - gap - (clear_inline and clear_width + gap or 0)))
  ledger.award_draft = ui.InputTextWithHint("##award-recipient", "Player name", ledger.award_draft, 65)
  ui.SameLine(0, gap)
  local cleaned = ledger.award_draft:match("^%s*(.-)%s*$")
  local submit = action_button("Save##award", cleaned ~= recipient, primary_colors)
  if clear_width > 0 then
    if clear_inline then ui.SameLine(0, gap) end
    if ui.Button("Clear##award", clear_width) then
      cleaned, submit = "", true
    end
  end
  if submit then save_award(plugin, entry, cleaned) end
end

local ledger_views = { { "ranked", "Ranked" }, { "log", "Roll log" }, { "summary", "Result text" } }

local function ledger_panel(plugin, state, scale)
  back("Loot Ledger")
  if panel ~= "ledger" then return end
  local data = load_ledger(plugin, state)
  if type(data.error) == "string" and data.error ~= "" then wrap_tint(warning, data.error) end
  local sessions = session_options(data, true)
  if not session_exists(sessions, ledger.filter) then ledger.filter = "" end
  local width = ui.GetContentRegionAvail()
  local export_width = 135 * scale
  local inline = width >= 265 * scale
  local filter = basic_combo("##ledger-session", ledger.filter, sessions, inline and width - export_width - 6 * scale or -1)
  if filter ~= ledger.filter then ledger.filter, ledger.key = filter, nil end
  if inline then ui.SameLine(0, 6 * scale) end
  if ui.Button("Discord summary", export_width) then
    panel = "discord"
    discord = { session = ledger.filter ~= "" and ledger.filter or data.current_session, page = 1 }
    return
  end

  local chosen = record_cards(data, scale)
  if not chosen then return end
  if chosen.key ~= ledger.key then
    ledger.key, ledger.refresh_at = chosen.key, 0
    ledger.pages.ranked, ledger.pages.log = 1, 1
    data = load_ledger(plugin, state)
  end
  local entry = data.selected
  if not entry or entry.key ~= chosen.key then
    ui.TextDisabled("Details for this round are unavailable.")
    return
  end

  ui.Separator()
  ui.TextWrapped(entry.label)
  ui.TextDisabled(entry.date and entry.date ~= "" and (entry.date .. " · " .. entry.status) or entry.status)
  wrap_tint(accent, "Roll result  " .. (entry.leader and entry.leader ~= "" and entry.leader or "No eligible rolls"))
  award_editor(plugin, entry, scale)
  ui.Separator()
  ledger.view = basic_combo("##record-view", ledger.view, ledger_views, 132 * scale)
  if ledger.view == "summary" then
    if type(entry.summary) == "string" and entry.summary ~= "" then
      read_only_text("##ledger-record-text", entry.summary, 116 * scale)
    end
  else
    roll_table(plugin, entry, ledger.view, false, scale, width, ledger.pages)
  end
end

local function discord_panel(plugin, state, scale)
  back("Discord summary", "ledger")
  if panel ~= "discord" then return end
  local data = load_ledger(plugin, state)
  local options = session_options(data, false)
  if #options == 0 then
    ui.TextDisabled("No completed sessions available.")
    return
  end
  if not session_exists(options, discord.session) then
    discord.session = session_exists(options, data.current_session) and data.current_session or options[1][1]
    discord.pages, discord.page = nil, 1
  end
  local selected = basic_combo("##discord-session", discord.session, options, -1)
  if selected ~= discord.session then discord.session, discord.pages, discord.page = selected, nil, 1 end
  if not discord.pages then
    local pages = plugin.raid_summary and plugin.raid_summary(discord.session)
    discord.error = type(pages) ~= "table"
    discord.pages = discord.error and {} or pages
    discord.page = math.max(1, math.min(discord.page, #discord.pages))
  end
  if discord.error then
    wrap_tint(warning, "Could not load the Discord summary.")
    return
  elseif #discord.pages == 0 then
    ui.TextDisabled("No completed rounds in this session.")
    return
  end
  if #discord.pages > 1 then
    if action_button("Previous##discord-page", discord.page > 1) then discord.page = discord.page - 1 end
    ui.SameLine()
    ui.TextDisabled(string.format("Page %d of %d", discord.page, #discord.pages))
    ui.SameLine()
    if action_button("Next##discord-page", discord.page < #discord.pages) then discord.page = discord.page + 1 end
  end
  ui.TextWrapped("Click the report, then use Select All and Copy (Ctrl+A, Ctrl+C or the Command equivalents).")
  local text = discord.pages[discord.page]
  if type(text) == "string" then
    local id = string.format("##discord-text-%s-%s-%d", discord.session, ledger.revision or "", discord.page)
    read_only_text(id, text, 174 * scale)
  end
end

ui.window("Dicewatch", function()
  local plugin = hs.Plugin("dicewatch")
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
  local width = (compact and 370 or state.view == "both" and panel == "round" and 650 or 506) * scale
  local screen_width = io.width or 1280
  width = math.floor(math.max(1, math.min(width, screen_width - 24)))
  local rightmost = math.max(0, screen_width - width - 12)
  local x, y = ui.FindWindowRect("Dicewatch")
  if x and (x < 0 or x > rightmost) then
    ui.SetNextWindowPos(math.max(0, math.min(x, rightmost)), y, ui.Cond.Always)
  elseif not x and rightmost < 60 then
    ui.SetNextWindowPos(rightmost, 60, ui.Cond.FirstUseEver)
  end
  local color_count = ui.PushStyleColors(window_colors)
  ui.PushStyleVar(ui.StyleVar.Alpha, 1)
  ui.PushStyleVar(ui.StyleVar.WindowPadding, 11 * scale, 9 * scale)
  ui.PushStyleVar(ui.StyleVar.WindowBorderSize, 1)
  ui.PushStyleVar(ui.StyleVar.WindowRounding, 7 * scale)
  ui.PushStyleVar(ui.StyleVar.FrameBorderSize, 1)
  ui.PushStyleVar(ui.StyleVar.FrameRounding, 4 * scale)
  ui.PushStyleVar(ui.StyleVar.FramePadding, 7 * scale, 4 * scale)
  ui.PushStyleVar(ui.StyleVar.ItemSpacing, 6 * scale, 4 * scale)
  ui.SetNextWindowSize(width, 0, ui.Cond.Always)
  ui.SetNextWindowBgAlpha(1)
  local shown = ui.Begin("Dicewatch", true, ui.WindowFlags.NoTitleBar | ui.WindowFlags.AlwaysAutoResize)
  if shown then
    title_bar(plugin, state, scale, compact)
    if panel == "new" then
      rules_form(plugin, state, scale)
    elseif panel == "settings" then
      settings_panel(plugin, state, scale)
    elseif panel == "ledger" then
      ledger_panel(plugin, state, scale)
    elseif panel == "discord" then
      discord_panel(plugin, state, scale)
    elseif panel == "results" then
      back("Round results")
      if panel == "results" then
        local round = state.current
        if round and type(round.key) == "string" and round.key ~= "" then
          if ui.Button("Record award") then open_ledger(round.key) end
        end
        if panel == "results" then results_text(round, scale) end
      end
    else
      current_round(plugin, state, scale, compact)
    end
    if panel == "round" or panel == "ledger" or panel == "results" then
      announcement_notice(plugin, state.announcement, scale, compact)
    end
    if type(state.message) == "string" and state.message ~= "" and
      (state.error or state.message:find("for this session", 1, true)) then
      if state.error then wrap_tint(warning, state.message) else ui.TextWrapped(state.message) end
    end
  end
  ui.End()
  ui.PopStyleVar(8)
  ui.PopStyleColor(color_count)
end)

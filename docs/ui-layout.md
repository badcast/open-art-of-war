# Interface layout (`assets/ui/layout.yaml`)

Where the pieces of the interface stand is data, not code. Edit the file in any editor while the game runs: it is read
again about once a second. A broken edit keeps the last good layout and prints `[UiLayout] ... (the last good layout
stays)` to the console. The file is visual only (`net_visual` in `main.yaml`), it never affects a network match.

Colours stay in `characters/game_ui.yaml`.

## Structure

```yaml
screens:
  hud:                 # a screen
    touch:             # a profile: touch (Android, or --touch-ui) or pc (desktop)
      scale: "clamp(H / 720, 1, 2)"   # `u` below
      nodes:
        - {id: menu, type: button, label: "MENU", action: menu, rect: ["W - 96u", "8u", "88u", "46u"]}
```

- **rect** `[x, y, w, h]`, each an expression: `W`, `H` (window), `u` (the profile's scale), numbers (`8u` = 8 * u),
  `+ - * / ( )`, `min(a,b)`, `max(a,b)`, `clamp(v,lo,hi)`, and the rects of nodes written above:
  `<id>.x .y .w .h .right .bottom .cx .cy`.
- **type**: `panel`, `button` (`label` is translated, `style`: primary / danger / ghost), `label`, `radar`, `cards`
  (the area the game fills with build / training cards), `area` (a place the game draws into), `flow`.
- **flow**: `dir: row|column`, `gap`, `children` with `size: [w, h]`; children that are hidden take no room, the ones
  that do not fit are left out.
- **visible_if / active_if / disabled_if**: names the game answers, combined with `! && || ( )`.
- **action**: what a button does (the game knows the names).

## HUD, profile `touch`
Conditions: `placing`, `panel_open`, `panel_build`, `panel_units`, `units_inf`, `units_veh`,
`tactical` (zoomed out into the radar map: no building), `attack_mode`, `sell_armed`, `has_combat`, `has_building`, `has_selection`.
Actions: `menu goals tech dock_build dock_units attack_move stop hold siege clear select_all repair sell
confirm cancel units_infantry units_vehicles scroll_left scroll_right`.

## Examples
- Move a button: change its `rect`. Left-handed layout: swap the `radar` and dock rects.
- Bigger buttons on a tablet: change `scale` (`clamp(H / 600, 1, 3)`).

## HUD, profile `pc` (the sidebar)
Names the game reads, all required: `panel header radar menu objectives menu_s objectives_s slower speed faster research
tab0..tab3 strip scroll_up scroll_down primary grid info`. `T` is the space the debug toolbar takes at the top, the
condition `speed` is on while the single-player speed buttons show (`menu_s` / `objectives_s` replace `menu` /
`objectives` then). `grid` holds the cards: 2 columns of 136 x 84. The sidebar is 300 px wide; the map viewport
reserves that much (`HUDController::kSidebarWidth`), so widening `panel` needs that constant changed too.
Without a `pc` profile the built-in numbers are used.

## Menus (profile `pc`)
Each screen is centred or anchored by its own nodes; the rows and cards inside a dialog are still the game's.
- `main_menu`: `title` (an area: x, y of the title block), `item0..item9`.
- `pause`: `dialog`, `item0..item5`.
- `settings`: `dialog`, `tab0..tab3`, `back`, `defaults`, `list` (the area the rows fill: 56 high, 6 apart; as many
  as fit are shown, the rest scroll).
- `network`: `dialog`, `name`, `password`, `address`, `connect`, `lan_refresh`, `lan_list`, `server_refresh`,
  `server_list`, `code`, `code_join`, `back`, `join`, `host`, `server_create`.
- `map_select`: the map window shared by the campaign, the custom game (skirmish) and the network room: `dialog`, `list`
  (maps / missions), `details`, footer buttons `back start side editor difficulty`, match options `credits time tech`, and
  the room's own `room_search room_list room_chat_field room_chat_send room_chat_log room_leave room_start room_ready`.
  The picture and the check boxes inside the details are derived from `details` by the game.
- `research`: the technology tree window, profiles `pc` and `touch` (finger-sized controls, the tree and its text grow with `u`).
  Names: `dialog content search filter close details tree start zoom_out zoom_in` (all required, else the built-in numbers
  are used). `T` is the bar of an observer at the top. In the `touch` profile a finger drags the tree and it glides on after
  a flick, the scroll bars are only indicators, a double tap on a technology starts it and closes the window.
Other dialogs (room seats, replays) keep their built-in layout for now.

## Touch profiles of the menus
`main_menu`, `pause` and `confirm` (Yes / No) have a `touch` profile; the other menu screens use `pc` on touch too: their rows,
pictures and check boxes are sized by the game, so scaling only the rectangles of the file made some things bigger and some
not (and overlapped them). A screen without the profile falls back to `pc`.

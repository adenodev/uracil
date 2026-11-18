## tui — alternate-screen apps rendered as a fixed character grid.
##
## The host page draws `lines` in a monospace pane and forwards key codes
## (KeyUp..KeyBackspace, or the character code for printables).

import std/[strutils, algorithm, json]
import uplatform, vfs, net, perms

const
  KeyUp* = 1
  KeyDown* = 2
  KeyLeft* = 3
  KeyRight* = 4
  KeyEnter* = 5
  KeyEsc* = 6
  KeyBackspace* = 7

var width* = 72
var height* = 16
var bootSeconds*: float64 = 0

type
  Tui = ref object
    app: string
    title: string
    status: string
    lines: seq[string]
    scroll: int
    cursor: int
    input: string
    inputMode: bool

var current: Tui = nil
var screenActive* = false

proc keyChar(c: char): int = ord(c)

proc newTui(app: string): Tui =
  Tui(app: app, title: app, status: "", lines: @[], scroll: 0, cursor: 0,
      input: "", inputMode: false)

proc stripTags*(html: string): string =
  var inTag = false
  var prevSpace = false
  for c in html:
    if c == '<':
      inTag = true
      continue
    if c == '>':
      inTag = false
      continue
    if inTag: continue
    let isSpace = c in {' ', '\t', '\r', '\n'}
    if isSpace:
      if not prevSpace: result.add ' '
      prevSpace = true
    else:
      result.add c
      prevSpace = false
  result = result.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
                 .replace("&quot;", "\"").replace("&#39;", "'").replace("&nbsp;", " ")

proc wrapText*(text: string, width: int): seq[string] =
  var line = ""
  for word in text.splitWhitespace():
    if line.len + word.len + 1 > width:
      result.add line
      line = word
    else:
      if line.len > 0: line.add ' '
      line.add word
  if line.len > 0: result.add line

proc closeApp*() =
  current = nil
  screenActive = false

# ── hosted doom ──────────────────────────────────────────────────────────

proc refreshDoomFrame*() =
  if current == nil or current.app != "doom":
    return
  when not defined(irisNative):
    let frame = $irisGlueDoomFrame()
    if frame.len > 0:
      current.lines = frame.splitLines()

proc forwardDoomKey*(key: int, pressed: int) =
  when not defined(irisNative):
    irisGlueDoomKey(cint(key), cint(pressed))
  refreshDoomFrame()

# ── files ────────────────────────────────────────────────────────────────

proc refreshFiles(t: Tui) =
  t.lines = @[]
  var entries: seq[string]
  for line in vfs.list().splitLines():
    let name = line.strip().strip(chars = {'-', ' '})
    if name.len > 0: entries.add name
  entries.sort()
  if entries.len == 0:
    t.lines.add "(empty — try: write note.txt hello)"
  for i, entry in entries:
    let mark = if i == t.cursor: ">" else: " "
    let size = vfs.fileSize(entry)
    t.lines.add mark & " " & entry.alignLeft(min(30, width - 10)) &
      (if size >= 0: $size & " B" else: "")
  t.status = "arrows move · Enter view · d delete · q quit"
  if t.cursor >= entries.len: t.cursor = max(0, entries.len - 1)

proc openPager(t: Tui, name: string) =
  var data: string
  if not vfs.read(name, data):
    t.status = "cannot read " & name
    return
  t.app = "pager"
  t.title = "pager · " & name
  t.scroll = 0
  t.lines = data.splitLines()
  t.status = "arrows scroll · q back"

# ── monitor ──────────────────────────────────────────────────────────────

proc refreshMonitor(t: Tui) =
  t.lines = @[]
  t.lines.add "SERVICE"
  when not defined(irisNative):
    for l in ($irisGlueDescribe()).splitLines():
      t.lines.add " " & l
  else:
    t.lines.add " iris.adeno.ltd · 0.1.0 · uracil.service.v1 (native twin)"
  t.lines.add ""
  t.lines.add "GRANTS"
  for i in 0 ..< PermCount:
    let on = (grantedMask and permMask(i)) != 0
    t.lines.add " [" & (if on: "x" else: " ") & "] " & PermNames[i]
  t.lines.add ""
  let counts = vfs.countFiles()
  t.lines.add "STORAGE   " & $counts.count & " files · " & $counts.bytes & " B · " & vfs.fsRoot
  if hasPerm(PermClockRead):
    var now: float64
    if platformTime(now):
      t.lines.add "UPTIME    " & formatFloat(now - bootSeconds, ffDecimal, 1) & " s"
  else:
    t.lines.add "UPTIME    withheld (grant clock.read)"
  t.status = "r refresh · q quit"

# ── browser ──────────────────────────────────────────────────────────────

proc refreshBrowserInput(t: Tui) =
  t.lines = @[
    "text-mode web browser · gated by net.client",
    "",
    "url> " & t.input & "_"
  ]
  t.status = "type a url · Enter fetch · q quit"

proc browserFetch(t: Tui) =
  if not hasPerm(PermNetClient):
    t.status = "net.client withheld — run: perm grant net.client"
    return
  t.inputMode = false
  t.scroll = 0
  t.lines = @["fetching " & t.input & " …"]
  let res = httpGet(t.input)
  if not res.ok and res.error.len > 0:
    t.lines = @["fetch failed: " & res.error]
  else:
    var text = stripTags(res.body)
    t.lines = @["HTTP " & $res.status & " · " & $res.body.len & " bytes · " & t.input, ""]
    t.lines.add wrapText(text, width)
  t.status = "arrows scroll · u new url · q quit"

# ── key routing ──────────────────────────────────────────────────────────

proc handleKey*(key: int): string =
  if current == nil or not screenActive: return ""
  let t = current
  case t.app
  of "files":
    if key == keyChar('q'): closeApp()
    elif key == KeyUp: t.cursor = max(0, t.cursor - 1)
    elif key == KeyDown: t.cursor = min(max(0, t.lines.len - 1), t.cursor + 1)
    elif key == KeyEnter:
      var entries: seq[string]
      for line in t.lines:
        if line.len > 2 and (line[0] == '>' or line[0] == ' '):
          let name = line[2 .. ^1].split(" ")[0]
          if name.len > 0 and name[0] != '(': entries.add name
      if t.cursor < entries.len: openPager(t, entries[t.cursor])
    elif key == keyChar('d'):
      var entries: seq[string]
      for line in t.lines:
        if line.len > 2 and (line[0] == '>' or line[0] == ' '):
          let name = line[2 .. ^1].split(" ")[0]
          if name.len > 0 and name[0] != '(': entries.add name
      if t.cursor < entries.len:
        if hasPerm(PermFsWrite):
          discard vfs.remove(entries[t.cursor])
        else:
          t.status = "fs.write withheld — run: perm grant fs.write"
      refreshFiles(t)
    elif key == keyChar('r'): refreshFiles(t)
  of "pager":
    if key == keyChar('q') or key == KeyEsc:
      t.app = "files"
      t.title = "files"
      refreshFiles(t)
    elif key == KeyUp: t.scroll = max(0, t.scroll - 1)
    elif key == KeyDown: t.scroll = min(max(0, t.lines.len - height), t.scroll + 1)
  of "monitor":
    if key == keyChar('q'): closeApp()
    elif key == keyChar('r'): refreshMonitor(t)
  of "browser":
    if key == keyChar('q'): closeApp()
    elif t.inputMode:
      if key == KeyEnter: browserFetch(t)
      elif key == KeyBackspace:
        if t.input.len > 0: t.input = t.input[0 ..< t.input.len - 1]
        refreshBrowserInput(t)
      elif key >= 32 and key < 127:
        t.input.add char(key)
        refreshBrowserInput(t)
    else:
      if key == keyChar('u'):
        t.inputMode = true
        refreshBrowserInput(t)
      elif key == KeyUp: t.scroll = max(0, t.scroll - 1)
      elif key == KeyDown: t.scroll = min(max(0, t.lines.len - height), t.scroll + 1)
  of "doom":
    # The hosted engine owns input; the page forwards raw keys through
    # iris_doom_key() and the host's tick pump refreshes frames.
    discard
  else: discard
  result = ""

proc openApp*(name: string): string =
  var openedText = ""
  case name
  of "files":
    current = newTui("files")
    refreshFiles(current)
  of "monitor":
    current = newTui("monitor")
    refreshMonitor(current)
  of "browser":
    current = newTui("browser")
    current.inputMode = true
    current.input = "https://adeno.ltd/"
    refreshBrowserInput(current)
  of "doom":
    when not defined(irisNative):
      # Downloads (gated), stages, loads and spawns the engine as a Uracil
      # service; the result text is the engine's own banner/refusal.
      openedText = ($irisGlueDoomOpen()).strip()
      if int(irisGlueDoomActive()) != 1:
        return (if openedText.len > 0: openedText else: "iris: doom engine unavailable")
      current = newTui("doom")
      current.title = "doom"
      current.status = "arrows move · ctrl fire · space use · shift run · q quits"
      refreshDoomFrame()
    else:
      return "iris: doom requires the Uracil wasm build"
  else:
    return "unknown app: " & name & " (files | monitor | browser | doom)"
  screenActive = true
  result = openedText

proc frameJson*(): string =
  if current == nil or not screenActive:
    return "{\"active\":false}"
  var lines = current.lines
  if current.app == "doom":
    # The hosted engine's ANSI frames come straight from the kernel.
    return $(%*{
      "active": true,
      "app": "doom",
      "title": "doom",
      "status": current.status,
      "input": "",
      "lines": lines
    })
  elif current.app in ["pager", "browser"] and not current.inputMode:
    let start = min(current.scroll, max(0, lines.len - height))
    lines = lines[start ..< min(lines.len, start + height)]
  elif current.app == "monitor":
    lines = lines[0 ..< min(lines.len, height)]
  else:
    lines = lines[0 ..< min(lines.len, max(height, 12))]
  result = $(%*{
    "active": true,
    "app": current.app,
    "title": current.title,
    "status": current.status,
    "input": (if current.inputMode: current.input else: ""),
    "lines": lines
  })

proc tickActive*(): string =
  ## Called by the host page's animation loop while a TUI app is open.
  ## Returns a fresh frame JSON when something changed, "" otherwise.
  if current == nil or not screenActive:
    return ""
  if current.app != "doom":
    return ""
  when not defined(irisNative):
    discard irisGlueDoomTick()
    let state = int(irisGlueDoomState())
    discard irisGlueDoomTakeOutput()
    if state == 2:
      closeApp()
      return "{\"active\":false}"
    refreshDoomFrame()
  return frameJson()

## iris — the capability shell for the Hermes enclave.
##
## Boots inside the real Uracil kernel (wasm32 build) as an in-process bundle;
## the identical source also compiles natively (‑d:irisNative) as the local
## twin of the same shell. Every capability is a permission the kernel owns.

import std/strutils
import uplatform, banner, vfs, net, perms, tui

var outBuf = ""
var bootSeconds: float64 = 0

proc tokenize(input: string): seq[string] =
  var cur = ""
  var inQuotes = false
  for c in input:
    if c == '"':
      inQuotes = not inQuotes
      continue
    if c == ' ' and not inQuotes:
      if cur.len > 0:
        result.add cur
        cur = ""
    else:
      cur.add c
  if cur.len > 0: result.add cur

proc usageLine(cmd: string): string =
  "usage: " & cmd

proc dispatch(input: string): string =
  let parts = tokenize(input.strip())
  if parts.len == 0: return ""
  let cmd = parts[0].toLowerAscii()

  case cmd
  of "help", "?":
    result = """
iris — capability shell for the Hermes enclave

  help                     this list
  banner                   redraw the mark
  uname · whoami           system + session identity
  describe                 kernel bundle descriptor (uracil.service.v1)
  date                     clock (clock.read)

  ls                       list sandbox files (fs.read)
  cat <file>               print a file (fs.read)
  write <file> <text>      write a file (fs.write)
  rm <file>                delete a file (fs.write)
  mkdir <dir>              create a directory (fs.write)

  net get <url>            gated HTTP fetch (net.client)
  perm                     show grants
  perm grant <perm>        grant (fs.read · fs.write · net.client · clock.read)
  perm revoke <perm>       revoke
  ps                       running services
  campaign [stage|load|complete|destroy]

  tui files · monitor · browser · doom
  clear                    clear the screen""".strip()
  of "clear": result = "\x0c"
  of "banner": result = IrisBanner.strip(leading = false) & "\n" & IrisTagline
  of "uname":
    result = "iris 0.1.0 · uracil 0.2.0 · wasm32 · rootless · abi uracil.service.v1"
  of "whoami":
    result = "uid=demo · service=" & IrisService & " · confined to " & vfs.fsRoot
  of "echo":
    if parts.len > 1: result = parts[1 .. ^1].join(" ")
  of "describe":
    when not defined(irisNative):
      result = ($irisGlueDescribe()).strip()
    else:
      result = "service: iris.adeno.ltd\nversion: 0.1.0 (native twin)\nabi: uracil.service.v1\nmode: rootless"
  of "date":
    if not hasPerm(PermClockRead):
      return denyMsg("date", "clock.read")
    var now: float64
    if platformTime(now):
      result = "unix " & formatFloat(now, ffDecimal, 2) & " · uptime " &
        formatFloat(now - bootSeconds, ffDecimal, 1) & "s"
    else:
      result = "iris: clock unavailable"
  of "ls":
    if not hasPerm(PermFsRead): return denyMsg("ls", "fs.read")
    result = vfs.list().strip()
    if result.len == 0: result = "(empty — try: write note.txt hello)"
  of "cat":
    if parts.len < 2: return usageLine("cat <file>")
    if not hasPerm(PermFsRead): return denyMsg("cat " & parts[1], "fs.read")
    var data: string
    if vfs.read(parts[1], data): result = data
    else: result = "iris: cannot read '" & parts[1] & "'"
  of "write":
    if parts.len < 3: return usageLine("write <file> <text>")
    if not hasPerm(PermFsWrite): return denyMsg("write " & parts[1], "fs.write")
    let text = parts[2 .. ^1].join(" ")
    if vfs.write(parts[1], text):
      result = "wrote " & $text.len & " B to " & parts[1]
    else:
      result = "iris: cannot write '" & parts[1] & "' (invalid name)"
  of "rm":
    if parts.len < 2: return usageLine("rm <file>")
    if not hasPerm(PermFsWrite): return denyMsg("rm " & parts[1], "fs.write")
    result = if vfs.remove(parts[1]): "removed " & parts[1]
             else: "iris: cannot remove '" & parts[1] & "'"
  of "mkdir":
    if parts.len < 2: return usageLine("mkdir <dir>")
    if not hasPerm(PermFsWrite): return denyMsg("mkdir " & parts[1], "fs.write")
    if vfs.validName(parts[1]):
      result = if platformMkdir(vfs.fsPath(parts[1])): "created " & parts[1]
               else: "iris: cannot create '" & parts[1] & "'"
    else:
      result = "iris: invalid name"
  of "net":
    if parts.len < 3 or parts[1].toLowerAscii() != "get":
      return usageLine("net get <url>")
    if not hasPerm(PermNetClient): return denyMsg("net get " & parts[2], "net.client")
    result = "GET " & parts[2] & " …"
    let res = httpGet(parts[2])
    if not res.ok and res.error.len > 0:
      result = "iris: fetch failed — " & res.error
    else:
      var preview = stripTags(res.body)
      if preview.len > 700: preview = preview[0 ..< 700] & " …"
      result = "HTTP " & $res.status & " · " & $res.body.len & " bytes\n" & preview
  of "perm", "perms":
    if parts.len == 1:
      result = grantSummary()
      when not defined(irisNative):
        result = ($irisGluePermList()).strip()
    elif parts[1].toLowerAscii() == "list":
      when not defined(irisNative):
        result = ($irisGluePermList()).strip()
      else:
        result = grantSummary()
    elif parts[1].toLowerAscii() == "grant" and parts.len >= 3:
      var messages: seq[string]
      for i in 2 ..< parts.len:
        messages.add updateGrant(parts[i], true)
      result = messages.join("\n")
    elif parts[1].toLowerAscii() == "revoke" and parts.len >= 3:
      var messages: seq[string]
      for i in 2 ..< parts.len:
        messages.add updateGrant(parts[i], false)
      result = messages.join("\n")
    else:
      result = "usage: perm [list | grant <perm>… | revoke <perm>…]"
  of "ps":
    result = "PID  SERVICE           STATE\n" &
      "0001 uracil.kernel     running · wasm32 · rootless\n" &
      "0002 iris.adeno.ltd    running · this shell\n" &
      "0003 campaign.kernel   idle (campaign stage|load|complete|destroy)"
  of "campaign":
    when not defined(irisNative):
      let sub = if parts.len >= 2: parts[1].toLowerAscii() else: ""
      case sub
      of "stage": result = ($irisGlueCampaignStage()).strip()
      of "load": result = ($irisGlueCampaignLoad()).strip()
      of "complete": result = ($irisGlueCampaignComplete()).strip()
      of "destroy": result = ($irisGlueCampaignDestroy()).strip()
      of "": result = ($irisGlueCampaign()).strip()
      else: result = usageLine("campaign [stage|load|complete|destroy]")
    else:
      result = "iris: campaign kernel requires the Uracil wasm build"
  of "tui":
    if parts.len < 2: return usageLine("tui <files|monitor|browser|doom>")
    # `tui doom` is gated inside the kernel glue, which lists every missing
    # permission (net.client, fs.read, fs.write, clock.read) at once.
    result = openApp(parts[1].toLowerAscii())
  else:
    result = "iris: unknown command: " & parts[0] & " (try help)"

# ── exports consumed by app/iris_bundle.c (and the native twin main) ────

proc iris_set_grants*(mask: uint64) {.exportc, cdecl.} =
  loadGrants(mask)

proc iris_set_state*(dir: cstring) {.exportc, cdecl.} =
  initVfs($dir)
  discard platformTime(bootSeconds)
  tui.bootSeconds = bootSeconds

proc iris_set_size*(w, h: cint) {.exportc, cdecl.} =
  tui.width = max(24, int(w))
  tui.height = max(8, int(h))

proc iris_boot*(): cstring {.exportc, cdecl.} =
  outBuf = ""
  outBuf.add "[iris] uracil runtime 0.2.0 · wasm32 · rootless service context\n"
  outBuf.add "[iris] bundle iris.adeno.ltd loaded · abi uracil.service.v1\n"
  when not defined(irisNative):
    outBuf.add "[iris] " & ($irisGlueKernelLog()).strip() & "\n"
  outBuf.add "[iris] " & grantSummary().replace("\n", "\n[iris] ") & "\n\n"
  outBuf.add IrisBanner.strip(leading = false) & "\n"
  outBuf.add IrisTagline & "\n"
  outBuf.add "tip: \"net get /content/legal/tos.md\" is denied until: perm grant net.client"
  result = outBuf.cstring

proc iris_exec*(line: cstring): cstring {.exportc, cdecl.} =
  outBuf = dispatch($line)
  result = outBuf.cstring

proc iris_output*(): cstring {.exportc, cdecl.} =
  ## Stable pointer to the most recent output buffer (read after any call
  ## that may have suspended via Asyncify).
  result = outBuf.cstring

proc iris_key*(key: cint): cstring {.exportc, cdecl.} =
  result = tui.handleKey(int(key)).cstring

proc iris_doom_key*(key: cint, pressed: cint) {.exportc, cdecl.} =
  ## Raw doom key events from the page (down and up).
  tui.forwardDoomKey(int(key), int(pressed))

proc iris_screen*(): cstring {.exportc, cdecl.} =
  outBuf = tui.frameJson()
  result = outBuf.cstring

proc iris_tick*(): cstring {.exportc, cdecl.} =
  ## The page's animation loop pumps hosted apps through this.
  outBuf = tui.tickActive()
  result = outBuf.cstring

proc iris_doom_closed*() {.exportc, cdecl.} =
  ## The page calls this when the player quits the doom app.
  when not defined(irisNative):
    irisGlueDoomClose()
  tui.closeApp()

# ── native twin: a straight REPL over the same dispatcher ───────────────

when defined(irisNative):
  import std/os

  proc nativeMain() =
    loadGrants(permissionLoad())
    initVfs(nativeStateDir)
    discard platformTime(bootSeconds)
    tui.bootSeconds = bootSeconds
    echo "[iris] native twin · ", IrisService
    echo grantSummary()
    echo IrisBanner
    while true:
      stdout.write("iris❯ ")
      stdout.flushFile()
      let line = stdin.readLine()
      if line.len == 0 and endOfFile(stdin): break
      if line.strip() == "exit" or line.strip() == "quit": break
      let output = dispatch(line)
      if output.len > 0: echo output

  when isMainModule:
    nativeMain()

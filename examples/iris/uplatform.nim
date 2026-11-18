## uplatform — the Uracil surface iris runs on.
##
## On the wasm kernel build the shell is linked into the real Uracil runtime,
## so these bindings resolve to the kernel's platform + permission functions
## (and to a thin glue layer in app/iris_bundle.c). The native twin implements
## the same surface over POSIX so the identical shell logic compiles standalone.
##
## Every capability call the shell makes goes through this module; permission
## enforcement is applied here with the kernel's own mask arithmetic.

const
  PermFsRead* = 1'u64 shl 0
  PermFsWrite* = 1'u64 shl 1
  PermNetClient* = 1'u64 shl 2
  PermNetServer* = 1'u64 shl 3
  PermProcSpawn* = 1'u64 shl 4
  PermClockRead* = 1'u64 shl 5
  PermCount* = 6

const PermNames* = ["fs.read", "fs.write", "net.client", "net.server", "proc.spawn", "clock.read"]

proc permMask*(id: int): uint64 = 1'u64 shl id

when defined(irisNative):
  # ── Native twin backend (POSIX; local-only reference build) ──────────
  import std/os
  import std/random
  import std/strutils
  import std/times

  proc free(p: pointer) {.importc: "free", header: "<stdlib.h>".}

  var nativeStateDir* = getEnv("IRIS_STATE_DIR", getTempDir() / "iris-state")

  proc readInto(path: string, data: var string): bool =
    try:
      data = readFile(path)
      result = true
    except CatchableError:
      result = false

  proc platformRead*(path: string, data: var string): bool = readInto(path, data)

  proc platformWrite*(path: string, data: string): bool =
    try:
      createDir(parentDir(path))
      writeFile(path, data)
      result = true
    except CatchableError:
      result = false

  proc platformRemove*(path: string): bool =
    try:
      removeFile(path)
      result = true
    except CatchableError:
      result = false

  proc platformMkdir*(path: string): bool =
    try:
      createDir(path)
      result = true
    except CatchableError:
      result = false

  proc platformList*(path: string): string =
    try:
      for kind, p in walkDir(path):
        result.add("- " & extractFilename(p) & "\n")
    except CatchableError:
      result = "unable to open directory '" & path & "'\n"

  proc platformTime*(outSeconds: var float64): bool =
    outSeconds = epochTime()
    result = true

  proc platformNet*(verb, url, headers, body: string, resp: var string): bool =
    result = false
    resp = ""

  proc platformRandom*(buf: pointer, len: int): bool =
    for i in 0 ..< len:
      cast[ptr uint8](cast[int](buf) + i)[] = uint8(rand(255))
    result = true

  # permissions: native twin keeps a simple grants file with identical semantics
  proc maskContains*(granted, required: uint64): bool = (granted and required) == required
  proc permName*(id: int): string = PermNames[id]
  proc permFromName*(name: string): int =
    result = -1
    for i, n in PermNames:
      if n == name: return i

  var grantsPath = nativeStateDir / "grants.iris.perm"

  proc permissionLoad*(): uint64 =
    result = 0'u64
    var text: string
    if readInto(grantsPath, text):
      for line in text.splitLines():
        let id = permFromName(line.strip())
        if id >= 0: result = result or permMask(id)

  proc permissionStore*(mask: uint64) =
    var text = ""
    for i in 0 ..< PermCount:
      if (mask and permMask(i)) != 0: text.add(PermNames[i] & "\n")
    discard platformWrite(grantsPath, text)

else:
  # ── In-kernel backend: real Uracil kernel + iris glue ────────────────
  proc urPlatformReadFile(path: cstring, outData: ptr pointer, outSize: ptr csize_t): cint
    {.importc: "ur_platform_read_file", cdecl.}
  proc urPlatformWriteFile(path: cstring, data: pointer, size: csize_t): cint
    {.importc: "ur_platform_write_file", cdecl.}
  proc urPlatformRemoveFile(path: cstring): cint
    {.importc: "ur_platform_remove_file", cdecl.}
  proc urPlatformCreateDirectory(path: cstring): cint
    {.importc: "ur_platform_create_directory", cdecl.}
  proc urPlatformGetTime(ts: ptr float64): cint
    {.importc: "ur_platform_get_time", cdecl.}
  proc urPlatformNetworkRequest(verb, url, headers: cstring, body: cstring,
                                bodyLen: csize_t, respBuf: cstring, respBufLen: csize_t,
                                outLen: ptr csize_t): cint
    {.importc: "ur_platform_network_request", cdecl.}
  proc urPlatformRandomBytes(buf: pointer, len: csize_t): cint
    {.importc: "ur_platform_random_bytes", cdecl.}

  proc urPermissionMaskContains(granted, required: uint64): bool
    {.importc: "ur_permission_mask_contains", cdecl.}
  proc urPermissionFromName(name: cstring): cint
    {.importc: "ur_permission_from_name", cdecl.}
  proc urPermissionName(id: cint): cstring
    {.importc: "ur_permission_name", cdecl.}

  # glue in app/iris_bundle.c
  proc irisGlueListDir*(path: cstring): cstring
    {.importc: "iris_glue_list_dir", cdecl.}
  proc irisGluePermUpdate*(name: cstring, grant: cint, outMask: ptr uint64): cstring
    {.importc: "iris_glue_perm_update", cdecl.}
  proc irisGluePermList*(): cstring
    {.importc: "iris_glue_perm_list", cdecl.}
  proc irisGlueDescribe*(): cstring
    {.importc: "iris_glue_describe", cdecl.}
  proc irisGlueCampaign*(): cstring
    {.importc: "iris_glue_campaign", cdecl.}
  proc irisGlueCampaignStage*(): cstring
    {.importc: "iris_glue_campaign_stage", cdecl.}
  proc irisGlueCampaignLoad*(): cstring
    {.importc: "iris_glue_campaign_load", cdecl.}
  proc irisGlueCampaignComplete*(): cstring
    {.importc: "iris_glue_campaign_complete", cdecl.}
  proc irisGlueCampaignDestroy*(): cstring
    {.importc: "iris_glue_campaign_destroy", cdecl.}
  proc irisGlueDoomOpen*(): cstring
    {.importc: "iris_glue_doom_open", cdecl.}
  proc irisGlueDoomTick*(): cstring
    {.importc: "iris_glue_doom_tick", cdecl.}
  proc irisGlueDoomFrame*(): cstring
    {.importc: "iris_glue_doom_frame", cdecl.}
  proc irisGlueDoomTakeOutput*(): cstring
    {.importc: "iris_glue_doom_take_output", cdecl.}
  proc irisGlueDoomState*(): cint
    {.importc: "iris_glue_doom_state", cdecl.}
  proc irisGlueDoomActive*(): cint
    {.importc: "iris_glue_doom_active", cdecl.}
  proc irisGlueDoomKey*(key, pressed: cint)
    {.importc: "iris_glue_doom_key", cdecl.}
  proc irisGlueDoomClose*()
    {.importc: "iris_glue_doom_close", cdecl.}
  proc irisGlueKernelLog*(): cstring
    {.importc: "iris_glue_kernel_log", cdecl.}

  proc free(p: pointer) {.importc: "free", header: "<stdlib.h>".}

  proc platformRead*(path: string, data: var string): bool =
    var p: pointer
    var size: csize_t
    if urPlatformReadFile(path.cstring, addr p, addr size) == 0:
      data = newString(size.int)
      if size.int > 0: copyMem(data[0].addr, p, size.int)
      free(p)
      result = true
    else:
      result = false

  proc platformWrite*(path: string, data: string): bool =
    var payload = data
    urPlatformWriteFile(path.cstring, payload[0].addr, csize_t(payload.len)) == 0

  proc platformRemove*(path: string): bool =
    urPlatformRemoveFile(path.cstring) == 0

  proc platformMkdir*(path: string): bool =
    urPlatformCreateDirectory(path.cstring) == 0

  proc platformList*(path: string): string =
    let listing = irisGlueListDir(path.cstring)
    if listing == nil: return "unable to open directory '" & path & "'\n"
    result = $listing

  proc platformTime*(outSeconds: var float64): bool =
    urPlatformGetTime(addr outSeconds) == 0

  proc platformNet*(verb, url, headers, body: string, resp: var string): bool =
    var buf = newString(65536)
    var outLen: csize_t
    let rc = urPlatformNetworkRequest(verb.cstring, url.cstring, headers.cstring,
                                      nil, csize_t(0), buf.cstring, csize_t(buf.len), addr outLen)
    if rc != 0: return false
    resp = buf[0 ..< min(int(outLen), buf.len)]
    result = true

  proc platformRandom*(buf: pointer, len: int): bool =
    urPlatformRandomBytes(buf, csize_t(len)) == 0

  proc maskContains*(granted, required: uint64): bool =
    urPermissionMaskContains(granted, required)

  proc permName*(id: int): string =
    if id >= 0 and id < PermCount: $urPermissionName(cint(id)) else: "unknown"

  proc permFromName*(name: string): int =
    int(urPermissionFromName(name.cstring))

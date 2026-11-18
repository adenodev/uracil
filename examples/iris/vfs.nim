## vfs — the shell's sandboxed filesystem, rooted at <state>/fs.
##
## Reads and writes go through the platform surface (uplatform), which the
## kernel mediates; permission checks happen in the command dispatch layer.

import std/strutils
import uplatform

var fsRoot* = ""

proc initVfs*(stateDir: string) =
  fsRoot = stateDir & "/fs"
  discard platformMkdir(fsRoot)

proc validName*(name: string): bool =
  if name.len == 0 or name.len > 64: return false
  if name.contains('/') or name.contains("..") or name[0] == '.': return false
  for c in name:
    if c notin {'a'..'z', 'A'..'Z', '0'..'9', '.', '-', '_'}: return false
  result = true

proc fsPath*(name: string): string = fsRoot & "/" & name

proc list*(): string =
  platformList(fsRoot)

proc read*(name: string, data: var string): bool =
  if not validName(name): return false
  platformRead(fsPath(name), data)

proc write*(name, data: string): bool =
  if not validName(name): return false
  platformWrite(fsPath(name), data)

proc remove*(name: string): bool =
  if not validName(name): return false
  platformRemove(fsPath(name))

proc fileSize*(name: string): int =
  var data: string
  if read(name, data): result = data.len else: result = -1

proc countFiles*(): tuple[count: int, bytes: int] =
  let listing = list()
  for line in listing.splitLines():
    let name = line.strip().strip(chars = {'-', ' '})
    if name.len == 0: continue
    let size = fileSize(name)
    if size >= 0:
      inc result.count
      result.bytes += size

## perms — the shell's view of the kernel grant mask.
##
## The mask itself is owned by the kernel (ur_permission_*). This module keeps
## the shell-side copy in sync via the glue layer and renders the same refusal
## text the kernel stub uses, so denials read identically everywhere.

import std/strutils
import uplatform

const IrisService* = "iris.adeno.ltd"

var grantedMask*: uint64 = 0

proc hasPerm*(perm: uint64): bool = maskContains(grantedMask, perm)

proc loadGrants*(mask: uint64) =
  grantedMask = mask

proc denyMsg*(cmd, permName: string): string =
  "iris: refusing to run '" & cmd & "'; missing permissions: " & permName & "\n" &
  "       grant with: perm grant " & permName

proc grantSummary*(): string =
  var granted, withheld: seq[string]
  for i in 0 ..< PermCount:
    if (grantedMask and permMask(i)) != 0: granted.add PermNames[i]
    else: withheld.add PermNames[i]
  result = "granted:  " & (if granted.len == 0: "(none)" else: granted.join(" ")) & "\n" &
           "withheld: " & (if withheld.len == 0: "(none)" else: withheld.join(" "))

proc updateGrant*(name: string, grant: bool): string =
  when defined(irisNative):
    let id = permFromName(name)
    if id < 0: return "unknown permission: " & name
    if grant: grantedMask = grantedMask or permMask(id)
    else: grantedMask = grantedMask and not permMask(id)
    permissionStore(grantedMask)
    result = "updated grant manifest:"
    for i in 0 ..< PermCount:
      if (grantedMask and permMask(i)) != 0: result.add " " & PermNames[i]
  else:
    var newMask: uint64 = 0
    let msg = irisGluePermUpdate(name.cstring, (if grant: 1 else: 0), addr newMask)
    grantedMask = newMask
    result = if msg != nil: $msg else: "permission update failed"

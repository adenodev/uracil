## net — the gated network client.
##
## One HTTP request through the kernel's network surface. The wasm bridge
## writes a line-based envelope into the response buffer:
##
##   <status> <ok|http-error>\n<raw body …>
##   ERR <message>\n
##
## so no JSON machinery (and no exceptions) are needed on the standalone
## target. The command layer checks net.client before calling in here.

import std/strutils
import uplatform

type NetResult* = object
  ok*: bool
  status*: int
  body*: string
  error*: string

proc parseLeadingInt(s: string): int =
  result = -1
  for c in s:
    if c in {'0'..'9'}:
      if result < 0: result = 0
      result = result * 10 + (ord(c) - ord('0'))
    else:
      break

proc httpGet*(url: string): NetResult =
  var raw: string
  if not platformNet("GET", url, "{}", "", raw):
    result.error = "network request failed (platform refused or offline)"
    return
  let nl = raw.find('\n')
  if nl < 0:
    result.error = "malformed bridge response"
    return
  let head = raw[0 ..< nl]
  result.body = raw[nl + 1 .. ^1]
  if head.startsWith("ERR"):
    result.error = head[4 .. ^1].strip()
    return
  let sp = head.find(' ')
  if sp <= 0:
    result.error = "malformed bridge status line"
    return
  result.status = parseLeadingInt(head[0 ..< sp])
  result.ok = head[sp + 1 .. ^1].strip() == "ok"

proc previewBody*(body: string, limit: int): string =
  var text = body
  if text.len > limit: text = text[0 ..< limit] & "\n... (" & $(body.len - limit) & " more bytes)"
  result = text

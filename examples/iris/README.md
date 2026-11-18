# iris — capability shell for the Hermes enclave

A tiny workable shell written in **Nim**, booted on the **Uracil** kernel.
One source, two builds:

- **wasm32 (the demo):** compiled to C and statically linked into the Uracil
  kernel wasm build (`app/iris_bundle.c` glue); the Hermes section of
  adeno.ltd boots it in the browser. Grants and files persist per visitor in
  IndexedDB (IDBFS mounted at `/urashell`).
- **native (local twin):** `nim c -d:release -d:irisNative` produces a
  standalone REPL with the same commands and permission semantics.

## Build

```bash
bash uracil/build_wasm.sh        # wasm: kernel + iris → adeno-site/app/public/uracil/
# local twin:
nim c -d:release -d:irisNative --threads:off -o:build/iris_native iris.nim
```

Smoke test (drives the wasm module through `ccall`, including the suspend
path for gated fetches):

```bash
node smoke.js [path/to/uracil.js]
```

## Capabilities & permissions

Requested: `fs.read` · `fs.write` · `net.client` · `clock.read`.
Every capability call checks the kernel grant mask
(`ur_permission_mask_contains`); denials use the stub's wording:

```
iris: refusing to run 'net get /…'; missing permissions: net.client
      grant with: perm grant net.client
```

```
perm                  show grants (real ur_runtime_print_permissions output)
perm grant <perm>     grant (persisted via ur_permission_save_grants)
perm revoke <perm>    revoke
```

## Commands

`ls` · `cat` · `write` · `rm` · `mkdir` (fs) · `net get <url>` (gated fetch) ·
`date` (clock) · `describe` (kernel bundle descriptor) · `ps` · `campaign`
(real campaign kernel) · `uname` · `whoami` · `banner` · `clear` ·
`tui files|monitor|browser|doom`

**TUI apps** — files browser, a text-mode
web browser (fetches through the gated network), and **the real Doom engine
as a `.ura` service binary**. The engine is compiled *for* Uracil: the guest
SDK (`uracil/sdk/`) bridges libc onto the kernel platform interface, so the
module imports only `uracil.*` (no wasi, no JS host); `ura_builder.py`
packages it as `doom-engine-1.9.0.wasm32.ura` (module + payload with
`.ura.meta`, the WAD and the manifest + trailer). `tui doom` downloads it
through the gated net, stages it as a campaign artifact, and the kernel
verifies the container, parses the meta into per-function requirements
(`req doom_start fs.read`, `req doom_tick clock.read`), extracts the payload
into `URACIL_SERVICE_DIR`, and services every platform call the engine makes
under its grant mask. The 320×200 framebuffer becomes ANSI truecolor
half-blocks (80×28 cells). Keys: arrows move/turn, `ctrl` fire, `space` use,
`shift` run, `q` quits.

The full run is gated: the download needs `net.client`, and the engine's
declared function requirements (`fs.read`, `clock.read`) must be granted —
the refusal prints the exact one-line `perm grant …` fix. Assets live at
`adeno-site/app/public/doom/` (`README.txt` there has provenance; engine
license ships alongside as `LICENSE-GPL-2.0.txt`).

**Campaign kernel** — `campaign [stage|load|complete|destroy]` drives a real
campaign: `stage` boots it, writes a service bundle manifest into its
artifacts directory (`/urashell/campaigns/<id>/…`, IDBFS-backed), attaches the
artifact and transitions bootstrap → running; `load` runs the kernel's bundle
loader over the staged directory and prints what the kernel sees; `complete`
cleans artifacts and closes the phase; `destroy` removes the state. The bundle
staged in the demo is the Kairos enclave's real manifest shape
(`ai.adeno.kairos-enclave` 0.3.0) — the first dry run of holding the enclave
as a Uracil service. Its declared capabilities (`storage.indexeddb`,
`crypto.subtle`, `network.fetch`, `worker.self`) have no Uracil equivalents
yet and the loader drops unknown names silently, so `campaign load` prints the
gap explicitly — see `IRIS.md` for the findings.

## Layout

```
iris.nim        dispatcher + exports (iris_boot/exec/key/screen/…)
uplatform.nim   kernel platform + permission ABI bindings (wasm + native backends)
perms.nim       grant-mask mirror + refusal text
vfs.nim         sandboxed filesystem at <state>/fs
net.nim         line-envelope HTTP client (no JSON parser needed on wasm)
tui.nim         screen grid + files/monitor/browser apps + doom frame plumbing
banner.nim      tetra mark
panicoverride.nim  required by --os:standalone
doomgeneric/    vendored Doom engine (GPL-2.0); platform layer in uracil/app/dg_iris.c
                (built as the separate, downloaded, GPL-2.0 doom.js/doom.wasm module)
```

## Notes

- The wasm build uses `--os:standalone --cpu:wasm32 --mm:arc -d:useMalloc
  --exceptions:goto --noMain:on`; try/except is avoided in the wasm path
  (the standalone runtime does not provide the borrow-exception RTL).
- TUI frames are JSON (`screen()`); the page renders `lines` + `status` and
  forwards key codes: 1..7 = up/down/left/right/enter/esc/backspace, other
  values are Unicode code points.

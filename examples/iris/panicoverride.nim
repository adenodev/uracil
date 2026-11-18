# Standalone/wasm builds require a panicoverride module (Nim embedded targets).
proc rawoutput(s: string) = discard
proc panic(s: string) {.noreturn.} = discard

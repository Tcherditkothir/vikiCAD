#pragma once

namespace viki {

// Drop OCCT's default console printer, once per process.
//
// OCCT reports transfer statistics and parse errors through its global
// messenger, which prints to STDOUT. The CLI writes JSON to stdout, so a single
// OCCT complaint -- "premature end of file" on a truncated STL, transfer stats
// on a STEP read -- turns a valid reply into unparsable output. Every importer
// and exporter that can trip OCCT diagnostics calls this first.
//
// Idempotent and safe to call from anywhere; the removal happens on the first
// call and later calls do nothing.
void silenceOcctMessages();

} // namespace viki

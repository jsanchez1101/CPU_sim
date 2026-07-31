#!/usr/bin/env node
/*
 * verify_trace.js -- checks that a recorded trace actually reconstructs the run.
 *
 *   node viewer/verify_trace.js run.json [expected.json]
 *
 * A trace is only useful if replaying its deltas lands on the same state the
 * simulator finished in. This replays the trace with the same engine the viewer
 * uses (viewer/replay.js) and asserts:
 *
 *   - every record is well formed and cycles increase monotonically
 *   - the counters accumulated from per-cycle deltas equal the `totals` block
 *     the C++ wrote out independently at the end of the run
 *   - seeking backwards via keyframes produces byte-identical state to replaying
 *     forwards from zero (this is what the scrubber does)
 *   - structural invariants: no stage references an unknown PC, no TLB entry
 *     points at a frame outside physical memory, no page maps to a bad frame
 *
 * Exits non-zero on the first failure, so it can be wired into `make test`.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const Replay = require(path.join(__dirname, 'replay.js'));

let checks = 0;
let failures = 0;

function check(cond, what, detail) {
  checks++;
  if (cond) return;
  failures++;
  console.log('[ FAIL ] ' + what + (detail ? '  ' + detail : ''));
}

function eq(actual, expected, what) {
  check(actual === expected, what, '(got ' + actual + ', want ' + expected + ')');
}

const file = process.argv[2];
if (!file) {
  console.error('usage: node verify_trace.js run.json');
  process.exit(2);
}
const T = JSON.parse(fs.readFileSync(file, 'utf8'));
console.log('[ run  ] ' + path.basename(file) + ': ' + T.cycles.length + ' records, ' +
            T.processes.length + ' process(es)');

/* ---- structure ---------------------------------------------------------- */

check(T.version === 1, 'trace version is 1');
check(Array.isArray(T.cycles) && T.cycles.length > 0, 'trace has cycles');
check(Array.isArray(T.processes) && T.processes.length > 0, 'trace has processes');
check(T.geometry && T.geometry.pageSize > 0, 'trace has geometry');

const knownPc = new Set();
for (const p of T.processes) {
  for (const pc of Object.keys(p.code)) knownPc.add(p.pid + ':' + pc);
}
const pids = new Set(T.processes.map(p => p.pid));

let lastCycle = -1;
let badStage = 0, badOrder = 0, badPid = 0;
for (const r of T.cycles) {
  if (!(r.c > lastCycle)) badOrder++;
  lastCycle = r.c;
  if (!pids.has(r.p)) badPid++;
  for (const pc of r.s) {
    if (pc === null || pc === undefined) continue;
    if (!knownPc.has(r.p + ':' + pc)) badStage++;
  }
}
eq(badOrder, 0, 'cycle numbers strictly increase');
eq(badPid, 0, 'every record names a known process');
eq(badStage, 0, 'every stage PC resolves to a disassembled instruction');

/* ---- replay ------------------------------------------------------------- */

const keyframes = Replay.buildKeyframes(T, 500);
const fwd = { state: Replay.createState(T), idx: -1, keyframes };
Replay.seekTo(T, fwd, T.cycles.length - 1);

const replayed = Replay.totals(fwd.state);
const declared = T.totals;

// `cycles` in the totals block is wall-clock and includes context-switch
// overhead the core never saw, so it is compared separately below.
const skip = new Set(['cycles', 'contextSwitches', 'contextSwitchCycles']);
let mismatched = [];
for (const [k, v] of Object.entries(declared)) {
  const key = k;
  if (skip.has(key)) continue;
  const got = replayed[key] || 0;
  // A truncated trace holds a prefix of the run while `totals` describes all of
  // it, so the deltas can only be expected to account for part of each counter.
  if (T.truncated ? got > v : got !== v) {
    mismatched.push(key + ': replay ' + got + ' vs totals ' + v);
  }
}
check(mismatched.length === 0,
      T.truncated ? 'per-cycle deltas stay within the declared totals (truncated trace)'
                  : 'per-cycle deltas sum to the declared totals',
      mismatched.slice(0, 6).join('; '));

check(T.cycles[T.cycles.length - 1].c <= declared.cycles,
      'last recorded cycle is within the wall-clock count');

/* ---- keyframe seeking matches forward replay ---------------------------- */

function fingerprint(s) {
  const parts = [];
  for (const pid of Object.keys(s.procs).sort()) {
    const p = s.procs[pid];
    parts.push(pid + '|' + p.regs.join(',') + '|' +
      [...p.pt.entries()].sort((a, b) => a[0] - b[0])
        .map(([v, e]) => v + ':' + e.frame + ':' + e.valid + ':' + e.dirty).join(' ') + '|' +
      Object.keys(p.stats).sort().map(k => k + '=' + p.stats[k]).join(','));
  }
  parts.push(s.icache.map(l => l.valid + '/' + l.dirty + '/' + l.tag).join(' '));
  parts.push(s.dcache.map(l => l.valid + '/' + l.dirty + '/' + l.tag).join(' '));
  parts.push(s.tlb.map(e => e.valid + '/' + e.pid + '/' + e.vpn + '/' + e.frame).join(' '));
  return parts.join('#');
}

const probes = [];
const n = T.cycles.length;
for (const f of [0, 0.13, 0.37, 0.5, 0.76, 0.99, 1]) {
  probes.push(Math.min(n - 1, Math.floor(f * (n - 1))));
}

let seekMismatch = 0;
for (const target of probes) {
  const clean = { state: Replay.createState(T), idx: -1, keyframes };
  Replay.seekTo(T, clean, target);
  // Same target, but arrived at by seeking backwards from the end.
  const back = { state: Replay.createState(T), idx: -1, keyframes };
  Replay.seekTo(T, back, n - 1);
  Replay.seekTo(T, back, target);
  if (fingerprint(clean.state) !== fingerprint(back.state)) seekMismatch++;
}
eq(seekMismatch, 0, 'backward seek via keyframes matches forward replay');

/* ---- physical invariants ------------------------------------------------ */

const frames = T.geometry.frames;
let badFrame = 0, badTlbFrame = 0;
const finalState = fwd.state;
for (const p of Object.values(finalState.procs)) {
  for (const e of p.pt.values()) {
    if (e.valid && e.frame >= frames) badFrame++;
  }
}
for (const e of finalState.tlb) {
  if (e.valid && e.frame >= frames) badTlbFrame++;
}
eq(badFrame, 0, 'every resident page maps to a real frame');
eq(badTlbFrame, 0, 'every valid TLB entry maps to a real frame');

let resident = 0;
for (const p of Object.values(finalState.procs)) {
  for (const e of p.pt.values()) if (e.valid) resident++;
}
check(resident <= frames, 'resident pages never exceed physical frames',
      '(' + resident + ' resident, ' + frames + ' frames)');

/* ---- ground truth: replayed state vs. what the simulator actually held --- */

// `finalRegs` is written directly from the Process objects at the end of the
// run, independently of the delta stream. If replaying every delta does not
// reproduce it exactly, the trace is lying about something.
check(Array.isArray(T.finalRegs) && T.finalRegs.length > 0, 'trace carries final register state');
let regMismatch = [];
for (const entry of T.finalRegs || []) {
  const got = finalState.procs[entry.pid];
  if (!got) { regMismatch.push('pid ' + entry.pid + ' missing'); continue; }
  for (let i = 0; i < entry.regs.length; i++) {
    if ((got.regs[i] >>> 0) !== (entry.regs[i] >>> 0)) {
      regMismatch.push('pid ' + entry.pid + ' r' + i + ': replay ' + (got.regs[i] >>> 0) +
                       ' vs actual ' + (entry.regs[i] >>> 0));
    }
  }
}
check(regMismatch.length === 0, 'replayed registers match the simulator final state',
      regMismatch.slice(0, 5).join('; '));

console.log(checks + ' checks, ' + failures + ' failures');
process.exit(failures === 0 ? 0 : 1);

/*
 * replay.js -- reconstructs machine state from a cpusim JSON trace.
 *
 * A trace stores one record per cycle containing only what changed, so the
 * state at cycle N is the result of applying records 0..N in order. Seeking
 * backwards replays from the nearest keyframe.
 *
 * This file is shared by the browser viewer (viewer/index.html) and the Node
 * verifier (viewer/verify_trace.js) so there is exactly one implementation of
 * the delta semantics to get wrong.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.Replay = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const NUM_REGS = 16;

  function createState(trace) {
    const g = trace.geometry;
    const mk = n => Array.from({ length: n }, () => ({ valid: 0, dirty: 0, tag: 0 }));
    const st = {
      procs: {},
      icache: mk(g.icacheSets * g.icacheWays),
      dcache: mk(g.dcacheSets * g.dcacheWays),
      tlb: Array.from({ length: g.tlbEntries }, () => ({ valid: 0, pid: 0, vpn: 0, frame: 0 })),
      touched: emptyTouched(),
      rec: null,
    };
    for (const p of trace.processes) {
      st.procs[p.pid] = { regs: new Array(NUM_REGS).fill(0), pt: new Map(), stats: {} };
    }
    return st;
  }

  function emptyTouched() {
    return { regs: new Set(), icache: new Set(), dcache: new Set(), tlb: new Set(), pt: new Set() };
  }

  function cloneState(s) {
    return {
      procs: Object.fromEntries(Object.entries(s.procs).map(([k, v]) => [k, {
        regs: v.regs.slice(),
        pt: new Map([...v.pt].map(([kk, vv]) => [kk, { ...vv }])),
        stats: Object.assign({}, v.stats),
      }])),
      icache: s.icache.map(l => ({ ...l })),
      dcache: s.dcache.map(l => ({ ...l })),
      tlb: s.tlb.map(e => ({ ...e })),
      touched: emptyTouched(),
      rec: s.rec,
    };
  }

  function applyRecord(trace, s, rec) {
    const g = trace.geometry;
    s.touched = emptyTouched();
    s.rec = rec;

    for (const d of rec.r || []) {
      const [pid, reg, value] = d;
      const p = s.procs[pid];
      if (!p) continue;
      p.regs[reg] = value >>> 0;
      if (pid === rec.p) s.touched.regs.add(reg);
    }
    for (const d of rec.d || []) {
      const [pid, name, value] = d;
      const p = s.procs[pid];
      if (!p) continue;
      p.stats[name] = (p.stats[name] || 0) + value;
    }
    for (const d of rec.L || []) {
      const [kind, index, way, tag, valid, dirty] = d;
      const ways = kind === 0 ? g.icacheWays : g.dcacheWays;
      const arr = kind === 0 ? s.icache : s.dcache;
      const flat = index * ways + way;
      if (!arr[flat]) continue;
      arr[flat] = { valid, dirty, tag };
      (kind === 0 ? s.touched.icache : s.touched.dcache).add(flat);
    }
    for (const d of rec.T || []) {
      const [slot, pid, vpn, frame, valid] = d;
      if (!s.tlb[slot]) continue;
      s.tlb[slot] = { valid, pid, vpn, frame };
      s.touched.tlb.add(slot);
    }
    for (const d of rec.P || []) {
      const [pid, vpn, frame, valid, dirty] = d;
      const p = s.procs[pid];
      if (!p) continue;
      p.pt.set(vpn, { frame, valid, dirty });
      s.touched.pt.add(pid + ':' + vpn);
    }
    return s;
  }

  function buildKeyframes(trace, every) {
    every = every || 500;
    const frames = [];
    const s = createState(trace);
    frames.push({ idx: -1, state: cloneState(s) });
    for (let i = 0; i < trace.cycles.length; i++) {
      applyRecord(trace, s, trace.cycles[i]);
      if ((i + 1) % every === 0) frames.push({ idx: i, state: cloneState(s) });
    }
    return frames;
  }

  // ctx = { state, idx, keyframes }. Mutates ctx in place.
  function seekTo(trace, ctx, target) {
    target = Math.max(0, Math.min(trace.cycles.length - 1, target));
    if (target < ctx.idx) {
      let kf = ctx.keyframes[0];
      for (const k of ctx.keyframes) {
        if (k.idx <= target) kf = k; else break;
      }
      ctx.state = cloneState(kf.state);
      ctx.idx = kf.idx;
      if (ctx.idx >= 0) ctx.state.rec = trace.cycles[ctx.idx];
    }
    while (ctx.idx < target) {
      ctx.idx++;
      applyRecord(trace, ctx.state, trace.cycles[ctx.idx]);
    }
    return ctx;
  }

  // Sums per-process counters into one object, the way the CLI report does.
  function totals(state) {
    const out = {};
    for (const p of Object.values(state.procs)) {
      for (const [k, v] of Object.entries(p.stats)) out[k] = (out[k] || 0) + v;
    }
    return out;
  }

  return { createState, cloneState, applyRecord, buildKeyframes, seekTo, totals, NUM_REGS };
}));

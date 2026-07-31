#!/usr/bin/env node
/*
 * test_viewer.js -- renders viewer/index.html in jsdom and checks it works.
 *
 *   node viewer/test_viewer.js [trace.json]
 *
 * The viewer is real code with real bugs available to it, so it gets tested
 * like the rest of the project rather than eyeballed. This loads the actual
 * page (not a copy), drives the controls, and asserts on the rendered DOM:
 *
 *   - the page boots, loads a trace, and throws no errors
 *   - the five stage boxes show the instruction the trace says is in them
 *   - stepping forward then back lands on identical DOM
 *   - the scrubber, "run until" breakpoints and process switcher work
 *   - registers, cache, TLB and page table tables populate and update
 *
 * jsdom is a devDependency only; skip this file if it is not installed.
 */
'use strict';

const fs = require('fs');
const path = require('path');

let JSDOM;
try {
  ({ JSDOM } = require('jsdom'));
} catch (e) {
  console.log('[ skip ] jsdom not installed (npm i -D jsdom) -- viewer DOM test skipped');
  process.exit(0);
}

const here = __dirname;
let checks = 0, failures = 0;
function check(cond, what, detail) {
  checks++;
  if (cond) return;
  failures++;
  console.log('[ FAIL ] ' + what + (detail ? '  ' + detail : ''));
}
function eq(a, b, what) { check(a === b, what, '(got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b) + ')'); }

const tracePath = process.argv[2];
let trace = null;
if (tracePath) {
  trace = JSON.parse(fs.readFileSync(tracePath, 'utf8'));
} else {
  const demo = path.join(here, 'demo-trace.js');
  if (!fs.existsSync(demo)) {
    console.log('[ skip ] no trace given and viewer/demo-trace.js absent (run `make traces`)');
    process.exit(0);
  }
  const g = {};
  new Function('window', fs.readFileSync(demo, 'utf8')).call(null, g);
  trace = g.CPUSIM_TRACE;
}

const html = fs.readFileSync(path.join(here, 'index.html'), 'utf8');
const errors = [];

const dom = new JSDOM(html, {
  runScripts: 'dangerously',
  url: 'file://' + path.join(here, 'index.html'),
  beforeParse(window) {
    // jsdom will not resolve the sibling <script src>, so inject them by hand.
    window.__inject = true;
    window.addEventListener('error', e => errors.push(String(e.error || e.message)));
  },
});

const { window } = dom;
window.onerror = (m) => errors.push(String(m));

// Evaluate the two sibling scripts in page context, then the inline page code
// has already run against the stubs the page defines.
function run(file) {
  window.eval(fs.readFileSync(path.join(here, file), 'utf8'));
}

try {
  run('replay.js');
} catch (e) {
  console.log('[ FAIL ] replay.js threw: ' + e.message);
  process.exit(1);
}

console.log('[ run  ] viewer/index.html in jsdom (' + trace.cycles.length + ' cycles)');

const doc = window.document;
check(!!doc.getElementById('app'), 'page has an app root');
check(typeof window.Replay === 'object', 'replay engine is exposed');

// The inline script ran at parse time; call its loader with our trace.
check(typeof window.load === 'function', 'page exposes load()');
if (typeof window.load !== 'function') {
  console.log(checks + ' checks, ' + (failures + 1) + ' failures');
  process.exit(1);
}
window.load(trace);

check(!doc.getElementById('app').classList.contains('hide'), 'app becomes visible after load');
check(doc.getElementById('empty').classList.contains('hide'), 'drop zone hides after load');

/* ---- stage boxes agree with the trace ----------------------------------- */

function stageTexts() {
  return [0, 1, 2, 3, 4].map(i => {
    const el = doc.getElementById('st' + i);
    const ins = el.querySelector('.ins');
    return ins ? ins.textContent.trim() : '';
  });
}

function codeOf(pid, pc) {
  const p = trace.processes.find(x => x.pid === pid);
  return p && p.code[String(pc)] ? p.code[String(pc)] : null;
}

// Find a cycle with a busy pipeline so the comparison is meaningful.
let busyIdx = trace.cycles.findIndex(r => r.s.filter(x => x !== null).length >= 3);
if (busyIdx < 0) busyIdx = Math.floor(trace.cycles.length / 2);
window.seek(busyIdx);

const rec = trace.cycles[busyIdx];
const want = [rec.pc, rec.s[0], rec.s[1], rec.s[2], rec.s[3]]
  .map(pc => (pc === null || pc === undefined) ? 'bubble' : (codeOf(rec.p, pc) || '?'));
const got = stageTexts();
for (let i = 0; i < 5; i++) {
  eq(got[i], want[i], 'stage ' + ['IF', 'ID', 'EX', 'MEM', 'WB'][i] + ' shows the right instruction');
}
eq(doc.getElementById('cycNum').textContent, String(rec.c), 'cycle number displayed');

/* ---- panels populate ---------------------------------------------------- */

eq(doc.querySelectorAll('#regs .reg').length, 16, 'sixteen registers rendered');
check(doc.querySelectorAll('#dcache tr').length > 1, 'D-cache table populated');
check(doc.querySelectorAll('#icache tr').length > 1, 'I-cache table populated');
eq(doc.querySelectorAll('#tlb tr').length, trace.geometry.tlbEntries + 1, 'TLB row per entry plus header');
check(doc.querySelectorAll('#ptable tr').length > 1, 'page table populated');
check(doc.getElementById('stats').textContent.includes('CPI'), 'counters panel shows CPI');
check(doc.getElementById('chips').children.length === trace.config.length, 'config chips rendered');
check(doc.getElementById('code').querySelectorAll('tr').length > 0, 'code listing rendered');

// The code listing should highlight exactly the stages that hold a known PC.
const highlighted = doc.getElementById('code').querySelectorAll('tr[class^="hl-"]');
const distinctPcs = new Set([rec.pc, ...rec.s].filter(x => x !== null && x !== undefined)
                            .filter(pc => codeOf(rec.p, pc)));
eq(highlighted.length, distinctPcs.size, 'code listing highlights each active PC');

/* ---- navigation --------------------------------------------------------- */

const snapshotDom = () => JSON.stringify([
  stageTexts(),
  doc.getElementById('cycNum').textContent,
  doc.getElementById('regs').textContent,
  doc.getElementById('dcache').textContent,
  doc.getElementById('tlb').textContent,
  doc.getElementById('ptable').textContent,
]);

const before = snapshotDom();
window.step(1);
check(snapshotDom() !== before || trace.cycles.length < 3, 'stepping forward changes the view');
window.step(-1);
eq(snapshotDom(), before, 'step forward then back returns to identical DOM');

// Seek far ahead then back to the same point: exercises keyframe rewind.
window.seek(trace.cycles.length - 1);
const atEnd = doc.getElementById('cycNum').textContent;
eq(atEnd, String(trace.cycles[trace.cycles.length - 1].c), 'jump to end lands on the last cycle');
window.seek(busyIdx);
eq(snapshotDom(), before, 'rewinding from the end reproduces the same DOM');

// Final register display must match the simulator's recorded ground truth.
window.seek(trace.cycles.length - 1);
if (trace.finalRegs && trace.finalRegs.length) {
  const lastPid = trace.cycles[trace.cycles.length - 1].p;
  const truth = trace.finalRegs.find(f => f.pid === lastPid);
  const shown = [...doc.querySelectorAll('#regs .reg')].map(d => {
    const v = Number(d.querySelector('.v').textContent);
    return v < 0 ? v + 0x100000000 : v;
  });
  let bad = 0;
  for (let i = 0; i < 16; i++) if (shown[i] !== (truth.regs[i] >>> 0)) bad++;
  eq(bad, 0, 'final register display matches simulator ground truth');
}

/* ---- controls ----------------------------------------------------------- */

const scrub = doc.getElementById('scrub');
eq(Number(scrub.max), trace.cycles.length - 1, 'scrubber range covers every record');
scrub.value = String(Math.floor(trace.cycles.length / 3));
scrub.dispatchEvent(new window.Event('input'));
eq(doc.getElementById('cycNum').textContent,
   String(trace.cycles[Math.floor(trace.cycles.length / 3)].c), 'scrubbing seeks to that cycle');

// "run until" should land on a cycle that actually has the requested event.
window.seek(0);
const kinds = ['pageFaults', 'dcacheMisses', 'mispredicts', 'stallLoadUse'];
for (const kind of kinds) {
  const exists = trace.cycles.some(r => (r.d || []).some(([, n]) => n === kind));
  if (!exists) continue;
  window.seek(0);
  doc.getElementById('bpKind').value = kind;
  doc.getElementById('btnRunTo').click();
  const at = Number(doc.getElementById('cycNum').textContent);
  const landed = trace.cycles.find(r => r.c === at);
  check(landed && (landed.d || []).some(([, n]) => n === kind),
        'run-until ' + kind + ' lands on a matching cycle', 'landed at cycle ' + at);
}

// The two stall-skipping modes: from inside a long stall they must jump past
// it, not step one cycle. This is the fix for "the demo is frozen for 400
// cycles and there is no way through it".
const stallStart = trace.cycles.findIndex((r, i) =>
  r.n && i + 40 < trace.cycles.length && trace.cycles[i + 40].n);
if (stallStart >= 0) {
  const matcher = {
    __retire: r => (r.d || []).some(([, n]) => n === 'instructions'),
    __running: r => !r.n,
  };
  for (const [mode, want] of [['__retire', 'retires'], ['__running', 'runs']]) {
    // A short trace can be entirely stalled (a cold start that never gets to
    // run), in which case staying put and reporting no match is correct.
    const exists = trace.cycles.slice(stallStart + 1).some(matcher[mode]);
    if (!exists) continue;
    window.seek(stallStart);
    doc.getElementById('bpKind').value = mode;
    doc.getElementById('btnRunTo').click();
    const at = Number(doc.getElementById('cycNum').textContent);
    const landed = trace.cycles.find(r => r.c === at);
    check(landed && matcher[mode](landed),
          'run-until "' + want + '" escapes a stall to a valid cycle',
          'landed at cycle ' + at);
    check(at > Number(trace.cycles[stallStart].c), 'run-until "' + want + '" moves forward');
  }
}

// The activity strip classifies every cycle; the legend must reflect what is
// actually in the trace.
check(doc.getElementById('stripLegend').children.length > 0, 'activity strip legend rendered');
const marker = doc.getElementById('stripMark');
window.seek(0);
const leftAtStart = marker.style.left;
window.seek(trace.cycles.length - 1);
check(marker.style.left !== leftAtStart, 'activity strip marker tracks position');
check(parseFloat(marker.style.left) === 100, 'marker reaches the end of the strip');

// Process switcher, when the trace has more than one process.
if (trace.processes.length > 1) {
  const sel = doc.getElementById('procSel');
  eq(sel.options.length, trace.processes.length, 'one option per process');
  sel.value = String(trace.processes[1].pid);
  sel.dispatchEvent(new window.Event('change'));
  check(doc.getElementById('code').querySelectorAll('tr').length ===
        Object.keys(trace.processes[1].code).length,
        'switching process reloads its code listing');
}

// Playback start/stop must not leave a timer running.
doc.getElementById('btnPlay').click();
doc.getElementById('btnPlay').click();
check(doc.getElementById('btnPlay').textContent.includes('Play'), 'pause restores the play label');

eq(errors.length, 0, 'no uncaught errors while driving the page', errors.slice(0, 3).join(' | '));

console.log(checks + ' checks, ' + failures + ' failures');
process.exit(failures === 0 ? 0 : 1);

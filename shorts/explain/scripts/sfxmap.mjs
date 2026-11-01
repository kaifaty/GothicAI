#!/usr/bin/env node
// The sound map of a plan: every sound of the motion (src/parts/sfxMap.ts) with its time, kind, beat and what it
// belongs to, then what the thinning left out and the count per kind.
//
//     node scripts/sfxmap.mjs "$W/plan.json"
import {readFileSync} from 'node:fs';
import {Plan} from '../src/schema.ts';
import {hits, KINDS, sounds} from '../src/parts/sfxMap.ts';

const plan = Plan.parse(JSON.parse(readFileSync(process.argv[2], 'utf8')));
const kept = sounds(plan);
const all = hits(plan);
const length = plan.beats.reduce((s, b) => s + b.dur, 0);
for (const s of kept) {
  console.log(`${s.t.toFixed(2).padStart(6)}  ${s.kind.padEnd(6)}  ${s.beat.padEnd(12)}  ${s.what}  (${s.file})`);
}
const left = all.filter((h) => !kept.some((s) => s.t === h.t && s.kind === h.kind && s.what === h.what));
if (left.length) {
  console.log(`\nleft out, too close to a stronger one (${left.length}):`);
  for (const h of left) console.log(`${h.t.toFixed(2).padStart(6)}  ${h.kind.padEnd(6)}  ${h.beat.padEnd(12)}  ${h.what}`);
}
const count = Object.keys(KINDS).map((k) => [k, kept.filter((s) => s.kind === k).length]).filter(([, n]) => n);
const ticks = kept.filter((s) => s.kind === 'tick').length;
console.log(`\n${kept.length} sounds in ${length.toFixed(1)} s (${((kept.length - ticks) / length * 60).toFixed(0)} a minute without the ticks): ` +
  count.map(([k, n]) => `${k} ${n}`).join(', '));

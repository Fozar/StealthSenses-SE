// Run: node --test tests/ReplayTests.cjs. No browser or npm packages required.
// Executes the actual standalone replay script with a minimal DOM/canvas host.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../tools/replay.html'), 'utf8');

test('NPC scent diagnostics show channel, segment, profile and only past decisions', () => {
  const r = replay();
  r.parse([
    { type: 'npc', t: 1, id: 'A', name: 'wolf<', pos: [0, 0, 0], meter: 0, pkg: 'P',
      prof: { name: 'wolf', smell: 1.6, smellBlood: 1, smellHunt: true },
      track: { seq: 5, channel: 'scent', segment: 3, mode: 'follow', target: [400, 0, 0], trail: [400, 0, 0] } },
    { type: 'scent_notice', t: 1, id: 'A', stage: 'continue', picked: 6, pickedSegment: 3, perceived: 0.4, reason: 'picked' },
    { type: 'scent_notice', t: 2, id: 'A', stage: 'continue', picked: 0, reason: 'barrier_or_budget' }
  ]);
  r.run('seek(1)');
  const row = r.element('#npcs tbody').innerHTML;
  assert.match(row, /запах/);
  assert.match(row, /участок 3/);
  assert.match(row, /нюх 1\.60/);
  assert.match(row, /#6/);
  assert.match(row, /wolf&lt;/);
  assert.doesNotMatch(row, /преграда/);
  r.run('seek(2)');
  assert.match(r.element('#npcs tbody').innerHTML, /преграда/);
});

test('legacy NPC tracking is rendered as visual without invented scent data', () => {
  const r = replay();
  r.parse([{ type: 'npc', t: 1, id: 'A', name: 'guard', pos: [0, 0, 0], meter: 0, pkg: 'P',
    track: { seq: 4, mode: 'follow', target: [400, 0, 0], trail: [400, 0, 0] } }]);
  r.run('seek(1)');
  assert.match(r.element('#npcs tbody').innerHTML, /зрение/);
  assert.doesNotMatch(r.element('#npcs tbody').innerHTML, /нюх /);
});

test('candidate diagnostics and committed targets are distinguishable in the replay', () => {
  const r = replay();
  r.parse([
    { type: 'npc', t: 1, id: 'A', name: 'wolf', pos: [0, 0, 0], meter: 0, pkg: 'P' },
    { type: 'scent_notice', t: 1, id: 'A', stage: 'continue', picked: 6, pickedSegment: 3, perceived: 0.4, reason: 'picked' },
    { type: 'track_target', t: 2, id: 'A', seq: 6, channel: 'scent', segment: 3 }
  ]);
  r.run('seek(1)');
  assert.match(r.element('#npcs tbody').innerHTML, /доступен источник/);
  assert.doesNotMatch(r.element('#npcs tbody').innerHTML, /подхват/);
  r.run('seek(2)');
  assert.match(r.element('log').innerHTML, /Цель.*запах.*#6.*участок 3/);
});

function replay() {
  const elements = new Map();
  const canvas = new Proxy({}, { get: (o, key) => o[key] ?? (() => {}), set: (o, k, v) => (o[k] = v, true) });
  const element = id => {
    if (!elements.has(id)) elements.set(id, {
      textContent: '', innerHTML: '', value: '0', checked: true,
      getBoundingClientRect: () => ({ width: 1000, height: 700 }), getContext: () => canvas
    });
    return elements.get(id);
  };
  const context = vm.createContext({
    document: { getElementById: element, querySelector: element, querySelectorAll: () => [], documentElement: {} },
    window: {}, devicePixelRatio: 1, getComputedStyle: () => ({ getPropertyValue: () => '#000' }),
    performance: { now: () => 0 }, requestAnimationFrame: () => {}
  });
  vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
  return {
    run: source => vm.runInContext(source, context), element,
    parse: records => vm.runInContext(`parse(${JSON.stringify(records.map(r => JSON.stringify(r)).join('\n'))})`, context)
  };
}

const wind = (changes = {}) => ({
  type: 'wind', v: 1, t: 10, space: '0000003C', pos: [0, 0, 0], exterior: true,
  intervalMs: 1000, sky: true, angle: 1.2, speed: 0.5, transition: 0.75,
  current: { id: '00012345', speedByte: 128, directionByte: 192, rangeByte: 64 },
  previous: { id: '00012344', speedByte: 64, directionByte: 0, rangeByte: 0 },
  trees: true, treeAnims: true, treeVector: [3, 4], treeMagnitude: 0.5, ...changes
});

test('historical selection never uses a future sample and expires stale wind', () => {
  const r = replay();
  r.parse([wind(), wind({ t: 20, angle: Math.PI })]);
  assert.equal(r.run('windAt(S.winds, 9, null).sample'), null);
  assert.equal(r.run('windAt(S.winds, 11, null).sample.t'), 10);
  assert.equal(r.run('windAt(S.winds, 14, null).direction'), null);
  assert.equal(r.run('windAt(S.winds, 20, null).direction[0]'), -1);
});

test('interior, wrong space, invalid Sky values and calm suppress arrows', () => {
  for (const changes of [
    { exterior: false }, { sky: false }, { angle: null }, { speed: null },
    { speed: 0 }, { speed: -0.1 }
  ]) {
    const r = replay(); r.parse([wind(changes)]);
    assert.equal(r.run('windAt(S.winds, 10, null).direction'), null, JSON.stringify(changes));
  }
  const r = replay(); r.parse([wind({ angle: 0 })]);
  assert.equal(r.run('windAt(S.winds, 10, "different").direction'), null);
  assert.equal(r.run('windAt(S.winds, 10, null).direction[0]'), 1);
  assert.equal(r.run('windAt(S.winds, 10, null).direction[1]'), 0);
});

test('Sky axes work with static or unavailable tree data, including existing v1 traces', () => {
  const r = replay();
  r.parse([wind({ angle: Math.PI / 2, trees: false, treeAnims: false, treeVector: [0, 1] })]);
  assert.ok(Math.abs(r.run('windAt(S.winds, 10, null).direction[0]')) < 1e-10);
  assert.equal(r.run('windAt(S.winds, 10, null).direction[1]'), 1);
  r.run('seek(10)');
  assert.match(r.element('windinfo').textContent, /Sky/);
});

test('slow sampling retains valid wind for two configured intervals', () => {
  const r = replay(); r.parse([wind({ intervalMs: 5000 })]);
  assert.ok(r.run('windAt(S.winds, 19, null).direction'));
  assert.equal(r.run('windAt(S.winds, 21, null).direction'), null);
});

test('replay renders unsigned weather direction and separates save sessions', () => {
  const r = replay();
  r.parse([{ type: 'load', t: 0 }, wind(), { type: 'load', t: 30 }, { type: 'mark', t: 31, n: 1 }]);
  assert.equal(r.run('S.winds.length'), 0);
  assert.match(r.element('windinfo').textContent, /нет данных ветра/);
  r.run('pick(0); seek(10)');
  assert.match(r.element('windinfo').textContent, /271\.06°/);
  assert.match(r.element('windinfo').textContent, /22\.59°/);
});

test('weather changes are visible in events and old traces still render', () => {
  const r = replay();
  r.parse([wind(), wind({ t: 11, current: { id: '000ABCDE' } })]);
  r.run('seek(11)');
  assert.match(r.element('log').innerHTML, /00012345.*000ABCDE/);
  r.parse([{ type: 'load', t: 0 }, { type: 'player', t: 1, space: '0000003C', pos: [0, 0, 0] }]);
  r.run('seek(1)');
  assert.match(r.element('windinfo').textContent, /нет данных ветра/);
});

const scentConfig = { v: 1, enabled: true, halflife_hours: 0.5, base_strength: 1, blood_bonus: 1, radius: 450, min_strength: 0.1 };
const spreadConfig = { ...scentConfig, v: 2, spread_max_mult: 1.25, spread_hours: 0.1 };
const source = (changes = {}) => ({ type: 'footprint', t: 1, gh: 10, born: 10, segment: 1,
  scentBase: 1, seq: 1, space: '0000003C', pos: [0, 0, 0], base: 0.05, blood: false, ...changes });

test('v2 dispersion expands the area, dilutes concentration and eventually loses perceptibility', () => {
  const r = replay();
  r.parse([{ type: 'load', t: 0, gh: 10, scent: spreadConfig }, source(),
    { type: 'player', t: 2, gh: 10.1, pos: [0, 0, 0], space: '0000003C' },
    { type: 'player', t: 3, gh: 11.5, pos: [0, 0, 0], space: '0000003C' }]);
  const fresh = r.run('scentSources(S, 1, null)[0]');
  assert.ok(fresh, 'v2 source exists');
  assert.equal(fresh.strength, 1);
  assert.equal(fresh.spreadRadius, 450);
  const old = r.run('scentSources(S, 2, null)[0]');
  assert.ok(Math.abs(old.spreadRadius - 521.11356) < 0.001);
  assert.ok(Math.abs(old.strength - 0.64913) < 0.001);
  assert.ok(old.radius > fresh.radius);
  const expired = r.run('scentSources(S, 3, null)[0]');
  assert.equal(expired.active, false);
  assert.equal(expired.radius, 0);
  assert.ok(expired.spreadRadius <= 562.5);
});

test('scent replay decays in game hours and shrinks its potential radius', () => {
  const r = replay();
  r.parse([{ type: 'load', t: 0, gh: 10, scent: scentConfig }, source(),
    { type: 'player', t: 2, gh: 10.5, space: '0000003C', pos: [0, 0, 0] }]);
  assert.equal(r.run('scentSources(S, 1, "0000003C")[0].strength'), 1);
  assert.equal(r.run('scentSources(S, 1, "0000003C")[0].radius'), 405);
  assert.equal(r.run('scentSources(S, 2, "0000003C")[0].strength'), 0.5);
  assert.equal(r.run('scentSources(S, 2, "0000003C")[0].radius'), 360);
  r.run('seek(2)');
  assert.match(r.element('scentinfo').textContent, /0\.50/);
});

test('scent replay respects original birth time of restored sources', () => {
  const r = replay();
  r.parse([{ type: 'load', t: 0, gh: 12, scent: scentConfig }, source({ t: 0, gh: 12, born: 10, src: 'save' })]);
  const p = r.run('scentSources(S, 0, null)[0]');
  assert.equal(p.strength, 0.0625);
  assert.equal(p.radius, 0);
  assert.equal(p.age, 2);
  assert.equal(p.active, false);
});

test('scent replay hides future, removed, legacy, disabled and other-space sources', () => {
  const r = replay();
  r.parse([{ type: 'load', t: 0, gh: 10, scent: scentConfig }, source(),
    source({ t: 2, seq: 2, segment: 0 }), { type: 'trail_removed', t: 3, gh: 10, seqs: [1], reason: 'thin' }]);
  assert.equal(r.run('scentSources(S, 0, null).length'), 0);
  assert.equal(r.run('scentSources(S, 2, "different").length'), 0);
  assert.equal(r.run('scentSources(S, 2, null).length'), 1);
  assert.equal(r.run('scentSources(S, 3, null).length'), 0);
  r.parse([{ type: 'load', t: 0, gh: 10, scent: { ...scentConfig, enabled: false } }, source()]);
  assert.equal(r.run('scentSources(S, 2, null).length'), 0);
  r.parse([{ type: 'load', t: 0, gh: 10 }, source()]);
  assert.equal(r.run('scentSources(S, 2, null).length'), 0);
});

test('scent breaks are visible and connections never bridge water or missing scent', () => {
  const r = replay();
  r.parse([{ type: 'load', t: 0, gh: 10, scent: scentConfig }, source(),
    { type: 'scent_break', t: 2, gh: 10, segment: 1, reason: 'water' }, source({ t: 3, seq: 2, segment: 2 })]);
  r.run('seek(3)');
  assert.match(r.element('log').innerHTML, /water/);
  assert.equal(r.run('scentConnected({segment:1,space:"a"},{segment:2,space:"a"})'), false);
  assert.equal(r.run('scentConnected({segment:0,space:"a"},{segment:0,space:"a"})'), false);
  assert.equal(r.run('scentConnected({segment:1,space:"a"},{segment:1,space:"b"})'), false);
  assert.equal(r.run('scentConnected({segment:1,space:"a"},{segment:1,space:"a"})'), true);
});

test('rotated traces recover the scent model from a self-describing footprint', () => {
  const r = replay();
  r.parse([{ type: 'player', t: 100, gh: 10, pos: [0, 0, 0], space: '0000003C' },
    source({ t: 101, scent: scentConfig })]);
  assert.equal(r.run('scentSources(S, 101, null).length'), 1);
  r.run('seek(101)');
  assert.match(r.element('scentinfo').textContent, /активных источников 1/);
});

test('replay never borrows a scent model from a future footprint', () => {
  const r = replay();
  r.parse([source(), source({ t: 3, seq: 2, scent: spreadConfig })]);
  assert.equal(r.run('scentSources(S, 2, null).length'), 0);
  r.run('seek(2)');
  assert.match(r.element('scentinfo').textContent, /нет модели запаха/);
});

test('restored v2 blood sources keep their age and invalid dispersion models are rejected', () => {
  const r = replay();
  r.parse([{ type: 'load', t: 0, gh: 10.5, scent: spreadConfig },
    source({ t: 0, gh: 10.5, born: 10, src: 'save', blood: true, scentBase: 2 })]);
  const p = r.run('scentSources(S, 0, null)[0]');
  assert.equal(p.age, 0.5);
  assert.ok(Math.abs(p.strength - 0.64173) < 0.0001);
  for (const changes of [{v:3}, {spread_hours:0}, {spread_max_mult:0.5}, {spread_max_mult:2.1}]) {
    r.parse([{ type:'load', t:0, gh:10, scent:{...spreadConfig,...changes} }, source()]);
    assert.equal(r.run('scentSources(S, 1, null).length'), 0);
  }
});

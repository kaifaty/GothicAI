'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync(process.argv[2], 'utf8');
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((done, fail) => { resolve = done; reject = fail; });
  return {promise, resolve, reject};
};
const response = (data) => ({ok: true, json: async () => JSON.parse(JSON.stringify(data))});
const take = (k) => ({k, dur: 1, peak: -12, speech: [0, 1]});
const session = (chosen = 1, restored = false) => ({
  name: 'Race test', video: null, room: true,
  lines: [
    {n: 1, text: 'Первая фраза.', takes: [take(1), take(2)], chosen, plan: null},
    {n: 2, text: 'Вторая фраза.', takes: restored ? [take(1)] : [], chosen: restored ? 1 : null, plan: null},
  ],
});

// Keep the real renderer: this DOM only supplies the elements it reads and creates.
function documentStub() {
  const ids = new Map();
  class Element {
    constructor(tagName = 'span', id) {
      this.tagName = tagName.toUpperCase();
      this.id = id;
      this.children = [];
      this.style = {};
      this.className = '';
      this.textContent = '';
      this.classList = {toggle: (name, on) => {
        const classes = new Set(this.className.split(/\s+/).filter(Boolean));
        if (on) classes.add(name); else classes.delete(name);
        this.className = [...classes].join(' ');
      }};
    }
    appendChild(child) { this.children.push(child); }
    get firstElementChild() { return this.children[0]; }
    set innerHTML(html) {
      const removeIds = (el) => {
        if (el.id) ids.delete(el.id);
        el.children.forEach(removeIds);
      };
      this.children.forEach(removeIds);
      this.children = [];
      for (const [, id] of html.matchAll(/id="([^"]+)"/g)) {
        const child = new Element('button', id);
        ids.set(id, child);
        this.appendChild(child);
      }
    }
  }
  for (const id of ['video', 'name', 'screen', 'count', 'prev', 'cur', 'next', 'planlen',
    'state', 'takes', 'lines', 'done', 'fit', 'msg', 'mic', 'sound', 'room', 'run', 'vspan', 'vhead']) {
    ids.set(id, new Element('div', id));
  }
  ids.get('fit').appendChild(new Element());
  const listeners = new Map();
  Object.assign(ids.get('video'), {
    src: '', duration: NaN, currentTime: 0, readyState: 0, pauseCalls: 0, playCalls: 0,
    pause() { this.pauseCalls += 1; },
    play() { this.playCalls += 1; return Promise.resolve(); },
    addEventListener(name, listener, options = {}) {
      if (!listeners.has(name)) listeners.set(name, []);
      listeners.get(name).push({listener, once: options.once});
    },
    fire(name) {
      const pending = listeners.get(name) || [];
      listeners.set(name, pending.filter((event) => !event.once));
      pending.forEach((event) => event.listener());
    },
    listenerCount(name) { return (listeners.get(name) || []).length; },
  });
  return {
    getElementById: (id) => ids.get(id) || null,
    createElement: (tag) => new Element(tag),
    addEventListener() {},
  };
}

async function page(fetchOverride) {
  const document = documentStub();
  const oldRead = deferred();
  const oldReadStarted = deferred();
  const restorePost = deferred();
  const restoreStarted = deferred();
  let reads = 0;
  const context = vm.createContext({
    document, navigator: {}, performance: {now: () => 0}, setTimeout, clearTimeout,
    requestAnimationFrame() {},
    fetch: fetchOverride || (async (path, options) => {
      if (path === '/api/session') {
        assert.equal(options.method, undefined);
        reads += 1;
        if (reads === 1) return response(session());
        if (reads === 2) { oldReadStarted.resolve(); return oldRead.promise; }
        assert.equal(reads, 3);
        return response(session(2, true));
      }
      assert.equal(options.method, 'POST');
      if (path === '/api/choose?line=1&take=2') return response({ok: true});
      assert.equal(path, '/api/restore?line=2&take=1');
      restoreStarted.resolve();
      return restorePost.promise;
    }),
  });
  vm.runInContext(source + '\nglobalThis.studio = {S, choose, discard, restore, goto, record, finish};', context, {timeout: 1000});
  await new Promise(setImmediate); // finish the application's initial load(false)
  assert.equal(document.getElementById('cur').textContent, 'Первая фраза.');
  return {studio: context.studio, document, oldRead, oldReadStarted, restorePost, restoreStarted};
}

async function restoreRace(oldResponseDuringPost) {
  const {studio, document, oldRead, oldReadStarted, restorePost, restoreStarted} = await page();
  studio.S.discarded.push({n: 2, k: 1});
  const choosing = studio.choose(2);
  await oldReadStarted.promise;
  const restoring = studio.restore();
  await restoreStarted.promise;
  if (oldResponseDuringPost) {
    oldRead.resolve(response(session(2)));
    await choosing;
  }
  restorePost.resolve(response({ok: true}));
  await restoring;
  if (!oldResponseDuringPost) {
    oldRead.resolve(response(session(2)));
    await choosing;
  }
  assert.equal(studio.S.i, 1, 'restore must stay on the restored line after an old choose finishes');
  assert.equal(studio.S.session.lines[1].chosen, 1);
  assert.equal(studio.S.session.lines[1].takes[0].k, 1);
  assert.equal(studio.S.discarded.length, 0);
  assert.equal(studio.S.changingTake, false);
  assert.equal(document.getElementById('cur').textContent, 'Вторая фраза.');
  assert.equal(document.getElementById('count').textContent, 'Фраза 2 из 2');
  const chosen = document.getElementById('takes').children.filter((el) => el.className === 'take chosen');
  assert.equal(chosen.length, 1);
  assert.match(chosen[0].title, /^дубль 1:/);
  assert.equal(document.getElementById('restorebtn'), null);
  assert.equal(document.getElementById('msg').textContent, 'Дубль фразы 2 вернулся и выбран.');
}

async function navigationRace() {
  const {studio, document, oldRead, oldReadStarted} = await page();
  const choosing = studio.choose(2);
  await oldReadStarted.promise;
  await studio.goto(1);
  oldRead.resolve(response(session(2)));
  await choosing;
  assert.equal(studio.S.i, 1, 'an old choose must not undo navigation while its GET was pending');
  assert.equal(document.getElementById('cur').textContent, 'Вторая фраза.');
}

async function playbackActions(mode) {
  const snapshot = session();
  const posts = [];
  const {studio, document} = await page(async (path, options) => {
    if (path === '/api/session') return response(snapshot);
    assert.equal(options.method, 'POST');
    posts.push(path);
    const ln = snapshot.lines[0];
    if (path === '/api/discard?line=1&take=1') {
      ln.takes = [take(2)];
      ln.chosen = 2;
    } else {
      assert.equal(path, '/api/restore?line=1&take=1');
      ln.takes = [take(1), take(2)];
      ln.chosen = 1;
    }
    return response({ok: true});
  });
  const video = document.getElementById('video');
  let audioPauses = 0;
  const start = () => {
    studio.S.mode = mode;
    studio.S.audio = mode === 'play' ? {pause() { audioPauses += 1; }} : null;
  };
  start();
  await studio.discard();
  assert.equal(studio.S.mode, 'idle');
  assert.equal(video.pauseCalls, 1);
  assert.equal(studio.S.audio, null);
  assert.equal(posts.length, 0, 'the first discard only arms, including during playback');
  assert.ok(studio.S.discardArm);
  assert.equal(document.getElementById('trashbtn').textContent, 'Ещё раз — в корзину');
  assert.match(document.getElementById('trashbtn').className, /\barmed\b/);
  await studio.discard();
  assert.deepEqual(posts, ['/api/discard?line=1&take=1']);
  assert.equal(studio.S.discardArm, null);
  assert.equal(studio.S.discarded.length, 1);
  assert.equal(studio.S.session.lines[0].chosen, 2);
  assert.equal(document.getElementById('trashbtn').textContent, 'В корзину');

  start();
  await studio.restore();
  assert.equal(studio.S.mode, 'idle');
  assert.equal(video.pauseCalls, 2);
  assert.equal(studio.S.audio, null);
  assert.equal(audioPauses, mode === 'play' ? 2 : 0);
  assert.deepEqual(posts, ['/api/discard?line=1&take=1', '/api/restore?line=1&take=1']);
  assert.equal(studio.S.session.lines[0].chosen, 1);
  assert.equal(studio.S.session.lines[0].takes.length, 2);
  assert.equal(studio.S.discarded.length, 0);
  assert.equal(document.getElementById('msg').textContent, 'Дубль фразы 1 вернулся и выбран.');
}

async function activeRecordingDoesNotChangeTakes(mode) {
  let posts = 0;
  const {studio, document} = await page(async (path) => {
    if (path === '/api/session') return response(session());
    posts += 1;
    throw new Error(`Unexpected request: ${path}`);
  });
  studio.S.mode = mode;
  studio.S.discarded.push({n: 2, k: 1});
  await studio.discard();
  await studio.restore();
  assert.equal(studio.S.mode, mode);
  assert.equal(posts, 0);
  assert.equal(document.getElementById('video').pauseCalls, 0);
  assert.equal(studio.S.discardArm, null);
  assert.equal(studio.S.discarded.length, 1);
  assert.equal(studio.S.session.lines[0].chosen, 1);
}

async function restoreMissingLineKeepsAValidIndex() {
  const posts = [];
  const {studio, document} = await page(async (path, options) => {
    if (path === '/api/session') return response(session());
    assert.equal(options.method, 'POST');
    posts.push(path);
    return response({ok: true});
  });
  studio.S.discarded.push({n: 99, k: 1});
  await studio.restore();
  assert.deepEqual(posts, ['/api/restore?line=99&take=1']);
  assert.equal(studio.S.i, 0);
  assert.equal(studio.S.discarded.length, 0);
  assert.equal(document.getElementById('cur').textContent, 'Первая фраза.');
  assert.equal(document.getElementById('msg').className, 'msg');
}

async function recordPreparation(stage) {
  const snapshot = session();
  snapshot.video = '/video.mp4';
  snapshot.lines[0].plan = [10, 12];
  const posts = [];
  const {studio, document} = await page(async (path, options) => {
    if (path === '/api/session') return response(snapshot);
    assert.equal(options.method, 'POST');
    posts.push(path);
    if (path === '/api/take?line=1&from=8.500') {
      snapshot.lines[0].takes.push(take(3));
      snapshot.lines[0].chosen = 3;
      return response(take(3));
    }
    // Baselines must finish an unguarded restore so the assertions show the race.
    if (path === '/api/restore?line=2&take=1') {
      snapshot.lines[1].takes = [take(1)];
      snapshot.lines[1].chosen = 1;
      return response({ok: true});
    }
    throw new Error(`Unexpected request: ${path}`);
  });
  const video = document.getElementById('video');
  Object.assign(video, {readyState: 1, duration: 100});
  const resume = deferred();
  let resumeCalls = 0;
  const commands = [];
  studio.S.ctx = {sampleRate: 48000, resume() { resumeCalls += 1; return resume.promise; }};
  studio.S.node = {port: {postMessage: (command) => commands.push(command)}};
  studio.S.discarded.push({n: 2, k: 1});
  const recording = studio.record();
  if (stage === 'seek') {
    resume.resolve();
    await new Promise(setImmediate);
    assert.equal(video.listenerCount('seeked'), 1);
  }
  assert.equal(studio.S.mode, 'idle');
  await studio.restore();
  assert.deepEqual(posts, [], 'restore must not run while record is preparing');
  assert.equal(document.getElementById('trashbtn').disabled, true);
  assert.equal(document.getElementById('restorebtn').disabled, true);
  await studio.choose(2);
  await studio.goto(1);
  await studio.discard();
  const duplicate = studio.record();
  await new Promise(setImmediate);
  assert.equal(resumeCalls, 1, 'another record must not start during preparation');
  assert.equal(studio.S.i, 0);
  assert.deepEqual(posts, []);
  assert.equal(studio.S.discarded.length, 1);
  assert.equal(studio.S.discardArm, null);
  assert.deepEqual(commands, []);
  if (stage === 'resume') {
    resume.resolve();
    await new Promise(setImmediate);
  }
  assert.equal(video.currentTime, 8.5);
  video.fire('seeked');
  await Promise.all([recording, duplicate]);
  assert.equal(studio.S.mode, 'rec');
  assert.equal(studio.S.preparingRecord, false);
  assert.equal(studio.S.i, 0);
  assert.deepEqual(commands, ['start']);
  studio.S.chunks = [new Float32Array([0.1, 0.2])];
  await studio.finish();
  assert.deepEqual(posts, ['/api/take?line=1&from=8.500']);
  assert.equal(studio.S.session.lines[0].chosen, 3);
  assert.equal(document.getElementById('cur').textContent, 'Первая фраза.');
  assert.equal(document.getElementById('msg').className, 'msg');
}

async function failedRecordPreparationUnlocksRestore() {
  let restored = false;
  const {studio, document} = await page(async (path) => {
    if (path === '/api/session') return response(session(1, restored));
    assert.equal(path, '/api/restore?line=2&take=1');
    restored = true;
    return response({ok: true});
  });
  const resume = deferred();
  studio.S.ctx = {resume: () => resume.promise};
  studio.S.node = {port: {postMessage() { assert.fail('failed resume must not start recording'); }}};
  studio.S.discarded.push({n: 2, k: 1});
  const recording = studio.record();
  resume.reject(new Error('resume failed'));
  await recording;
  assert.equal(studio.S.mode, 'idle');
  assert.equal(studio.S.preparingRecord, false);
  assert.match(document.getElementById('msg').textContent, /resume failed/);
  assert.equal(document.getElementById('msg').className, 'msg err');
  await studio.restore();
  assert.equal(restored, true);
  assert.equal(studio.S.i, 1);
  assert.equal(studio.S.discarded.length, 0);
  assert.equal(document.getElementById('msg').textContent, 'Дубль фразы 2 вернулся и выбран.');
}

async function lateChoosePostPrecedesRestore(failChoose = false) {
  const snapshot = session(2);
  snapshot.lines[0].takes = [take(2), take(3)];
  const chooseStarted = deferred();
  const chooseRelease = deferred();
  const posts = [];
  const {studio, document} = await page(async (path, options) => {
    if (path === '/api/session') return response(snapshot);
    assert.equal(options.method, 'POST');
    posts.push(path);
    if (path === '/api/choose?line=1&take=3') {
      chooseStarted.resolve();
      await chooseRelease.promise;
      if (failChoose) throw new Error('choose failed');
      snapshot.lines[0].chosen = 3;
    } else {
      assert.equal(path, '/api/restore?line=1&take=1');
      snapshot.lines[0].takes.unshift(take(1));
      snapshot.lines[0].chosen = 1;
    }
    return response({ok: true});
  });
  studio.S.discarded.push({n: 1, k: 1});
  const choosing = studio.choose(3);
  await chooseStarted.promise;
  const restoring = studio.restore();
  await new Promise(setImmediate);
  assert.deepEqual(posts, ['/api/choose?line=1&take=3'], 'restore must wait for the earlier choose POST');
  chooseRelease.resolve();
  await Promise.all([choosing, restoring]);
  assert.deepEqual(posts, ['/api/choose?line=1&take=3', '/api/restore?line=1&take=1']);
  assert.equal(snapshot.lines[0].chosen, 1, 'restore must be the final server-side selection');
  assert.equal(studio.S.session.lines[0].chosen, 1);
  assert.equal(studio.S.discarded.length, 0);
  const chosen = document.getElementById('takes').children.filter((el) => el.className === 'take chosen');
  assert.equal(chosen.length, 1);
  assert.match(chosen[0].title, /^дубль 1:/);
  assert.equal(document.getElementById('msg').textContent, 'Дубль фразы 1 вернулся и выбран.');
}

const timeout = setTimeout(() => { throw new Error('studio race test timed out'); }, 5000);
(async () => {
  const scenarios = {
    existing: async () => {
      await restoreRace(false);
      await restoreRace(true);
      await navigationRace();
      for (const mode of ['play', 'run']) await playbackActions(mode);
      for (const mode of ['rec', 'saving', 'room']) await activeRecordingDoesNotChangeTakes(mode);
      await restoreMissingLineKeepsAValidIndex();
    },
    'record-resume': () => recordPreparation('resume'),
    'record-seek': () => recordPreparation('seek'),
    'record-error': failedRecordPreparationUnlocksRestore,
    'choose-post': lateChoosePostPrecedesRestore,
    'choose-error': () => lateChoosePostPrecedesRestore(true),
  };
  if (process.argv[3]) {
    assert.ok(scenarios[process.argv[3]], 'unknown scenario');
    await scenarios[process.argv[3]]();
  } else {
    for (const scenario of Object.values(scenarios)) await scenario();
  }
})().catch((err) => {
  console.error(err);
  process.exitCode = 1;
}).finally(() => clearTimeout(timeout));

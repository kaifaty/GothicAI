// The voice-over studio: a line of the script at a time, over the part of the draft it is said over.
// Space records (the picture starts PREROLL seconds before the line's place, the cue turns at the place),
// Space again keeps the take, Esc throws it away, Enter plays the chosen take with the picture.
// Backspace/Delete or the trash button twice within 3 s discards a take; Z restores the last discarded take.
'use strict';

const PREROLL = 1.5; // seconds of picture before the line's place: time to breathe in
const $ = (id) => document.getElementById(id);
const video = $('video');

const S = {
  session: null,
  loadRequest: 0, // only the latest session request may update the page
  i: 0, // the current line (index)
  mode: 'idle', // idle | rec | saving | play | run | room
  ctx: null,
  node: null,
  stream: null,
  chunks: [],
  cancel: false,
  recStart: 0,
  videoFrom: null,
  clipped: false,
  sound: false,
  audio: null, // a take playing
  peakHold: 0,
  discardArm: null,
  discarded: [], // {n, k}, newest last; only this page's session
  changingTake: false,
  preparingRecord: false,
  takeChanges: Promise.resolve(), // serialize take mutations; session reads may finish independently
};

const line = () => S.session.lines[S.i];
const chosenTake = (ln) => ln.takes.find((t) => t.k === ln.chosen) || ln.takes[ln.takes.length - 1];
const hasVideo = () => Boolean(S.session && S.session.video) && video.readyState > 0;
const sec = (x) => x.toFixed(1).replace('.', ',');

function say(text, err = false) {
  $('msg').textContent = text;
  $('msg').className = err ? 'msg err' : 'msg';
}

async function api(path, body) {
  const res = await fetch(path, body === undefined ? {} : {method: 'POST', body, headers: {'Content-Type': 'audio/wav'}});
  const data = await res.json();
  if (!res.ok || data.error) throw new Error(data.error || res.statusText);
  return data;
}

function changeTake(path) {
  const request = S.takeChanges.then(() => api(path, ''));
  S.takeChanges = request.catch(() => {}); // a failed request must not block the next action
  return request;
}

async function load(keepLine = true) {
  const request = ++S.loadRequest;
  const session = await api('/api/session');
  if (request !== S.loadRequest) return;
  S.session = session;
  S.i = keepLine ? Math.min(S.i, S.session.lines.length - 1) : 0;
  document.title = `Озвучка · ${S.session.name}`;
  $('name').textContent = `${S.session.name} · озвучка`;
  if (S.session.video && !video.src.endsWith(S.session.video)) {
    video.src = S.session.video;
  }
  if (!S.session.video) {
    $('screen').innerHTML = '<div class="novideo">Черновика нет (render/preview.mp4).<br>Суфлёр работает и без картинки.</div>';
  }
  render();
}

// -- drawing ------------------------------------------------------------------------------------------------

function render() {
  const lines = S.session.lines;
  const ln = line();
  const arm = S.discardArm;
  if (arm && (S.mode !== 'idle' || arm.n !== ln.n || arm.k !== chosenTake(ln)?.k ||
              performance.now() >= arm.until)) disarmDiscard();
  $('count').textContent = `Фраза ${ln.n} из ${lines.length}`;
  $('prev').textContent = S.i > 0 ? lines[S.i - 1].text : '';
  $('cur').textContent = ln.text;
  $('next').textContent = S.i < lines.length - 1 ? lines[S.i + 1].text : '';
  $('planlen').textContent = ln.plan ? `по плану ${sec(ln.plan[1] - ln.plan[0])} с` : '';
  if (S.mode === 'idle') {
    $('cur').className = 'cur';
    $('state').className = 'state ready';
    $('state').textContent = S.preparingRecord ? 'Подготавливаю запись…'
      : S.ctx ? 'Пробел — записать' : 'Пробел — включить микрофон и записать';
    fit(0);
  }

  const box = $('takes');
  box.innerHTML = '';
  ln.takes.forEach((t, idx) => {
    const el = document.createElement('span');
    el.className = 'take' + (t.k === ln.chosen ? ' chosen' : '');
    const speech = t.speech ? t.speech[1] - t.speech[0] : t.dur;
    let warn = '';
    if (t.peak !== null && t.peak > -1) warn = 'перегруз';
    else if (t.peak !== null && t.peak < -30) warn = 'тихо';
    el.innerHTML = `<span class="k">${idx + 1}</span>${sec(speech)} с · ${t.peak === null ? '—' : Math.round(t.peak)} dB` +
      (warn ? `<span class="warn">${warn}</span>` : '');
    el.title = `дубль ${t.k}: щёлкни — выбрать, двойной щелчок — прослушать`;
    el.onclick = () => choose(t.k);
    el.ondblclick = () => play(t);
    box.appendChild(el);
  });
  if (!ln.takes.length) box.innerHTML = '<span class="hint">Дублей пока нет.</span>';
  if (ln.takes.length || S.discarded.length) {
    const acts = document.createElement('span');
    acts.className = 'actions';
    acts.innerHTML = (ln.takes.length
      ? '<button id="playbtn">▶ Прослушать</button><button id="trashbtn">В корзину</button>' : '') +
      (S.discarded.length ? '<button id="restorebtn">Вернуть дубль</button>' : '');
    box.appendChild(acts);
    if (ln.takes.length) {
      $('playbtn').onclick = () => play(chosenTake(line()));
      $('trashbtn').onclick = () => discard();
    }
    if (S.discarded.length) {
      $('restorebtn').onclick = () => restore();
      $('restorebtn').disabled = S.changingTake || S.preparingRecord;
    }
  }
  renderDiscardButton();

  const pills = $('lines');
  pills.innerHTML = '';
  lines.forEach((l, idx) => {
    const b = document.createElement('button');
    b.textContent = l.chosen ? `${l.n} ✓` : `${l.n}`;
    b.className = (idx === S.i ? 'here ' : '') + (l.chosen ? 'ok' : l.takes.length ? 'has' : '');
    b.title = l.text;
    b.onclick = () => goto(idx);
    pills.appendChild(b);
  });

  const all = lines.every((l) => l.chosen);
  $('done').textContent = all
    ? `Все ${lines.length} фраз записаны. Скажи агенту, что готово: дальше чистка и разметка слов.` +
      (S.session.room ? '' : ' Ещё запиши 10 секунд тишины комнаты (кнопка вверху).')
    : '';
  drawBar();
}

function drawBar() {
  const ln = line();
  const d = video.duration;
  if (!d || !isFinite(d)) return;
  if (ln.plan) {
    $('vspan').style.left = `${(100 * ln.plan[0]) / d}%`;
    $('vspan').style.width = `${(100 * (ln.plan[1] - ln.plan[0])) / d}%`;
  }
  $('vhead').style.left = `${(100 * video.currentTime) / d}%`;
}

function fit(k, over = false) {
  $('fit').firstElementChild.style.width = `${Math.min(1, Math.max(0, k)) * 100}%`;
  $('fit').className = over ? 'fit over' : 'fit';
}

function level(peak) {
  const db = peak > 0 ? 20 * Math.log10(peak) : -90;
  S.peakHold = Math.max(db, S.peakHold - 0.6);
  const w = (v) => `${Math.max(0, Math.min(1, (v + 60) / 60)) * 100}%`;
  $('meter').firstElementChild.style.width = w(db);
  $('meter').lastElementChild.style.left = w(S.peakHold);
  $('meter').className = S.peakHold > -1 ? 'meter clip' : 'meter';
  $('db').textContent = `${Math.round(S.peakHold)} dB`;
  if (S.mode === 'rec' && peak >= 0.98) S.clipped = true;
}

// -- the microphone ---------------------------------------------------------------------------------------

async function mic() {
  if (S.node) return;
  const id = localStorage.getItem('mic') || undefined;
  // no echo cancelling, noise suppression or gain riding: the voice as the microphone hears it
  S.stream = await navigator.mediaDevices.getUserMedia({
    audio: {
      deviceId: id ? {exact: id} : undefined,
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
      channelCount: 1,
    },
  });
  try {
    S.ctx = new AudioContext({sampleRate: 48000});
  } catch {
    S.ctx = new AudioContext();
  }
  await S.ctx.audioWorklet.addModule('/app/recorder.js');
  S.node = new AudioWorkletNode(S.ctx, 'recorder');
  const mute = S.ctx.createGain();
  mute.gain.value = 0; // the node must reach the output to run; nothing is heard
  S.ctx.createMediaStreamSource(S.stream).connect(S.node).connect(mute).connect(S.ctx.destination);
  S.node.port.onmessage = (e) => {
    const m = e.data;
    if (m.type === 'level') level(m.peak);
    else if (m.type === 'chunk') S.chunks.push(m.data);
    else if (m.type === 'done') finish();
  };
  await listMics();
}

async function listMics() {
  const devs = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === 'audioinput');
  const sel = $('mic');
  const cur = S.stream && S.stream.getAudioTracks()[0].getSettings().deviceId;
  sel.innerHTML = '';
  for (const d of devs) {
    const o = document.createElement('option');
    o.value = d.deviceId;
    o.textContent = d.label || 'микрофон';
    o.selected = d.deviceId === cur;
    sel.appendChild(o);
  }
}

$('mic').onchange = async (e) => {
  localStorage.setItem('mic', e.target.value);
  if (S.stream) S.stream.getTracks().forEach((t) => t.stop());
  if (S.ctx) await S.ctx.close();
  S.node = null;
  S.ctx = null;
  await mic();
  say(`Микрофон: ${e.target.selectedOptions[0].textContent}`);
};

function wav(chunks, rate) {
  const n = chunks.reduce((s, c) => s + c.length, 0);
  const buf = new ArrayBuffer(44 + n * 4);
  const dv = new DataView(buf);
  const str = (o, s) => [...s].forEach((ch, i) => dv.setUint8(o + i, ch.charCodeAt(0)));
  str(0, 'RIFF');
  dv.setUint32(4, 36 + n * 4, true);
  str(8, 'WAVE');
  str(12, 'fmt ');
  dv.setUint32(16, 16, true);
  dv.setUint16(20, 3, true); // IEEE float
  dv.setUint16(22, 1, true);
  dv.setUint32(24, rate, true);
  dv.setUint32(28, rate * 4, true);
  dv.setUint16(32, 4, true);
  dv.setUint16(34, 32, true);
  str(36, 'data');
  dv.setUint32(40, n * 4, true);
  let o = 44;
  for (const c of chunks) {
    new Float32Array(buf, o, c.length).set(c);
    o += c.length * 4;
  }
  return buf;
}

// -- the picture ------------------------------------------------------------------------------------------

function seek(t) {
  return new Promise((resolve) => {
    if (!hasVideo() || Math.abs(video.currentTime - t) < 0.02) return resolve();
    video.addEventListener('seeked', () => resolve(), {once: true});
    video.currentTime = t;
  });
}

async function showLine() {
  const ln = line();
  if (hasVideo() && ln.plan) {
    video.pause();
    await seek(ln.plan[0]);
  }
  drawBar();
}

// -- takes ------------------------------------------------------------------------------------------------

async function record() {
  if (S.changingTake || S.preparingRecord || S.mode === 'rec' || S.mode === 'saving' || S.mode === 'room') return;
  disarmDiscard();
  stopPlay();
  S.preparingRecord = true;
  try {
    render();
    if (!S.node) {
      // the first press only turns the microphone on: nobody is ready to speak right after the permission
      try {
        await mic();
        await S.ctx.resume();
      } catch (err) {
        say(`Микрофон недоступен: ${err.message}. Разреши доступ к микрофону для этой страницы.`, true);
        return;
      }
      say('Микрофон включён: посмотри на уровень, скажи пару слов. Пробел — записать.');
      return;
    }
    await S.ctx.resume();
    const ln = line();
    S.chunks = [];
    S.clipped = false;
    S.cancel = false;
    S.videoFrom = null;
    if (hasVideo() && ln.plan) {
      S.videoFrom = Math.max(0, ln.plan[0] - PREROLL);
      await seek(S.videoFrom);
      video.muted = !S.sound;
      video.play();
    }
    S.node.port.postMessage('start');
    S.recStart = performance.now();
    S.mode = 'rec';
    say('');
    requestAnimationFrame(tick);
  } catch (err) {
    say(`Запись не началась: ${err.message}`, true);
  } finally {
    S.preparingRecord = false;
    if (S.mode === 'idle') render();
  }
}

function stop(cancel) {
  S.cancel = cancel;
  S.mode = 'saving';
  video.pause();
  S.node.port.postMessage('stop'); // the worklet sends what it has, then 'done'
  $('state').textContent = cancel ? 'Отменено' : 'Сохраняю…';
}

async function finish() {
  if (S.mode === 'room') return finishRoom();
  const chunks = S.chunks;
  S.chunks = [];
  if (S.cancel || !chunks.length) {
    S.mode = 'idle';
    render();
    return;
  }
  const ln = line();
  const from = S.videoFrom === null ? '' : S.videoFrom.toFixed(3);
  try {
    const t = await api(`/api/take?line=${ln.n}&from=${from}`, wav(chunks, S.ctx.sampleRate));
    S.mode = 'idle';
    await load();
    const speech = t.speech[1] - t.speech[0];
    let note = `Дубль ${line().takes.length}: ${sec(speech)} с речи`;
    if (ln.plan) note += ` (по плану ${sec(ln.plan[1] - ln.plan[0])} с)`;
    if (S.clipped || (t.peak !== null && t.peak > -1)) note += '. Перегруз: отодвинься от микрофона или убавь вход.';
    else if (t.peak !== null && t.peak < -30) note += '. Очень тихо: ближе к микрофону.';
    else note += '. Enter — прослушать с картинкой.';
    say(note);
  } catch (err) {
    S.mode = 'idle';
    render();
    say(`Дубль не сохранился: ${err.message}`, true);
  }
}

function tick() {
  if (S.mode === 'rec') {
    const ln = line();
    const el = (performance.now() - S.recStart) / 1000;
    if (S.videoFrom !== null && ln.plan) {
      const vt = video.currentTime;
      if (vt < ln.plan[0]) {
        $('cur').className = 'cur wait';
        $('state').className = 'state ready';
        $('state').textContent = `Приготовься… ${sec(ln.plan[0] - vt)}`;
        fit(0);
      } else {
        $('cur').className = 'cur go';
        $('state').className = 'state rec';
        $('state').textContent = `● Говори  ${sec(el)} с`;
        const k = (vt - ln.plan[0]) / (ln.plan[1] - ln.plan[0]);
        fit(k, k > 1.15);
      }
    } else {
      $('cur').className = 'cur go';
      $('state').className = 'state rec';
      $('state').textContent = `● Запись  ${sec(el)} с`;
    }
    drawBar();
    requestAnimationFrame(tick);
  } else if (S.mode === 'run') {
    // the prompter follows the picture: the line whose place has come
    const vt = video.currentTime;
    let idx = S.i;
    S.session.lines.forEach((l, j) => {
      if (l.plan && vt >= l.plan[0] - 0.3) idx = j;
    });
    if (idx !== S.i) {
      S.i = idx;
      render();
    }
    const ln = line();
    const on = ln.plan && vt >= ln.plan[0] && vt <= ln.plan[1] + 0.2;
    $('cur').className = on ? 'cur go' : 'cur wait';
    $('state').className = 'state ready';
    $('state').textContent = 'Прогон: читай вслух, запись не идёт';
    if (ln.plan) fit((vt - ln.plan[0]) / (ln.plan[1] - ln.plan[0]));
    drawBar();
    if (video.ended) stopPlay();
    else requestAnimationFrame(tick);
  } else if (S.mode === 'play' || S.mode === 'room') {
    drawBar();
    requestAnimationFrame(tick);
  }
}

async function play(t) {
  if (!t || S.changingTake || S.preparingRecord) return;
  disarmDiscard();
  stopPlay();
  const a = new Audio(t.url);
  S.audio = a;
  // from just before the speech: the preroll's silence is not worth waiting through
  const skip = Math.max(0, (t.speech ? t.speech[0] : 0) - 0.4);
  a.currentTime = skip;
  a.onended = () => stopPlay();
  S.mode = 'play';
  $('state').className = 'state ready';
  $('state').textContent = '▶ Слушаю';
  if (hasVideo() && t.video_from !== null && t.video_from !== undefined) {
    await seek(t.video_from + skip);
    video.muted = !S.sound;
    video.play();
  }
  await a.play();
  requestAnimationFrame(tick);
}

function stopPlay() {
  if (S.audio) {
    S.audio.pause();
    S.audio = null;
  }
  if (S.mode === 'play' || S.mode === 'run') {
    video.pause();
    S.mode = 'idle';
    render();
  }
}

async function choose(k) {
  if (S.changingTake || S.preparingRecord) return;
  if (k !== chosenTake(line())?.k) disarmDiscard();
  try {
    await changeTake(`/api/choose?line=${line().n}&take=${k}`);
    await load();
  } catch (err) {
    say(err.message, true);
  }
}

function renderDiscardButton() {
  const btn = $('trashbtn');
  if (!btn) return;
  btn.textContent = S.discardArm ? 'Ещё раз — в корзину' : 'В корзину';
  btn.classList.toggle('armed', Boolean(S.discardArm));
  btn.disabled = S.changingTake || S.preparingRecord;
}

function disarmDiscard(announce = true) {
  const arm = S.discardArm;
  if (!arm) return;
  clearTimeout(arm.timer);
  S.discardArm = null;
  renderDiscardButton();
  if (announce) say(`Дубль ${arm.idx} остался.`);
}

async function discard() {
  if (S.mode === 'play' || S.mode === 'run') stopPlay();
  if (S.mode !== 'idle' || S.changingTake || S.preparingRecord) return;
  const ln = line();
  const t = chosenTake(ln);
  if (!t) return;
  const idx = ln.takes.indexOf(t) + 1;
  const arm = S.discardArm;
  if (!arm || arm.n !== ln.n || arm.k !== t.k || performance.now() >= arm.until) {
    disarmDiscard(false);
    const next = {n: ln.n, k: t.k, idx, until: performance.now() + 3000};
    S.discardArm = next;
    next.timer = setTimeout(() => {
      if (S.discardArm === next) disarmDiscard();
    }, 3000);
    renderDiscardButton();
    say(`Дубль ${idx} фразы ${ln.n} — в корзину? Ещё раз ⌫ или кнопку — за 3 с.`);
    return;
  }
  disarmDiscard(false);
  S.changingTake = true;
  render();
  try {
    await changeTake(`/api/discard?line=${ln.n}&take=${t.k}`);
    S.discarded.push({n: ln.n, k: t.k});
    await load();
    say(`Дубль ${idx} фразы ${ln.n} в корзине. Z — вернуть.`);
  } catch (err) {
    say(err.message, true);
  } finally {
    S.changingTake = false;
    render();
  }
}

async function restore() {
  if (S.mode === 'play' || S.mode === 'run') stopPlay();
  if (S.mode !== 'idle' || S.changingTake || S.preparingRecord || !S.discarded.length) return;
  disarmDiscard();
  const {n, k} = S.discarded[S.discarded.length - 1];
  S.changingTake = true;
  render();
  try {
    await changeTake(`/api/restore?line=${n}&take=${k}`);
    S.discarded.pop();
    S.i = Math.max(0, S.session.lines.findIndex((ln) => ln.n === n));
    await load();
    await showLine();
    say(`Дубль фразы ${n} вернулся и выбран.`);
  } catch (err) {
    say(err.message, true);
  } finally {
    S.changingTake = false;
    render();
  }
}

async function goto(idx) {
  if (S.changingTake || S.preparingRecord || S.mode === 'rec' || S.mode === 'saving' || S.mode === 'room') return;
  const next = Math.max(0, Math.min(S.session.lines.length - 1, idx));
  if (next !== S.i) disarmDiscard();
  stopPlay();
  S.i = next;
  render();
  await showLine();
}

// -- the room's silence, the rehearsal ---------------------------------------------------------------------

async function room() {
  if (S.mode !== 'idle' || S.changingTake || S.preparingRecord) return;
  disarmDiscard();
  try {
    await mic();
  } catch (err) {
    say(`Микрофон недоступен: ${err.message}`, true);
    return;
  }
  await S.ctx.resume();
  S.chunks = [];
  S.mode = 'room';
  S.node.port.postMessage('start');
  const t0 = performance.now();
  const timer = setInterval(() => {
    const left = 10 - (performance.now() - t0) / 1000;
    $('state').className = 'state rec';
    $('state').textContent = `Тишина… ${Math.ceil(left)} с, не говори и не двигайся`;
    if (left <= 0) {
      clearInterval(timer);
      S.node.port.postMessage('stop');
    }
  }, 100);
}

async function finishRoom() {
  const chunks = S.chunks;
  S.chunks = [];
  try {
    const r = await api('/api/room', wav(chunks, S.ctx.sampleRate));
    say(`Тишина записана: ${sec(r.dur)} с, пик ${r.peak === null ? '—' : Math.round(r.peak)} dB.`);
  } catch (err) {
    say(`Тишина не сохранилась: ${err.message}`, true);
  }
  S.mode = 'idle';
  await load();
}

async function rehearse() {
  if (S.mode === 'run') return stopPlay();
  if (S.mode !== 'idle' || S.changingTake || S.preparingRecord || !hasVideo()) return;
  disarmDiscard();
  const ln = line();
  await seek(ln.plan ? Math.max(0, ln.plan[0] - PREROLL) : 0);
  video.muted = !S.sound;
  S.mode = 'run';
  video.play();
  requestAnimationFrame(tick);
}

// -- keys and buttons ---------------------------------------------------------------------------------------

document.addEventListener('keydown', (e) => {
  if (e.target.tagName === 'SELECT' || e.metaKey || e.ctrlKey || e.altKey) return;
  const k = e.key;
  if (k === ' ') {
    e.preventDefault();
    if (S.mode === 'idle') record();
    else if (S.mode === 'rec') stop(false);
    else if (S.mode === 'play' || S.mode === 'run') stopPlay();
  } else if (k === 'Escape') {
    disarmDiscard();
    if (S.mode === 'rec') stop(true);
    else stopPlay();
  } else if (k === 'Enter') {
    e.preventDefault();
    if (S.mode === 'play') stopPlay();
    else if (S.mode === 'idle') play(chosenTake(line()));
  } else if (k === 'ArrowUp' || k === 'ArrowLeft') {
    e.preventDefault();
    goto(S.i - 1);
  } else if (k === 'ArrowDown' || k === 'ArrowRight') {
    e.preventDefault();
    goto(S.i + 1);
  } else if (/^[1-9]$/.test(k) && S.mode === 'idle') {
    const t = line().takes[Number(k) - 1];
    if (t) choose(t.k);
  } else if ((k === 'Backspace' || k === 'Delete') && S.mode === 'idle') {
    e.preventDefault();
    if (!e.repeat) discard();
  } else if (e.code === 'KeyZ' && !e.shiftKey && S.mode === 'idle') {
    e.preventDefault();
    if (!e.repeat) restore();
  }
});

$('sound').onclick = () => {
  S.sound = !S.sound;
  $('sound').textContent = `Звук видео: ${S.sound ? 'вкл' : 'выкл'}`;
  $('sound').className = S.sound ? 'on' : '';
  video.muted = !S.sound;
  if (S.sound) say('Звук игры только в наушниках: из колонок он попадёт в запись.');
};
$('room').onclick = () => room();
$('run').onclick = () => rehearse();
video.addEventListener('loadedmetadata', () => showLine());
video.addEventListener('timeupdate', drawBar);

load(false)
  .then(async () => {
    // an embedded browser (an app's preview pane) may refuse the microphone outright: say so before a press
    const state = navigator.permissions
      ? await navigator.permissions.query({name: 'microphone'}).then((r) => r.state).catch(() => '')
      : '';
    if (state === 'denied') {
      say(`Здесь микрофон запрещён. Открой ${location.origin} в Chrome или Safari ` +
          '(или разреши микрофон в настройках сайта).', true);
    }
  })
  .catch((err) => say(`Не загрузилось: ${err.message}`, true));

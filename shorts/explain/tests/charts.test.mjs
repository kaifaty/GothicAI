import assert from 'node:assert/strict';
import test from 'node:test';
import {CombatData, Plan} from '../src/schema.ts';
import {inCone, interpolatePath, quadraticParameterAtLength, quadraticPoint} from '../src/parts/diagramMath.ts';
import {arcPoint, settledText, tagLineCentre} from '../src/parts/carryMath.ts';
import {holdPush, travelDur} from '../src/parts/timing.ts';
import {sampleHp, selectAction, smoothedTrack, sourceFrame, transformPoint, zoomScale} from '../src/parts/followMath.ts';

const source = 'reports/example.md';
const line = () => ({
  type: 'line', title: 'Победы', source,
  axisX: {min: 0, max: 40, ticks: [0, 20, 40], label: 'Итерация'},
  axisY: {min: 0, max: 100, ticks: [0, 50, 100], label: '%'},
  series: [{name: 'P30', color: '#d9b36c', points: [[0, 25], [20, 50], [40, 95]], draw: [1, 3]}],
  marks: [{x: 30, text: 'P30', at: 4}],
  labels: [{x: 40, y: 95, text: '95%', at: 5}],
});
const big = () => ({
  type: 'big', title: 'Размер модели', source,
  items: [{value: '476 556', label: 'чисел', at: 1, color: '#d9b36c',
    count: {from: 0, to: 476556, dur: 2, format: '{v}'}},
  {value: 1, label: 'мс на решение', at: 3}],
});
const table = () => ({
  type: 'table', title: 'Оценка', source,
  columns: ['Показатель', 'BC', 'P30'], highlight: 2,
  rows: [{cells: ['HP', 218.2, 294.5], at: 1}, {cells: ['Без урона', 78, 1814], at: 2}],
});
const cells = () => ({
  type: 'cells', title: 'Экзамен', source,
  cells: [{label: 'v1', state: 'fail', at: 0}, {label: 'v2', state: 'skip', at: 1},
    {label: 'v3', state: 'pass', at: 2}],
  spans: [{from: 0, to: 1, text: '14.10'}, {from: 2, to: 2, text: '15.10'}],
  notes: [{text: 'Экзамен сдан', at: 3, color: '#d9b36c'}],
});
const bars = () => ({
  type: 'bars', title: 'Решения в секунду', source,
  series: [{name: 'Скорость', color: '#aaa'}],
  groups: [{label: 'BC', values: [2.5], at: 0}, {label: 'P30', values: [90], at: 1}],
  max: 100,
});
const rays = () => ({
  type: 'rays', title: '184 числа', source, maxCm: 500,
  rays: Array.from({length: 16}, (_, i) => ({cm: i % 2 ? 500 : 200, hit: i % 2 === 0})),
  target: {x: 100, y: 200, label: 'противник', at: 1}, hideAt: 5, raysAt: [0.5, 2],
  panels: [{title: 'Я', lines: ['HP: 100'], color: '#d9b36c', at: 0},
    {title: 'Противник', lines: ['HP: 80'], color: '#78a8cf', at: 1}],
});
const cone = () => ({
  type: 'cone', title: 'Конус зрения', source, halfDeg: 45, rangeCm: 600,
  path: [{t: 0, x: 0, y: 300}, {t: 2, x: 400, y: 300}, {t: 5, x: 400, y: 300}],
  targetLabel: 'противник', turn: {at: 3, dur: 0.4, deg: 55, label: '≤ 400 мс'},
  texts: [{text: 'Цель потеряна', at: 2, color: '#e67868'}],
});
const ticks = () => ({
  type: 'ticks', title: 'Один тик', source, dur: 2,
  rows: [{label: 'игра', cells: 'DBBBDBBB', offset: 0, at: 0},
    {label: 'стенд', cells: 'DBBBDBBB', offset: 0.5, at: 1}],
  legend: {D: 'решение', B: 'тело занято'}, align: {at: 4, dur: 1},
});
const grid = () => ({
  type: 'grid', title: 'Действия', source, columns: 2,
  tiles: [{label: 'подойти', at: 0}, {label: 'повернуться', at: 1}],
});
const flow = () => ({
  type: 'flow', title: 'Самоимитация', source,
  nodes: [{id: 'play', label: 'играть', x: 0.2, y: 0.4, w: 0.2, color: '#d9b36c', at: 0},
    {id: 'learn', label: 'доучить', x: 0.8, y: 0.4, at: 1}],
  edges: [{from: 'play', to: 'learn', label: 'отобрать', at: 2, packets: {every: 0.4, dur: 1}},
    {from: 'learn', to: 'play', label: 'снова', at: 3, curve: -0.2}],
});

function plan(chart, format = 'wide') {
  const input = {lang: 'ru', hook: {text: '', until: 0}, beats: [{id: 'chart', kind: 'card', dur: 10, chart}]};
  if (format !== undefined) input.format = format;
  return input;
}

function expectIssue(chart, path, message) {
  const result = Plan.safeParse(plan(chart));
  assert.equal(result.success, false);
  assert.ok(result.error.issues.some((issue) =>
    issue.path.join('.') === `beats.0.chart.${path}` && message.test(issue.message)),
  JSON.stringify(result.error.issues));
}

for (const makeChart of [line, big, table, cells, rays, cone, ticks, grid, flow]) {
  const type = makeChart().type;
  test(`${type}: wide сохраняет данные и моменты появления`, () => {
    const chart = makeChart();
    assert.deepEqual(Plan.parse(plan(chart)).beats[0].chart, chart);
  });

  for (const format of ['tall', undefined]) {
    test(`${type}: ${format ?? 'формат по умолчанию'} отклонён с путём и «только wide»`, () => {
      const input = plan(makeChart());
      if (format === undefined) delete input.format;
      else input.format = format;
      const result = Plan.safeParse(input);
      assert.equal(result.success, false);
      assert.deepEqual(result.error.issues.map(({path, message}) => ({path, message})), [
        {path: ['beats', 0, 'chart', 'type'], message: `${type}: только wide`},
      ]);
    });
  }
}

test('bars: прежний tall-план остаётся валидным без новых полей', () => {
  const chart = bars();
  const parsed = Plan.parse(plan(chart, 'tall')).beats[0].chart;
  assert.deepEqual(parsed, {...chart, log: false, format: '×{v}', decimal: ','});
});

test('bars: log, цвет и примечание группы сохраняются', () => {
  const chart = bars();
  chart.log = true;
  chart.groups[0].color = '#d9b36c';
  chart.groups[1].note = 'без модели';
  chart.groups.push({label: 'Ноль', values: [0], at: 2});
  const parsed = Plan.parse(plan(chart)).beats[0].chart;
  assert.equal(parsed.log, true);
  assert.deepEqual(parsed.groups, chart.groups);
});

for (const [name, change, path, message] of [
  ['нулевой диапазон оси', (c) => { c.axisX.max = c.axisX.min; }, 'axisX.max', /больше min/],
  ['обратный диапазон оси', (c) => { c.axisY.min = 101; }, 'axisY.max', /больше min/],
  ['деление вне оси', (c) => { c.axisY.ticks[1] = 101; }, 'axisY.ticks.1', /вне диапазона/],
  ['повтор X', (c) => { c.series[0].points[1][0] = 0; }, 'series.0.points.1.0', /строго возрастать/],
  ['убывание X', (c) => { c.series[0].points[2][0] = 10; }, 'series.0.points.2.0', /строго возрастать/],
  ['точка вне X', (c) => { c.series[0].points[2][0] = 41; }, 'series.0.points.2.0', /вне диапазона X/],
  ['точка вне Y', (c) => { c.series[0].points[1][1] = -1; }, 'series.0.points.1.1', /вне диапазона Y/],
]) {
  test(`line: ${name}`, () => {
    const chart = line();
    change(chart);
    expectIssue(chart, path, message);
  });
}

for (const count of [2, 4]) {
  test(`table: ${count} ячейки при трёх колонках отклонены`, () => {
    const chart = table();
    chart.rows[1].cells = Array(count).fill('ячейка');
    expectIssue(chart, 'rows.1.cells', /число ячеек/);
  });
}

test('table: highlight за последней колонкой отклонён', () => {
  const chart = table();
  chart.highlight = chart.columns.length;
  expectIssue(chart, 'highlight', /индекс вне columns/);
});

for (const [name, span] of [
  ['обратный диапазон', {from: 2, to: 1, text: 'дата'}],
  ['конец за последней клеткой', {from: 0, to: 3, text: 'дата'}],
  ['весь диапазон за лентой', {from: 3, to: 4, text: 'дата'}],
]) {
  test(`cells: ${name} отклонён`, () => {
    const chart = cells();
    chart.spans = [span];
    expectIssue(chart, 'spans.0.to', /диапазон вне cells/);
  });
}

for (const max of [0, -1, Infinity]) {
  test(`bars log: max=${max} отклонён`, () => {
    const chart = bars();
    chart.log = true;
    chart.max = max;
    expectIssue(chart, 'max', /max > 0/);
  });
}

for (const value of [-1, 101, Infinity]) {
  test(`bars log: value=${value} вне 0…max отклонён`, () => {
    const chart = bars();
    chart.log = true;
    chart.groups[1].values[0] = value;
    expectIssue(chart, 'groups.1.values.0', /0 ≤ value ≤ max/);
  });
}

test('rays: target.at по умолчанию равен нулю', () => {
  const chart = rays();
  delete chart.target.at;
  assert.equal(Plan.parse(plan(chart)).beats[0].chart.target.at, 0);
});

test('cone: подпись цели и texts имеют значения по умолчанию', () => {
  const chart = cone();
  delete chart.targetLabel;
  delete chart.texts;
  const parsed = Plan.parse(plan(chart)).beats[0].chart;
  assert.equal(parsed.targetLabel, 'противник');
  assert.deepEqual(parsed.texts, []);
});

test('ticks: offset по умолчанию равен нулю', () => {
  const chart = ticks();
  delete chart.rows[0].offset;
  assert.equal(Plan.parse(plan(chart)).beats[0].chart.rows[0].offset, 0);
});

for (const [makeChart, name, change, path, message = /./] of [
  [rays, 'нужно ровно 16 лучей', (c) => { c.rays.pop(); }, 'rays'],
  [rays, 'длина луча неотрицательна', (c) => { c.rays[0].cm = -1; }, 'rays.0.cm'],
  [rays, 'длина луча ограничена maxCm', (c) => { c.rays[0].cm = 501; }, 'rays.0.cm', /≤ maxCm/],
  [rays, 'maxCm положителен', (c) => { c.maxCm = 0; }, 'maxCm'],
  [rays, 'длительность появления положительна', (c) => { c.raysAt[1] = 0; }, 'raysAt.1'],
  [rays, 'hideAt неотрицателен', (c) => { c.hideAt = -1; }, 'hideAt'],
  [cone, 'halfDeg положителен', (c) => { c.halfDeg = 0; }, 'halfDeg'],
  [cone, 'halfDeg меньше 180', (c) => { c.halfDeg = 180; }, 'halfDeg'],
  [cone, 'rangeCm положителен', (c) => { c.rangeCm = 0; }, 'rangeCm'],
  [cone, 'path не пуст', (c) => { c.path = []; }, 'path'],
  [cone, 'время path неотрицательно', (c) => { c.path[0].t = -1; }, 'path.0.t'],
  [cone, 'времена path не повторяются', (c) => { c.path[1].t = 0; }, 'path.1.t', /строго возрастать/],
  [cone, 'времена path возрастают', (c) => { c.path[2].t = 1; }, 'path.2.t', /строго возрастать/],
  [cone, 'координаты конечны', (c) => { c.path[0].x = Infinity; }, 'path.0.x'],
  [cone, 'доворот имеет положительную длительность', (c) => { c.turn.dur = 0; }, 'turn.dur'],
  [cone, 'угол доворота конечен', (c) => { c.turn.deg = Infinity; }, 'turn.deg'],
  [ticks, 'есть хотя бы один ряд', (c) => { c.rows = []; }, 'rows'],
  [ticks, 'есть хотя бы одна клетка', (c) => { c.rows[0].cells = ''; }, 'rows.0.cells', /только D и B/],
  [ticks, 'клетки только D или B', (c) => { c.rows[0].cells = 'DBX'; }, 'rows.0.cells', /только D и B/],
  [ticks, 'offset неотрицателен', (c) => { c.rows[1].offset = -0.5; }, 'rows.1.offset'],
  [ticks, 'dur положителен', (c) => { c.dur = 0; }, 'dur'],
  [ticks, 'выравнивание не мгновенно', (c) => { c.align.dur = 0; }, 'align.dur'],
  [grid, 'есть хотя бы одна плитка', (c) => { c.tiles = []; }, 'tiles'],
  [grid, 'columns положителен', (c) => { c.columns = 0; }, 'columns'],
  [grid, 'columns целый', (c) => { c.columns = 1.5; }, 'columns'],
  [grid, 'at неотрицателен', (c) => { c.tiles[0].at = -1; }, 'tiles.0.at'],
  [flow, 'есть хотя бы один узел', (c) => { c.nodes = []; c.edges = []; }, 'nodes'],
  [flow, 'x в долях кадра', (c) => { c.nodes[0].x = 1.1; }, 'nodes.0.x'],
  [flow, 'y в долях кадра', (c) => { c.nodes[0].y = -0.1; }, 'nodes.0.y'],
  [flow, 'ширина положительна', (c) => { c.nodes[0].w = 0; }, 'nodes.0.w'],
  [flow, 'ширина не больше кадра', (c) => { c.nodes[0].w = 1.1; }, 'nodes.0.w'],
  [flow, 'id уникальны', (c) => { c.nodes[1].id = 'play'; }, 'nodes.1.id', /уникальными/],
  [flow, 'from существует', (c) => { c.edges[0].from = 'missing'; }, 'edges.0.from', /неизвестный узел/],
  [flow, 'to существует', (c) => { c.edges[0].to = 'missing'; }, 'edges.0.to', /неизвестный узел/],
  [flow, 'изгиб конечен', (c) => { c.edges[1].curve = Infinity; }, 'edges.1.curve'],
  [flow, 'период пакетов положителен', (c) => { c.edges[0].packets.every = 0; }, 'edges.0.packets.every'],
  [flow, 'длительность пакетов положительна', (c) => { c.edges[0].packets.dur = 0; }, 'edges.0.packets.dur'],
]) {
  test(`${makeChart().type}: ${name}`, () => {
    const chart = makeChart();
    change(chart);
    expectIssue(chart, path, message);
  });
}

const motionPath = [{t: 1, x: 10, y: 40}, {t: 3, x: 30, y: 0}, {t: 7, x: -10, y: 80}];

test('cone.path: координаты ограничены первым и последним ключом', () => {
  assert.deepEqual(interpolatePath(motionPath, -5), {x: 10, y: 40});
  assert.deepEqual(interpolatePath(motionPath, 10), {x: -10, y: 80});
});

test('cone.path: в ключах возвращаются их координаты', () => {
  for (const {t, x, y} of motionPath) assert.deepEqual(interpolatePath(motionPath, t), {x, y});
});

test('cone.path: линейная интерполяция между ключами', () => {
  assert.deepEqual(interpolatePath(motionPath, 2), {x: 20, y: 20});
});

test('cone.path: интервалы разной длины интерполируются по своему времени', () => {
  assert.deepEqual(interpolatePath(motionPath, 4), {x: 20, y: 20});
  assert.deepEqual(interpolatePath(motionPath, 6), {x: 0, y: 60});
});

test('cone.path: единственная точка остаётся неподвижной', () => {
  assert.deepEqual(interpolatePath([{t: 2, x: -7, y: 12}], 5), {x: -7, y: 12});
});

test('cone: взгляд направлен вверх, граница конуса включена', () => {
  assert.equal(inCone({x: 0, y: 100}, 45, 500), true);
  assert.equal(inCone({x: 100, y: 100}, 45, 500), true);
  assert.equal(inCone({x: -100, y: 100}, 45, 500), true);
  assert.equal(inCone({x: 101, y: 100}, 45, 500), false);
  assert.equal(inCone({x: 0, y: -100}, 45, 500), false);
});

test('cone: дальность ограничивает видимость независимо от угла', () => {
  assert.equal(inCone({x: 0, y: 500}, 45, 500), true);
  assert.equal(inCone({x: 0, y: 501}, 45, 500), false);
});

test('cone: положительный доворот направляет взгляд вправо', () => {
  assert.equal(inCone({x: 100, y: 0}, 20, 500, 90), true);
  assert.equal(inCone({x: 0, y: 100}, 20, 500, 90), false);
});

test('cone: видимость не разрывается при переходе через ±180°', () => {
  const point = (deg) => ({x: Math.sin(deg * Math.PI / 180) * 100, y: Math.cos(deg * Math.PI / 180) * 100});
  assert.equal(inCone(point(-179), 5, 500, 179), true);
  assert.equal(inCone(point(179), 5, 500, -179), true);
  assert.equal(inCone(point(170), 5, 500, -179), false);
});

// Independent reference: walk a dense polyline and interpolate within the
// segment containing the requested distance, without the production lookup.
function quadraticPointAtLengthReference(a, c, b, progress) {
  const points = Array.from({length: 20001}, (_, i) => {
    const t = i / 20000;
    return {
      x: a.x + 2 * t * (c.x - a.x) + t * t * (a.x - 2 * c.x + b.x),
      y: a.y + 2 * t * (c.y - a.y) + t * t * (a.y - 2 * c.y + b.y),
    };
  });
  const lengths = points.slice(1).map((p, i) => Math.hypot(p.x - points[i].x, p.y - points[i].y));
  let remaining = lengths.reduce((sum, value) => sum + value, 0) * progress;
  for (let i = 0; i < lengths.length; i++) {
    if (remaining <= lengths[i]) {
      const fraction = lengths[i] === 0 ? 0 : remaining / lengths[i];
      return {
        x: points[i].x + (points[i + 1].x - points[i].x) * fraction,
        y: points[i].y + (points[i + 1].y - points[i].y) * fraction,
      };
    }
    remaining -= lengths[i];
  }
  return points.at(-1);
}

const arcStart = {x: 400, y: 400};
const arcEnd = {x: 1520, y: 400};
const arcControl = {x: 960, y: 400 + 0.5 * 1080};

for (const [direction, a, b] of [
  ['вперёд', arcStart, arcEnd],
  ['назад', arcEnd, arcStart],
]) {
  for (const progress of [0.25, 0.5, 0.75]) {
    test(`flow F1: curve=0.5, ${direction}, progress=${progress}: наконечник в пределах 2 px от конца линии`, () => {
      const expected = quadraticPointAtLengthReference(a, arcControl, b, progress);
      const parameter = quadraticParameterAtLength(a, arcControl, b, progress);
      const tip = quadraticPoint(a, arcControl, b, parameter);
      const distance = Math.hypot(tip.x - expected.x, tip.y - expected.y);
      assert.ok(distance <= 2, `наконечник отстаёт на ${distance.toFixed(4)} px`);
      if (progress !== 0.5) {
        const naive = quadraticPoint(a, arcControl, b, progress);
        const naiveDistance = Math.hypot(naive.x - expected.x, naive.y - expected.y);
        assert.ok(naiveDistance > 2, `геометрия должна выявлять прежнюю ошибку: ${naiveDistance.toFixed(4)} px`);
      }
    });
  }
}

test('flow: доля длины ограничена началом и концом дуги', () => {
  for (const [progress, parameter] of [[-1, 0], [0, 0], [1, 1], [2, 1]]) {
    assert.equal(quadraticParameterAtLength(arcStart, arcControl, arcEnd, progress), parameter);
  }
});

test('flow: прямая стрелка сохраняет линейное движение', () => {
  const a = {x: 100, y: 200}, c = {x: 400, y: 500}, b = {x: 700, y: 800};
  for (const progress of [0, 0.25, 0.5, 0.75, 1]) {
    assert.ok(Math.abs(quadraticParameterAtLength(a, c, b, progress) - progress) < 1e-10);
  }
});

test('flow: дуга нулевой длины даёт конечный параметр и неподвижный наконечник', () => {
  const point = {x: 300, y: 600};
  for (const progress of [0, 0.25, 0.5, 0.75, 1]) {
    const parameter = quadraticParameterAtLength(point, point, point, progress);
    assert.ok(Number.isFinite(parameter) && parameter >= 0 && parameter <= 1);
    const tip = quadraticPoint(point, point, point, parameter);
    assert.ok(Math.hypot(tip.x - point.x, tip.y - point.y) < 1e-10);
  }
});

const nearPoint = (actual, expected) => {
  assert.ok(Math.abs(actual.x - expected.x) < 1e-8, `x: ${actual.x} ≠ ${expected.x}`);
  assert.ok(Math.abs(actual.y - expected.y) < 1e-8, `y: ${actual.y} ≠ ${expected.y}`);
};

test('follow: cover wide центрирует обрезку и масштабирует исходные пиксели', () => {
  const viewport = {left: 0, top: 0, width: 1920, height: 1080};
  nearPoint(transformPoint({x: 250, y: 250}, [1000, 1000], viewport), {x: 480, y: 60});
  nearPoint(transformPoint({x: 640, y: 360}, [1280, 720], viewport), {x: 960, y: 540});
});

test('follow: cover tall сохраняет центр footage и обрезает боковые края', () => {
  const viewport = {left: 0, top: 0, width: 1080, height: 1920};
  nearPoint(transformPoint({x: 960, y: 540}, [1920, 1080], viewport), {x: 540, y: 960});
  nearPoint(transformPoint({x: 1068, y: 432}, [1920, 1080], viewport), {x: 732, y: 768});
  assert.ok(transformPoint({x: 0, y: 540}, [1920, 1080], viewport).x < 0);
});

test('follow: правая половина wide split добавляет смещение после cover', () => {
  const viewport = {left: 960, top: 0, width: 960, height: 1080};
  nearPoint(transformPoint({x: 1000, y: 400}, [1920, 1080], viewport), {x: 1480, y: 400});
});

test('follow: нижняя половина tall split добавляет вертикальное смещение', () => {
  const viewport = {left: 0, top: 960, width: 1080, height: 960};
  nearPoint(transformPoint({x: 960, y: 540}, [1920, 1080], viewport), {x: 540, y: 1440});
});

test('follow: zoom применяется после cover вокруг локального origin половины split', () => {
  const viewport = {left: 960, top: 0, width: 960, height: 1080};
  const zoom = {at: 1, dur: 2, scale: 2, x: 480, y: 540};
  assert.equal(zoomScale(zoom, 0), 1);
  assert.equal(zoomScale(zoom, 2), 1.5);
  assert.equal(zoomScale(zoom, 4), 2);
  nearPoint(transformPoint({x: 1000, y: 400}, [1920, 1080], viewport, zoom, 0), {x: 1480, y: 400});
  nearPoint(transformPoint({x: 1000, y: 400}, [1920, 1080], viewport, zoom, 2), {x: 1500, y: 330});
  nearPoint(transformPoint({x: 960, y: 540}, [1920, 1080], viewport, zoom, 4), {x: 1440, y: 540});
});

test('follow: from и freeze выбирают тот же исходный кадр, что Picture', () => {
  assert.equal(sourceFrame(4, 2, 30), 180);
  assert.equal(sourceFrame(4, 2, 30, 1.5), 165);
  assert.equal(sourceFrame(4, 1, 30, 1.5), 150);
  assert.equal(sourceFrame(4, 1.5, 30, 1.5), 165);
  assert.equal(sourceFrame(4, 2, 30, 0), 120);
  // Picture rounds startFrom and the beat/freeze frame independently.
  assert.equal(sourceFrame(1.01, 0.01, 30), 30);
  assert.equal(sourceFrame(1.01, 3, 30, 0.05), 32);
});

const actionSamples = [
  {frame: 10, t: 10 / 30, action: 'approach', id: 1},
  {frame: 30, t: 1, action: 'attack_front', id: 2},
  {frame: 35, t: 35 / 30, action: 'unmapped', id: 3},
  {frame: 40, t: 40 / 30, action: 'approach', id: 1},
];
const actionNames = {approach: 'подойти', attack_front: 'удар прямо'};

test('follow: действие видно с его кадра до следующего, неназванное гасит плашку', () => {
  assert.equal(selectAction(actionSamples, actionNames, 9, 30), null);
  assert.deepEqual(selectAction(actionSamples, actionNames, 10, 30), {...actionSamples[0], name: 'подойти'});
  assert.equal(selectAction(actionSamples, actionNames, 29, 30).action, 'approach');
  assert.equal(selectAction(actionSamples, actionNames, 30, 30).name, 'удар прямо');
  assert.equal(selectAction(actionSamples, actionNames, 35, 30), null);
  assert.equal(selectAction(actionSamples, actionNames, 39, 30), null);
  assert.equal(selectAction(actionSamples, actionNames, 40, 30).name, 'подойти');
});

test('follow: hold по умолчанию 1,2 с, правая граница исключена', () => {
  assert.equal(selectAction(actionSamples, actionNames, 75, 30).name, 'подойти');
  assert.equal(selectAction(actionSamples, actionNames, 76, 30), null);
  assert.equal(selectAction(actionSamples, actionNames, 54, 30, 0.5).name, 'подойти');
  assert.equal(selectAction(actionSamples, actionNames, 55, 30, 0.5), null);
  assert.equal(selectAction(actionSamples, actionNames, 40, 30, 0), null);
  assert.equal(selectAction([], actionNames, 0, 30), null);
  assert.equal(selectAction([{frame: 0, t: 0, action: 'toString', id: 0}], {}, 0, 30), null);
});

test('follow: track сглажен без состояния, null немедленно скрывает плашку', () => {
  const track = [[0, 0, 100], [1, 20, 100], [2, 0, 100], [3, null, null], [4, 500, 200]];
  nearPoint(smoothedTrack(track, 1, 30), {x: 8, y: 100});
  assert.equal(smoothedTrack(track, 3, 30), null);
  nearPoint(smoothedTrack(track, 4, 30), {x: 500, y: 200});
  nearPoint(smoothedTrack(track, 1, 30), {x: 8, y: 100});
  assert.equal(smoothedTrack(track, -1, 30), null);
  assert.equal(smoothedTrack([], 0, 30), null);
});

test('follow: окно сглаживания не пересекает null с обеих сторон', () => {
  const track = [[0, 1000, 1000], [1, null, null], [2, 10, 20], [3, 20, 30], [4, null, null], [5, 1000, 1000]];
  nearPoint(smoothedTrack(track, 2, 30), {x: 100 / 7, y: 170 / 7});
  assert.equal(smoothedTrack(track, 4, 30), null);
  assert.equal(smoothedTrack([[0, null, null], [10, 200, 300]], 9, 30), null);
  nearPoint(smoothedTrack([[0, 100, 200], [20, 300, 400]], 10, 30), {x: 100, y: 200});
});

test('hp: берёт последнее значение по кадру, включая разреженные данные', () => {
  const hp = [[5, 300, 300], [20, 250, 180], [50, 0, 40]];
  assert.equal(sampleHp(hp, 4), null);
  assert.deepEqual(sampleHp(hp, 5), [300, 300]);
  assert.deepEqual(sampleHp(hp, 19), [300, 300]);
  assert.deepEqual(sampleHp(hp, 20), [250, 180]);
  assert.deepEqual(sampleHp(hp, 100), [0, 40]);
  assert.equal(sampleHp([], 0), null);
});

const combatBeat = () => ({
  id: 'fight', kind: 'footage', dur: 8, src: 'footage/p30-100000/duel.mp4',
  follow: {data: 'footage/p30-100000/actions.json', names: actionNames},
  hp: {data: 'footage/p30-100000/actions.json', labels: ['бандит', 'противник']},
});
const combatPlan = (beat = combatBeat(), format = 'wide') => ({
  format, lang: 'ru', hook: {text: '', until: 0}, beats: [beat],
});

test('follow/hp: схема сохраняет путь, русские имена и значения по умолчанию', () => {
  const beat = Plan.parse(combatPlan()).beats[0];
  assert.deepEqual(beat.follow, {...combatBeat().follow, at: 0, offsetY: -42, hold: 1.2});
  assert.deepEqual(beat.hp, {...combatBeat().hp, at: 0});
});

for (const format of ['wide', 'tall']) {
  test(`follow/hp: все четыре поля split сохраняются в ${format}`, () => {
    const input = {...combatBeat(), kind: 'split', srcBottom: 'footage/p30-100004/duel.mp4',
      follow: {...combatBeat().follow, at: 0.5, until: 7, offsetY: -60, hold: 0.7},
      hp: {...combatBeat().hp, at: 1},
      followBottom: {data: 'footage/p30-100004/actions.json', names: {approach: 'подойти справа'}},
      hpBottom: {data: 'footage/p30-100004/actions.json', labels: ['свой', 'противника']},
    };
    const beat = Plan.parse(combatPlan(input, format)).beats[0];
    assert.deepEqual(beat.follow, input.follow);
    assert.deepEqual(beat.hp, input.hp);
    assert.deepEqual(beat.followBottom, {...input.followBottom, at: 0, offsetY: -42, hold: 1.2});
    assert.deepEqual(beat.hpBottom, {...input.hpBottom, at: 0});
  });
}

for (const hold of [0, -0.1, Infinity]) {
  test(`follow: схема отклоняет hold=${hold} с точным путём`, () => {
    const beat = combatBeat();
    beat.follow.hold = hold;
    const result = Plan.safeParse(combatPlan(beat));
    assert.equal(result.success, false);
    assert.ok(result.error.issues.some(({path}) => path.join('.') === 'beats.0.follow.hold'));
  });
}

test('follow/hp: пустые пути data отклонены во всех полях', () => {
  const beat = {...combatBeat(), kind: 'split', srcBottom: 'other.mp4'};
  beat.followBottom = {...beat.follow};
  beat.hpBottom = {...beat.hp};
  for (const field of ['follow', 'hp', 'followBottom', 'hpBottom']) beat[field].data = '';
  const result = Plan.safeParse(combatPlan(beat));
  assert.equal(result.success, false);
  assert.deepEqual(result.error.issues.map(({path}) => path.join('.')).sort(),
    ['follow', 'hp', 'followBottom', 'hpBottom'].map((field) => `beats.0.${field}.data`).sort());
});

for (const labels of [['только свой'], ['свой', 'противника', 'лишний']]) {
  test(`hp: схема требует ровно две подписи, получено ${labels.length}`, () => {
    const beat = combatBeat();
    beat.hp.labels = labels;
    const result = Plan.safeParse(combatPlan(beat));
    assert.equal(result.success, false);
    assert.ok(result.error.issues.some(({path}) => path.join('.') === 'beats.0.hp.labels'));
  });
}

const combatData = () => ({
  fps: 30, frames: 90, size: [1920, 1080],
  track: [[0, 960, 400], [1, null, null], [2, 970, 410]],
  hp: [[0, 300, 300], [2, 280, 250]],
  actions: [{frame: 0, t: 0, action: 'approach', id: 1}],
});

test('CombatData: записанные координаты, null, HP и действия сохраняются', () => {
  assert.deepEqual(CombatData.parse(combatData()), combatData());
});

test('CombatData: только парный null допустим для головы вне кадра', () => {
  for (const tuple of [[1, null, 400], [1, 960, null]]) {
    const input = combatData();
    input.track[1] = tuple;
    const result = CombatData.safeParse(input);
    assert.equal(result.success, false);
    assert.ok(result.error.issues.some(({path}) => path.join('.') === 'track.1'));
  }
});

test('CombatData: размеры кадра должны быть положительными и конечными', () => {
  for (const size of [[0, 1080], [1920, -1], [Infinity, 1080]]) {
    const result = CombatData.safeParse({...combatData(), size});
    assert.equal(result.success, false);
    assert.ok(result.error.issues.some(({path}) => path[0] === 'size'));
  }
});

test('motion: zoom по умолчанию плавный, linear остаётся линейным', () => {
  const zoom = {at: 0, dur: 4, scale: 2, x: 0, y: 0};
  assert.ok(zoomScale(zoom, 1) < 1.25, 'начало медленнее линейного');
  assert.ok(zoomScale(zoom, 3) > 1.75, 'конец медленнее линейного');
  assert.equal(zoomScale(zoom, 2), 1.5);
  assert.equal(zoomScale({...zoom, ease: 'linear'}, 1), 1.25);
});

test('motion: переходы, входы и пути проходят схему с умолчаниями', () => {
  const plan = Plan.parse({lang: 'ru', hook: {text: 'x', until: 1}, beats: [
    {id: 'a', kind: 'still', dur: 2, src: 'a.png', texts: [{text: 'x'}]},
    {id: 'b', kind: 'still', dur: 2, src: 'b.png', enter: {type: 'slide'},
      texts: [{text: 'y', enter: 'pop'}], paths: [{points: [[0, 0], [100, 50], [200, 0]], color: '#fff', arrow: true}]},
  ]});
  assert.equal(plan.beats[0].texts[0].enter, 'rise');
  assert.equal(plan.beats[1].enter.dur, 0.5, 'slide по умолчанию дольше: едет весь кадр');
  assert.equal(plan.beats[1].paths[0].curve, true);
  const enter = (e) => Plan.parse({lang: 'ru', hook: {text: 'x', until: 1},
    beats: [{id: 'a', kind: 'still', dur: 2}, {id: 'b', kind: 'still', dur: 2, enter: e}]}).beats[1].enter;
  assert.deepEqual(enter({type: 'fade'}), {type: 'fade', dur: 0.35});
  assert.deepEqual(enter({type: 'wipe'}), {type: 'wipe', dur: 0.5});
  assert.deepEqual(enter({type: 'slide', dur: 0.3}), {type: 'slide', dur: 0.3}, 'явная длительность из плана не меняется');
  assert.throws(() => Plan.parse({lang: 'ru', hook: {text: 'x', until: 1},
    beats: [{id: 'a', kind: 'still', dur: 2, enter: {type: 'spin'}}]}));
});

const carryPlan = (carry, tag = {x: 540, y: 900, lines: [{text: 'шанс 25%'}, {text: 'бросок {n}', at: 1, count: {to: 65}}]}) => ({
  lang: 'ru', hook: {text: 'x', until: 1},
  beats: [{id: 'a', kind: 'footage', src: 'a.mp4', dur: 2, tags: [tag]}, {id: 'b', kind: 'card', dur: 3, carry}],
});
const carryIssues = (input) => {
  const result = Plan.safeParse(input);
  assert.equal(result.success, false);
  return result.error.issues.map((issue) => `${issue.path.join('.')}: ${issue.message}`);
};

test('carry: строка метки по умолчанию, со склейки, с дугой вверх', () => {
  const carry = Plan.parse(carryPlan({from: {tag: 0}, to: [300, 700]})).beats[1].carry;
  assert.deepEqual(carry, {from: {tag: 0, line: 0}, to: [300, 700], at: 0, arc: 0.15});
});

test('carry: метка и строка должны быть в бите перед склейкой', () => {
  assert.match(carryIssues(carryPlan({from: {tag: 1}, to: [0, 0]})).join(), /beats\.1\.carry\.from\.tag: нет такой метки/);
  assert.match(carryIssues(carryPlan({from: {tag: 0, line: 2}, to: [0, 0]})).join(), /from\.line: нет такой строки/);
  const late = carryPlan({from: {tag: 0}, to: [0, 0]}, {x: 0, y: 0, lines: [{text: 'поздно', at: 2}]});
  assert.match(carryIssues(late).join(), /не успевает появиться до склейки/);
  const gone = carryPlan({from: {tag: 0}, to: [0, 0]}, {x: 0, y: 0, until: 1, lines: [{text: 'ушла'}]});
  assert.match(carryIssues(gone).join(), /метка уходит до склейки/);
});

test('carry: из точки нужен text, на первом бите нечего переносить', () => {
  assert.match(carryIssues(carryPlan({from: [10, 20], to: [0, 0]})).join(), /beats\.1\.carry\.text: carry из точки: нужен text/);
  assert.equal(Plan.parse(carryPlan({from: [10, 20], to: [0, 0], text: '40%'})).beats[1].carry.text, '40%');
  const first = {lang: 'ru', hook: {text: 'x', until: 1}, beats: [{id: 'a', kind: 'card', dur: 2, carry: {from: [0, 0], to: [1, 1], text: 'x'}}]};
  assert.match(carryIssues(first).join(), /beats\.0\.carry: carry: нужен бит перед этим/);
});

test('carry: центр строки метки там, где её рисует Tags', () => {
  const measure = (text) => ({width: text.length * 30, height: 60});
  const tag = (side, lines = [{text: 'шанс 25%', at: 0}, {text: 'бросок {n}', at: 1, count: {to: 65, dur: 0.8}}]) =>
    ({x: 540, y: 900, side, lines, at: 0, enter: 'rise'});
  // box: 2 lines × 60 + 2 gap + 24 padding = 146 high, bottom at 870; widest line 'бросок 65' 270 + 44 = 314
  assert.deepEqual(tagLineCentre(tag('center'), 0, 2, measure), {x: 540, y: 870 - 146 + 12 + 30});
  assert.deepEqual(tagLineCentre(tag('center'), 1, 2, measure), {x: 540, y: 870 - 146 + 12 + 62 + 30});
  assert.deepEqual(tagLineCentre(tag('right'), 0, 2, measure).x, 540 - 36 + 157);
  assert.deepEqual(tagLineCentre(tag('left'), 0, 2, measure).x, 540 + 36 - 157);
  // a line not shown by the cut takes no room
  assert.deepEqual(tagLineCentre(tag('center'), 0, 0.5, measure), {x: 540, y: 870 - 84 + 12 + 30});
  assert.equal(settledText({text: 'бросок {n}', at: 0, count: {to: 65, dur: 0.8}}), 'бросок 65');
});

test('carry: дуга от точки к точке, середина выше прямой', () => {
  const a = {x: 0, y: 1000};
  const b = {x: 600, y: 200};
  assert.deepEqual(arcPoint(a, b, 0.15, 0), a);
  assert.deepEqual(arcPoint(a, b, 0.15, 1), b);
  const mid = arcPoint(a, b, 0.15, 0.5);
  // 0.15 × 1000 px off the chord's middle (300, 600), along the upward normal (−0.8, −0.6)
  assert.ok(Math.abs(mid.x - (300 - 120)) < 1e-9 && Math.abs(mid.y - (600 - 90)) < 1e-9, JSON.stringify(mid));
  assert.deepEqual(arcPoint(a, b, 0, 0.5), {x: 300, y: 600});
  const down = arcPoint({x: 0, y: 0}, {x: 0, y: 800}, 0.1, 0.5);
  assert.ok(down.x > 0, 'вертикальный полёт выгибается вправо');
});

test('carry: длительность растёт с расстоянием в пределах набора', () => {
  assert.equal(travelDur(100), 0.35);
  assert.equal(travelDur(300), 0.35);
  assert.ok(Math.abs(travelDur(600) - 0.35 * Math.SQRT2) < 1e-9, 'вдвое дальше — на ~40% дольше');
  assert.equal(travelDur(2000), 0.6);
});

test('push: долгая карточка, стоп-кадр и заморозка наезжают сами, медленнее при долгом стоянии', () => {
  assert.deepEqual(holdPush({kind: 'card', dur: 2}), {at: 0, dur: 2, scale: 1.03});
  assert.deepEqual(holdPush({kind: 'still', dur: 4}), {at: 0, dur: 4, scale: 1.04});
  assert.deepEqual(holdPush({kind: 'card', dur: 10}), {at: 0, dur: 10, scale: 1.05});
  assert.deepEqual(holdPush({kind: 'footage', dur: 5, freeze: 1}), {at: 1, dur: 4, scale: 1.04});
  assert.deepEqual(holdPush({kind: 'card', dur: 6, push: 0.03}), {at: 0, dur: 6, scale: 1.03});
});

test('push: живое видео, короткое стояние, свой zoom и push 0 — без наезда', () => {
  assert.equal(holdPush({kind: 'footage', dur: 8}), null);
  assert.equal(holdPush({kind: 'footage', dur: 3, freeze: 1.5}), null);
  assert.equal(holdPush({kind: 'card', dur: 1.9}), null);
  assert.equal(holdPush({kind: 'split', dur: 6}), null);
  assert.equal(holdPush({kind: 'card', dur: 6, zoom: {at: 0, dur: 6, scale: 1.1, x: 0, y: 0}}), null);
  assert.equal(holdPush({kind: 'card', dur: 6, push: 0}), null);
  const both = {lang: 'ru', hook: {text: 'x', until: 1},
    beats: [{id: 'a', kind: 'still', src: 'a.png', dur: 4, push: 0.04, zoom: {at: 0, dur: 4, scale: 1.1, x: 0, y: 0}}]};
  assert.match(carryIssues(both).join(), /beats\.0\.push: push и zoom в одном бите/);
  assert.match(carryIssues({...both, beats: [{id: 'a', kind: 'card', dur: 4, push: 0.2}]}).join(), /beats\.0\.push/);
});

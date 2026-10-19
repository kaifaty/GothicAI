import assert from 'node:assert/strict';
import test from 'node:test';
import {Plan} from '../src/schema.ts';

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

for (const makeChart of [line, big, table, cells]) {
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

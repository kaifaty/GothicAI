import json
from pathlib import Path
import stat
import struct
import zlib

import pytest
from PIL import Image

import remaster


def png_with_c2pa(path: Path, size=(64, 48), agent=b'ChatGPT', version=b'gpt-image'):
    """A real PNG plus a caBX chunk shaped like OpenAI's C2PA softwareAgent record."""
    Image.new('RGB', size, (90, 80, 60)).save(path)
    data = path.read_bytes()
    cbor = (b'msoftwareAgent\xa2' + bytes([0x64]) + b'name' + bytes([0x60 + len(agent)]) + agent
            + bytes([0x67]) + b'version' + bytes([0x60 + len(version)]) + version)
    chunk = struct.pack('>I', len(cbor)) + b'caBX' + cbor + struct.pack('>I', zlib.crc32(b'caBX' + cbor))
    path.write_bytes(data[:33] + chunk + data[33:])
    return path


@pytest.fixture
def env(tmp_path, monkeypatch):
    monkeypatch.setenv('GOTHIC_AI_DATA', str(tmp_path / 'data'))
    monkeypatch.setenv('REMASTER_ASSETS', str(tmp_path / 'assets'))
    return tmp_path


def run(*argv):
    return remaster.main([str(a) for a in argv])


def fill_intent(asset):
    path = remaster.asset_dir(asset) / 'intent.md'
    text = path.read_text()
    start = text.index('## Список сохранения')
    end = text.index('## Свойства нашего кадра')
    preserve = ('## Список сохранения\n\n1. Главная деталь одного типа на 1 и 2?\n'
                '   1a. Доля ширины совпадает ±10%?\n2. Кривизна не меньше?\n3. Мох на тех же местах?\n\n')
    path.write_text(text[:start] + preserve + text[end:])


def test_new_creates_asset_from_templates(env):
    assert run('new', 'harbour-hut', '--title', 'Хижина порта') == 0
    folder = env / 'assets/harbour-hut'
    manifest = json.loads((folder / 'asset.json').read_text())
    assert manifest['goal'] == 'the same Gothic, better made'
    assert (folder / 'intent.md').read_text().startswith('# Замысел оригинала: Хижина порта')
    assert (folder / 'reviews').is_dir()
    assert run('new', 'harbour-hut', '--title', 'again') == 1


def test_c2pa_agent_reads_name_and_version(tmp_path):
    png = png_with_c2pa(tmp_path / 'x.png')
    assert remaster.c2pa_agent(png) == {'agent': 'ChatGPT', 'version': 'gpt-image'}
    plain = tmp_path / 'plain.png'
    Image.new('RGB', (4, 4)).save(plain)
    assert remaster.c2pa_agent(plain) is None
    assert remaster.model_label(None) == 'unknown (no C2PA)'


def test_import_numbers_attempts_keeps_files_read_only(env, tmp_path):
    run('new', 'hut', '--title', 'Hut')
    prompt = tmp_path / 'prompt.txt'
    prompt.write_text('Edit of the approved concept…')
    for _ in range(2):
        source = png_with_c2pa(tmp_path / 'gen.png')
        assert run('import', 'hut', source, '--role', 'texture', '--texture', 'NW_CITY_HAFEN_HOLZWAND_01.TGA',
                   '--prompt-file', prompt, '--start', '2026-10-21T07:00:00Z', '--end', '2026-10-21T07:00:42Z',
                   '--thread', 't1') == 0
    folder = env / 'data/content-sources/hut'
    second = folder / 'NW_CITY_HAFEN_HOLZWAND_01-attempt-2.png'
    meta = json.loads(second.with_suffix('.json').read_text())
    assert meta['attempt'] == 2 and meta['seconds'] == 42 and meta['c2pa']['version'] == 'gpt-image'
    assert not second.stat().st_mode & stat.S_IWUSR
    manifest = remaster.load_manifest('hut')
    assert [s['attempt'] for s in manifest['sources']] == [1, 2]
    assert manifest['sources'][1]['model'] == 'ChatGPT gpt-image'


def test_import_keeps_model_maps_next_to_the_albedo(env, tmp_path, capsys):
    run('new', 'roofs', '--title', 'Roofs')
    prompt = tmp_path / 'prompt.txt'
    prompt.write_text('slate assembled in Blender')
    normal, rough = tmp_path / 'n.png', tmp_path / 'r.png'
    normal.write_bytes(b'normal-bytes')
    rough.write_bytes(b'rough-bytes')
    common = ['--role', 'texture', '--texture', 'NW_CITY_ROOF_04.TGA', '--prompt-file', prompt,
              '--start', '2026-10-21T07:00:00Z', '--end', '2026-10-21T07:00:01Z', '--thread', 'blender']
    assert run('import', 'roofs', png_with_c2pa(tmp_path / 'a.png'), *common, '--map', f'normal={normal}') != 0
    assert 'needs --tool' in capsys.readouterr().err
    assert run('import', 'roofs', png_with_c2pa(tmp_path / 'a.png'), *common, '--tool', 'slate_bake.py',
               '--map', f'height={normal}') != 0
    assert 'unknown map' in capsys.readouterr().err
    assert run('import', 'roofs', png_with_c2pa(tmp_path / 'a.png'), *common, '--tool', 'slate_bake.py',
               '--map', f'normal={normal}', '--map', f'roughness={rough}') == 0
    folder = env / 'data/content-sources/roofs'
    copy = folder / 'NW_CITY_ROOF_04-attempt-1.normal.png'
    assert copy.read_bytes() == b'normal-bytes' and not copy.stat().st_mode & stat.S_IWUSR
    entry = remaster.load_manifest('roofs')['sources'][0]
    assert entry['maps']['roughness'] == dict(file='roofs/NW_CITY_ROOF_04-attempt-1.roughness.png',
                                              sha256=remaster.sha256(folder / 'NW_CITY_ROOF_04-attempt-1.roughness.png'))


def test_import_warns_when_image_models_mix(env, tmp_path, capsys):
    run('new', 'hut', '--title', 'Hut')
    prompt = tmp_path / 'p.txt'
    prompt.write_text('p')
    common = ['--prompt-file', prompt, '--start', '2026-10-21T07:00:00Z', '--end', '2026-10-21T07:00:01Z',
              '--thread', 't']
    run('import', 'hut', png_with_c2pa(tmp_path / 'a.png'), '--role', 'concept', *common)
    run('import', 'hut', png_with_c2pa(tmp_path / 'b.png', version=b'gpt-image-2.5'), '--role', 'concept', *common)
    assert 'mixes image models' in capsys.readouterr().err


def test_register_existing_namespace(env):
    run('new', 'hut', '--title', 'Hut')
    folder = env / 'data/content-sources/old-hut'
    folder.mkdir(parents=True)
    png = png_with_c2pa(folder / 'NW_CITY_WOODBEAM_01-attempt-2.png')
    png.with_suffix('.json').write_text(json.dumps({'texture': 'NW_CITY_WOODBEAM_01.TGA', 'utc': 'x'}))
    concept = png_with_c2pa(folder / 'hut-elevations-attempt-1.png')
    concept.with_suffix('.json').write_text(json.dumps({'utc': 'y'}))
    assert run('register', 'hut', '--namespace', 'old-hut') == 0
    assert run('register', 'hut', '--namespace', 'old-hut') == 0
    manifest = remaster.load_manifest('hut')
    assert sorted((s['role'], s['attempt']) for s in manifest['sources']) == [('concept', 1), ('texture', 2)]
    assert 'old-hut' in manifest['namespaces']


def test_pair_blind_swaps_orders_and_keeps_key_outside(env, tmp_path):
    run('new', 'hut', '--title', 'Hut')
    fill_intent('hut')
    original = tmp_path / 'orig.png'
    Image.new('RGB', (4000, 2250), (10, 10, 10)).save(original)
    ours = tmp_path / 'ours.png'
    Image.new('RGB', (1280, 720), (200, 200, 200)).save(ours)
    assert run('pair', 'hut', '--id', 'r3-front', '--stage', 'ingame', '--mode', 'blind', '--a', original,
               '--b', ours, '--subject', 'Длинная сторона хижины') == 0
    review = env / 'data/remaster/hut/reviews/r3-front'
    one = Image.open(review / 'order-1/image-1.png')
    assert max(one.size) <= remaster.MAX_EDGE
    tokens = -(-one.width // 28) * -(-one.height // 28)
    assert tokens <= remaster.MAX_TOKENS
    assert Image.open(review / 'order-2/image-1.png').getpixel((0, 0)) == (200, 200, 200)
    packet = (review / 'order-1/packet.md').read_text()
    assert 'слепая пара' in packet and '1a. Доля ширины' in packet and 'Отклонённые дефолты' in packet
    assert 'ours' not in packet and 'orig' not in packet
    key = json.loads((review.parent / 'r3-front.key.json').read_text())
    assert key['orders']['order-1']['ours'] == 2 and key['orders']['order-2']['ours'] == 1
    assert not list(review.rglob('*.key.json'))


def test_pair_requires_filled_preserve_list(env, tmp_path):
    run('new', 'hut', '--title', 'Hut')
    image = tmp_path / 'i.png'
    Image.new('RGB', (10, 10)).save(image)
    assert run('pair', 'hut', '--id', 'r1', '--stage', 'concept', '--mode', 'reference', '--a', image,
               '--b', image, '--subject', 's') == 1


def verdict(answer, criteria, result, differs=None, image=None):
    return {'preserve': [{'id': '1', 'answer': answer, 'differs': differs}],
            'findings': [] if image is None else [{'image': image, 'severity': 'blocker', 'kind': 'preserve',
                                                   'text': 'window smaller'}],
            'criteria': criteria, 'verdict': result, 'reasons': []}


def test_merge_relabels_blind_orders():
    key = {'mode': 'blind', 'orders': {'order-1': {'ours': 2}, 'order-2': {'ours': 1}}}
    merged = remaster.merge_verdicts({
        'order-1': verdict('no', {'main_feature': '1'}, 'REJECT', differs=2, image=2),
        'order-2': verdict('no', {'main_feature': '2'}, 'REJECT', differs=1, image=1),
    }, key)
    assert merged['verdict'] == 'REJECT'
    assert merged['criteria']['main_feature'] == 'worse'
    assert merged['per_order']['order-2']['findings'][0]['image'] == 'ours'
    assert merged['per_order']['order-1']['preserve']['1']['differs'] == 'ours'


def test_merge_disagreement_is_disputed():
    key = {'mode': 'blind', 'orders': {'order-1': {'ours': 2}, 'order-2': {'ours': 1}}}
    merged = remaster.merge_verdicts({
        'order-1': verdict('yes', {'main_feature': 'same'}, 'PASS'),
        'order-2': verdict('no', {'main_feature': '2'}, 'REJECT', differs=2),
    }, key)
    assert merged['verdict'] == 'DISPUTED'
    assert merged['disputed_preserve'] == ['1']
    assert merged['criteria']['main_feature'] == 'disputed'


def test_merge_command_stores_review_in_git_folder(env, tmp_path):
    run('new', 'hut', '--title', 'Hut')
    fill_intent('hut')
    a, b = tmp_path / 'a.png', tmp_path / 'b.png'
    Image.new('RGB', (320, 180), (0, 0, 0)).save(a)
    Image.new('RGB', (320, 180), (255, 255, 255)).save(b)
    run('pair', 'hut', '--id', 'r3', '--stage', 'ingame', '--mode', 'blind', '--a', a, '--b', b, '--subject', 's')
    review = env / 'data/remaster/hut/reviews/r3'
    for name in ('order-1', 'order-2'):
        (review / name / 'verdict.json').write_text(json.dumps(verdict('yes', {'coherence': 'same'}, 'PASS')))
    assert run('merge', 'hut', '--id', 'r3') == 0
    stored = env / 'assets/hut/reviews/r3'
    assert json.loads((stored / 'merged.json').read_text())['verdict'] == 'PASS'
    assert (stored / 'pair.jpg').stat().st_size <= 400_000
    assert remaster.load_manifest('hut')['reviews'][-1]['verdict'] == 'PASS'


def test_merge_requires_every_order(env, tmp_path):
    run('new', 'hut', '--title', 'Hut')
    fill_intent('hut')
    a = tmp_path / 'a.png'
    Image.new('RGB', (32, 18)).save(a)
    run('pair', 'hut', '--id', 'r3', '--stage', 'ingame', '--mode', 'blind', '--a', a, '--b', a, '--subject', 's')
    assert run('merge', 'hut', '--id', 'r3') == 1


def test_check_walks_gates(env, tmp_path):
    run('new', 'hut', '--title', 'Hut')
    assert run('check', 'hut', '--stage', 'intent') == 1
    fill_intent('hut')
    assert run('check', 'hut', '--stage', 'concept') == 0
    assert run('check', 'hut', '--stage', 'textures') == 1
    manifest = remaster.load_manifest('hut')
    manifest['sources'].append({'file': 'hut/concept-attempt-1.png', 'model': 'm'})
    manifest['reviews'].append({'id': 'r1', 'stage': 'concept', 'verdict': 'PASS'})
    remaster.save_manifest('hut', manifest)
    run('select', 'hut', '--key', 'concept', '--file', 'hut/concept-attempt-1.png', '--reason', 'r')
    run('decide', 'hut', '--gate', 'concept', '--verdict', 'approved', '--quote', 'Да, хорошо')
    assert run('check', 'hut', '--stage', 'textures') == 0
    assert '«Да, хорошо»' in (env / 'assets/hut/decisions.md').read_text()
    run('decide', 'hut', '--gate', 'concept', '--verdict', 'changes', '--quote', 'окна меньше')
    assert run('check', 'hut', '--stage', 'textures') == 1


def test_compare_digests_reports_mismatches():
    matched, bad = remaster.compare_digests({'a.png': '1', 'b.png': '2', 'c.png': '3'},
                                            '1  a.png\nX  b.png\n')
    assert matched == 1 and bad == ['b.png', 'c.png']


def test_sheet_builds_unlabelled_grid(tmp_path):
    images = []
    for i in range(3):
        path = tmp_path / f'{i}.png'
        Image.new('RGB', (100, 50), (i * 80, 0, 0)).save(path)
        images.append(path)
    out = tmp_path / 'sheet.png'
    assert run('sheet', out, *images, '--cols', '2', '--width', '400') == 0
    assert Image.open(out).size == (400, 200)


def test_properties_and_defaults_count_only_on_ours():
    key = {'mode': 'blind', 'orders': {'order-1': {'ours': 2}, 'order-2': {'ours': 1}}}
    clean = {'preserve': [{'id': '1', 'answer': 'yes'}], 'criteria': {}, 'verdict': 'PASS'}
    # The original (image 1 in order-1, image 2 in order-2) has speckled plaster and a default; ours is clean.
    one = dict(clean, properties=[{'id': 'P1', 'image_1': 'no', 'image_2': 'yes'}],
               findings=[{'image': 1, 'severity': 'blocker', 'kind': 'default', 'text': 'lichen plaster'}])
    two = dict(clean, properties=[{'id': 'P1', 'image_1': 'yes', 'image_2': 'no'}],
               findings=[{'image': 2, 'severity': 'blocker', 'kind': 'default', 'text': 'lichen plaster'}])
    merged = remaster.merge_verdicts({'order-1': one, 'order-2': two}, key)
    assert merged['verdict'] == 'PASS'
    assert merged['per_order']['order-1']['properties']['P1'] == {'evidence': None, 'ours': 'yes', 'original': 'no'}
    # Now the same defect is on ours in both orders.
    one['findings'][0]['image'], two['findings'][0]['image'] = 2, 1
    merged = remaster.merge_verdicts({'order-1': one, 'order-2': two}, key)
    assert merged['verdict'] == 'REJECT'
    assert any('lichen' in reason for reason in merged['per_order']['order-1']['reasons'])


def test_reference_mode_treats_image_two_as_candidate():
    key = {'mode': 'reference', 'orders': {'order-1': {'ours': 2}}}
    verdict_ = {'preserve': [{'id': '1', 'answer': 'yes'}], 'criteria': {},
                'properties': [{'id': 'P1', 'image_1': 'no', 'image_2': 'yes'}],
                'findings': [{'image': 1, 'severity': 'blocker', 'kind': 'defect', 'text': 'blurry original'}]}
    assert remaster.merge_verdicts({'order-1': verdict_}, key)['verdict'] == 'PASS'
    verdict_['properties'][0]['image_2'] = 'no'
    assert remaster.merge_verdicts({'order-1': verdict_}, key)['verdict'] == 'REJECT'


def test_pair_packet_lists_properties(env, tmp_path):
    run('new', 'hut', '--title', 'Hut')
    fill_intent('hut')
    path = remaster.asset_dir('hut') / 'intent.md'
    path.write_text(path.read_text().replace('P1. …', 'P1. Заполнение читается как штукатурка, без крапа?'))
    image = tmp_path / 'i.png'
    Image.new('RGB', (32, 18)).save(image)
    run('pair', 'hut', '--id', 'r3', '--stage', 'ingame', '--mode', 'blind', '--a', image, '--b', image, '--subject', 's')
    packet = (env / 'data/remaster/hut/reviews/r3/order-1/packet.md').read_text()
    assert 'P1. Заполнение читается как штукатурка' in packet


def test_context_and_minor_defaults_do_not_reject():
    key = {'mode': 'blind', 'orders': {'order-1': {'ours': 2}}}
    verdict_ = {'preserve': [{'id': '1', 'answer': 'yes'}], 'criteria': {}, 'properties': [],
                'findings': [{'image': 2, 'severity': 'notable', 'kind': 'context', 'text': 'neighbour roof lighter'},
                             {'image': 2, 'severity': 'minor', 'kind': 'default', 'text': 'stripes on a beam'}]}
    assert remaster.merge_verdicts({'order-1': verdict_}, key)['verdict'] == 'PASS'
    verdict_['findings'][1]['severity'] = 'notable'
    assert remaster.merge_verdicts({'order-1': verdict_}, key)['verdict'] == 'REJECT'


def test_criteria_token_survives_appended_reasons():
    key = {'mode': 'blind', 'orders': {'order-1': {'ours': 2}, 'order-2': {'ours': 1}}}
    one = {'preserve': [], 'criteria': {'material': '1 — на 2 нижняя полоса стены шумит'}, 'verdict': 'PASS'}
    two = {'preserve': [], 'criteria': {'material': '2'}, 'verdict': 'PASS'}
    assert remaster.merge_verdicts({'order-1': one, 'order-2': two}, key)['criteria']['material'] == 'worse'


def test_bench_lines_put_eye_and_target_back():
    import math
    eye, target = [1354., 822., 2414.], [1366., 351., 1706.]
    lines = remaster.bench_lines('city-roofs', [{'name': 'roof-01-close', 'eye': eye, 'target': target}])
    look = float(next(l for l in lines if l.startswith('cam look')).split()[2])
    hero = [float(v) for v in next(l for l in lines if l.startswith('goto pos')).split()[2:]]
    distance, height, yaw = (float(v) for v in next(l for l in lines if l.startswith('cam world')).split()[2:])
    rebuilt = [hero[0] + distance * math.cos(math.radians(yaw)), hero[1] + height,
               hero[2] + distance * math.sin(math.radians(yaw))]
    assert rebuilt == pytest.approx(eye, abs=1e-3)
    assert [hero[0], hero[1] + look, hero[2]] == pytest.approx(target, abs=1e-3)
    assert hero[1] > max(eye[1], target[1]) and 'shot roof-01-close' in lines
    with pytest.raises(ValueError, match='unique'):
        remaster.bench_lines('a', [{'name': 'x', 'eye': eye, 'target': target}] * 2)


def test_private_reference_stays_out_of_pair_preview(env, tmp_path):
    run('new', 'hut', '--title', 'Hut')
    fill_intent('hut')
    reference = tmp_path / 'textures.png'
    Image.new('RGB', (300, 200), (255, 0, 0)).save(reference)
    concept = tmp_path / 'concept.png'
    Image.new('RGB', (300, 200), (0, 0, 255)).save(concept)
    run('pair', 'hut', '--id', 'r1', '--stage', 'concept', '--mode', 'reference', '--a', reference, '--b', concept,
        '--subject', 's', '--a-label', 'превью оригинальных текстур', '--private-a')
    review = env / 'data/remaster/hut/reviews/r1'
    assert 'эталон, превью оригинальных текстур' in (review / 'order-1/packet.md').read_text()
    remaster._pair_preview(json.loads((review.parent / 'r1.key.json').read_text()), tmp_path / 'pair.jpg')
    preview = Image.open(tmp_path / 'pair.jpg').convert('RGB')
    assert preview.size[0] < 2 * preview.size[1]
    red, green, blue = preview.getpixel((preview.width // 2, preview.height // 2))
    assert blue > 200 and red < 50


def test_material_concepts_are_named_per_texture(env, tmp_path):
    run('new', 'roofs', '--title', 'Roofs', '--kind', 'material')
    prompt = tmp_path / 'p.txt'
    prompt.write_text('slate')
    common = ['--prompt-file', prompt, '--start', '2026-10-21T10:00:00Z', '--end', '2026-10-21T10:01:00Z',
              '--thread', 't']
    run('import', 'roofs', png_with_c2pa(tmp_path / 'a.png'), '--role', 'concept',
        '--texture', 'NW_CITY_ROOF_04.TGA', *common)
    source = remaster.load_manifest('roofs')['sources'][0]
    assert source['file'] == 'roofs/concept-NW_CITY_ROOF_04-attempt-1.png'
    assert source['role'] == 'concept' and source['texture'] == 'NW_CITY_ROOF_04.TGA'
    fill_intent('roofs')
    (remaster.asset_dir('roofs') / 'concept.md').write_text('x')
    run('select', 'roofs', '--key', 'concept:NW_CITY_ROOF_04.TGA', '--file', source['file'], '--reason', 'r')
    assert not any('selected concept' in m for m in remaster.missing_for('roofs', 'textures'))


def test_queue_status_follows_the_users_gates():
    gates = lambda *pairs: {'gates': [dict(gate=g, verdict=v) for g, v in pairs], 'build': {'set': 'roofs-8'}}
    assert remaster.queue_status(gates()) == 'начат'
    assert remaster.queue_status(gates(('concept', 'changes'), ('concept', 'approved'), ('textures', 'changes'))) \
        == 'концепт принят; текстуры — правки'
    assert remaster.queue_status(gates(('concept', 'approved'), ('ingame', 'approved'), ('ingame', 'changes'))) \
        == 'концепт принят; игра — правки'
    assert remaster.queue_status(gates(('concept', 'approved'), ('enable', 'approved'))) == 'включён, набор roofs-8'
    enabled = gates(('concept', 'approved'), ('enable', 'approved'))
    enabled['gates'][-1]['set'] = 'roofs-7'                # a later build does not move what is enabled
    assert remaster.queue_status(enabled) == 'включён, набор roofs-7'


def test_queue_table_matches_the_gates(capsys):
    # every decide changes an asset's status: the queue table in the pipeline doc must follow in the same change
    assert remaster.main(['queue', '--check', str(remaster.QUEUE_DOC)]) == 0, capsys.readouterr().out

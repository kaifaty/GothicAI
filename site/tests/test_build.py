import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from gothic_site.build import Builder
from gothic_site.content import ContentError, load, load_post

SITE = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    out = tmp_path_factory.mktemp("site")
    Builder(load(SITE), out, "/GothicAI/").build()
    return out


def test_every_page_of_both_languages_is_there(built):
    site = load(SITE)
    for prefix in ("", "ru/"):
        assert (built / prefix / "index.html").is_file()
        assert (built / prefix / "posts/index.html").is_file()
        for post in site.posts:
            assert (built / prefix / f"posts/{post.slug}/index.html").is_file()
    assert (built / "404.html").is_file() and (built / ".nojekyll").is_file()
    for post in site.posts:
        for f in post.media:
            assert (built / "media" / post.slug / f.name).is_file()


def test_links_live_under_the_base_path(built):
    html = (built / "ru/posts/shorts-by-the-agent/index.html").read_text()
    assert 'href="/GothicAI/static/style.css"' in html
    assert 'src="/GothicAI/media/shorts-by-the-agent/sheet-ru.jpg"' in html  # relative in the Markdown
    assert 'href="/GothicAI/posts/shorts-by-the-agent/"' in html  # the same post in English
    assert '<html lang="ru">' in html


def test_the_feeds_are_valid_rss_with_absolute_links(built):
    for prefix in ("", "ru/"):
        root = ET.parse(built / prefix / "feed.xml").getroot()
        items = root.findall("./channel/item")
        assert items
        for item in items:
            assert item.findtext("link").startswith("https://kaifaty.github.io/GothicAI/")
            assert item.findtext("pubDate")


def test_the_home_page_shows_the_conversation_and_the_short(built):
    html = (built / "index.html").read_text()
    assert html.count('class="line ') >= 2 and "by-hero" in html and "by-npc" in html
    assert 'src="/GothicAI/static/hero/wrath-en.mp4"' in html
    ru = (built / "ru/index.html").read_text()
    assert "wrath-ru.mp4" in ru and "Ксардас" in ru


def test_the_home_page_carries_the_claim_the_steps_and_the_credit(built):
    html = (built / "index.html").read_text()
    assert "data-words>Nobody wrote Xardas" in html
    assert html.count('class="step-n"') == 5 and "<h3>Recognition</h3>" in html
    assert 'href="https://github.com/Try/OpenGothic">OpenGothic</a>' in html  # the footer's credit
    assert 'content="https://kaifaty.github.io/GothicAI/static/brand/og.jpg"' in html
    post = (built / "posts/blade-hits/index.html").read_text()
    assert 'aria-current="page"' in post and 'class="older"' in post  # the bar marks the devlog
    assert 'content="https://kaifaty.github.io/GothicAI/media/blade-hits/cover.jpg"' in post


def test_the_illustrations_are_on_the_home_page_and_named_as_such(built):
    html = (built / "index.html").read_text()
    assert 'src="/GothicAI/static/art/valley.webp"' in html  # the first screen
    assert html.count("<figure data-reveal>") == 4 and "<figcaption>Miner</figcaption>" in html
    assert "Illustrations drawn for the site" in html and "the frames of the game are real takes" in html
    for name in ("valley", "valley-2", "mage", "miner", "guard", "novice", "hunter", "parchment"):
        assert (built / "static/art" / f"{name}.webp").is_file()


def test_a_post_needs_its_front_matter(tmp_path):
    post = tmp_path / "2026-10-13-oops"
    post.mkdir()
    (post / "en.md").write_text('+++\ntitle = "x"\n+++\ntext')
    with pytest.raises(ContentError, match="summary"):
        load_post(post)
    (post / "en.md").write_text('+++\ntitle = "x"\nsummary = "y"\ndate = 2026-10-13T10:00:00\n+++\n')
    with pytest.raises(ContentError, match="time zone"):
        load_post(post)
    (post / "en.md").write_text('+++\ntitle = "x"\nsummary = "y"\ndate = 2026-10-13T10:00:00+03:00\n+++\nok')
    assert load_post(post).slug == "oops"


def test_a_youtube_short_loads_nothing_from_youtube_until_clicked(tmp_path):
    b = Builder(load(SITE), tmp_path, "/")
    html = b.player("abc123", "/static/hero/xardas.jpg", "/static/hero/xardas-en.mp4", "Play", "Xardas")
    assert 'data-id="abc123"' in html and "<button" in html
    assert "youtube" not in html and "<video" not in html  # the iframe comes from site.js on click


def test_the_privacy_page_is_linked_from_every_page(built):
    for prefix in ("", "ru/"):
        assert (built / prefix / "privacy/index.html").is_file()
    home = (built / "index.html").read_text()
    assert 'href="/GothicAI/privacy/">Privacy</a>' in home
    assert 'href="/GothicAI/ru/privacy/">' in (built / "ru/index.html").read_text()
    assert 'href="https://myaccount.google.com/permissions"' in (built / "privacy/index.html").read_text()

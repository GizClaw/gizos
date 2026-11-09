import sys
import tarfile


def test_archive_contains_complete_browser_artifact():
    with tarfile.open(sys.argv[1], "r") as archive:
        names = {name.rsplit("/", 1)[-1] for name in archive.getnames()}
        html_member = next(
            member for member in archive.getmembers()
            if member.name.rsplit("/", 1)[-1] == "index.html"
        )
        html = archive.extractfile(html_member).read().decode("utf-8")
    assert {"index.html", "index.js", "index.wasm"} <= names
    assert "pagehide" in html
    assert "freeze" in html
    assert 'id="suite"' not in html
    assert "Run all tests" in html
    assert "const fullSuite = 1 | 2 | 4 | 8" in html
    assert "executeRun(plan).catch(failBeforeStart)" in html

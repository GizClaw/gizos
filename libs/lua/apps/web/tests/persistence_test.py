"""Exercise one actual WASM catalog across reloads in a single browser origin."""

import argparse
import contextlib
import hashlib
from http.server import ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--app-id", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--expected-count", type=int, required=True)
    parser.add_argument("--preinstalled-version", default="")
    parser.add_argument("--main-observation", action="store_true")
    args = parser.parse_args()
    sys.path.insert(0, str(args.helper.resolve().parent))
    spec = importlib.util.spec_from_file_location("browser_harness", args.helper.resolve())
    harness = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(harness)
    browser = harness.find_browser()
    events = queue.Queue()

    with harness.prepared_archive(args.archive.resolve()) as root, \
            tempfile.TemporaryDirectory(dir="/tmp" if os.environ.get("H2_WEB_TEST_BROWSER") else None) as profile:
        shutil.copyfile(args.package, root / "local-test-package.tar.zlib")
        server = ThreadingHTTPServer(("127.0.0.1", 0), harness.make_handler(
            root, harness.read_header_policy(root), frozenset()))
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        to_read, to_write = os.pipe()
        from_read, from_write = os.pipe()

        def pipes():
            reader = os.dup(to_read)
            writer = os.dup(from_write)
            os.dup2(reader, 3)
            os.dup2(writer, 4)

        with (Path(profile) / "browser.log").open("w+") as log:
            process = subprocess.Popen([
                str(browser), "--headless", "--no-sandbox", "--remote-debugging-pipe",
                "--use-mock-keychain", "--autoplay-policy=no-user-gesture-required",
                "--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
                "--user-data-dir=" + profile + "/profile", "about:blank",
            ], stdout=log, stderr=log, preexec_fn=pipes, pass_fds=(3, 4))
            os.close(to_read)
            os.close(from_write)
            cdp = harness.Cdp(to_write, from_read, events)
            session = None

            def drain():
                while not events.empty():
                    event = events.get_nowait()
                    for line in harness.event_lines(event):
                        print(line, flush=True)
                        if event.get("method") in {"Runtime.exceptionThrown", "Inspector.targetCrashed"} or "Aborted(" in line:
                            raise AssertionError(line)

            def evaluate(expression, await_promise=False):
                result = cdp.send("Runtime.evaluate", {
                    "expression": expression, "returnByValue": True,
                    "awaitPromise": await_promise, "userGesture": True,
                }, session=session)
                drain()
                if "exceptionDetails" in result:
                    raise AssertionError(result["exceptionDetails"])
                return result.get("result", {}).get("value")

            def wait(expression, reason, seconds=30):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    if evaluate(expression):
                        return
                    time.sleep(0.05)
                raise AssertionError(reason)

            def start(reload=False):
                if reload:
                    evaluate("window.__lua_test_previous_document = true")
                    cdp.send("Page.reload", {"ignoreCache": True}, session=session)
                    wait("document.readyState === 'complete' && !window.__lua_test_previous_document",
                         "new document loads")
                else:
                    wait("document.readyState === 'complete' && !!document.querySelector('#start')",
                         "shell loads")
                evaluate("document.querySelector('#start').click()")
                wait("globalThis.h2WebLuaApps?.version === 1", "local catalog attaches")
                if args.main_observation:
                    wait("globalThis.h2WebState?.luaCatalogReady", "Main catalog observation")
                constants = {
                    "id": args.app_id, "version": args.version,
                    "baseCount": args.expected_count,
                    "initial": args.preinstalled_version,
                    "sha256": hashlib.sha256(args.package.read_bytes()).hexdigest(),
                    "observe": args.main_observation,
                }
                evaluate("globalThis.luaTest = " + json.dumps(constants))

            common = r"""
              const api = globalThis.h2WebLuaApps, t = globalThis.luaTest;
              const check = (ok, reason) => { if (!ok) throw new Error(reason); };
              const retry = async operation => {
                for (let n = 0; ; n++) {
                  try { return await operation(); }
                  catch (error) {
                    if (error.code !== -18 || n === 99) throw error;
                    await new Promise(resolve => setTimeout(resolve, 20));
                  }
                }
              };
              const rejects = async (operation, code) => {
                try { await operation(); } catch (error) {
                  check(code === undefined || error.code === code, 'unexpected error: ' + error);
                  return;
                }
                throw new Error('operation unexpectedly succeeded');
              };
              const list = () => retry(() => api.list());
              const current = async () => (await list()).find(app => app.app_id === t.id);
              const fixture = async () => new File(
                  [await (await fetch('/local-test-package.tar.zlib')).arrayBuffer()], 'app.lua-app.tar.zlib');
              const count = async expected => {
                check((await list()).length === expected, 'visible catalog count');
                if (t.observe) {
                  for (let n = 0; n < 300 && h2WebState.luaCatalogCount !== expected; n++)
                    await new Promise(resolve => setTimeout(resolve, 20));
                  check(h2WebState.luaCatalogCount === expected,
                        'Main and local API share the same in-memory catalog');
                }
              };
            """

            def step(script):
                return evaluate("(async () => {" + common + script + ";return true;})()", True)

            def stop():
                evaluate("document.querySelector('#stop').click()")
                wait("/result=PASS rc=0 fs=0 destroy=0/.test(document.querySelector('#status').textContent)",
                     "Runtime/IndexedDB/platform clean shutdown")
                drain()

            try:
                target = cdp.send("Target.createTarget", {"url": "about:blank"})["targetId"]
                session = cdp.send("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
                for domain in ("Runtime", "Page", "Log", "Network", "Inspector"):
                    cdp.send(domain + ".enable", session=session)
                # Local management must not require any GizClaw service.
                cdp.send("Network.setBlockedURLs", {"urls": ["*://*.gizclaw.com/*", "*://*.gizclaw.com:*/*"]}, session=session)
                cdp.send("Page.navigate", {"url": "http://127.0.0.1:" + str(server.server_address[1]) + "/"}, session=session)
                start()
                step(r"""
                  await count(t.baseCount);
                  const initial = await current();
                  check(t.initial ? initial?.version === t.initial : !initial, 'initial identity');
                  const fixtureFile = await fixture();
                  const first = api.list();
                  await rejects(() => api.list(), -18);
                  await first.catch(error => { if (error.code !== -18) throw error; });
                  const installed = await retry(() => api.install(fixtureFile, {sha256: t.sha256}));
                  check(installed.app_id === t.id && installed.version === t.version, 'install response');
                  check((await current()).version === t.version, 'installed identity shadows preinstalled');
                  await count(t.baseCount + (t.initial ? 0 : 1));
                  await rejects(() => api.install(new Uint8Array([1, 2, 3])), -15);
                  await rejects(() => api.install(fixtureFile, {sha256: '0'.repeat(64)}), -15);
                  check((await current()).version === t.version, 'failed install preserves published version');
                  await rejects(() => api.uninstall('../escape'), -1);
                """)
                stop()
                print("WEB_LUA_APPS phase=installed PASS", flush=True)

                start(reload=True)
                step(r"""
                  check((await current())?.version === t.version, 'installed version survives reload');
                  await retry(() => api.uninstall(t.id));
                  check(!(await current()), 'uninstall hides identity, including preinstalled');
                  await retry(() => api.uninstall(t.id));
                  await count(t.baseCount - (t.initial ? 1 : 0));
                """)
                stop()
                print("WEB_LUA_APPS phase=uninstalled PASS", flush=True)

                start(reload=True)
                step(r"""
                  check(!(await current()), 'uninstall survives reload');
                  let reading = false;
                  class SlowFile extends Blob {
                    slice(begin, end) {
                      const chunk = super.slice(begin, end);
                      return {arrayBuffer: async () => {
                        reading = true;
                        await new Promise(resolve => setTimeout(resolve, 2000));
                        return chunk.arrayBuffer();
                      }};
                    }
                  }
                  const fixtureFile = await fixture();
                  const pending = retry(() => api.install(new SlowFile([fixtureFile])));
                  for (let n = 0; n < 500 && !reading; n++)
                    await new Promise(resolve => setTimeout(resolve, 10));
                  check(reading, 'installation reservation reached source read');
                  document.querySelector('#stop').click();
                  await rejects(() => pending, -10);
                  await rejects(() => api.list(), -10);
                """)
                wait("/result=PASS rc=0 fs=0 destroy=0/.test(document.querySelector('#status').textContent)",
                     "Stop aborts installer and releases shared catalog")
                print("WEB_LUA_APPS phase=canceled PASS", flush=True)

                start(reload=True)
                step(r"""
                  check(!(await current()), 'canceled install never publishes');
                  const bytes = new Uint8Array(await (await fixture()).arrayBuffer());
                  await retry(() => api.install(bytes));
                  check((await current()).version === t.version, 'Uint8Array installation');
                  await retry(() => api.uninstall(t.id));
                  await retry(() => api.install(bytes.buffer));
                  check((await current()).version === t.version, 'reinstall enables hidden identity');
                  await retry(() => api.uninstall(t.id));
                """)
                stop()
                print("WEB_LUA_APPS persistence=PASS offline=PASS cleanup=PASS", flush=True)
                return 0
            finally:
                with contextlib.suppress(Exception):
                    cdp.send("Browser.close")
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                server.shutdown()
                log.seek(0)
                if process.returncode:
                    print(log.read()[-4000:], file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())

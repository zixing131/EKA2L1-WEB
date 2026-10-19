#!/usr/bin/env python3
"""Install a JAR through the library, reload persisted storage and cold-launch J9.

Requires Playwright, Chromium and serve_regression.py. Each run uses a fresh
browser context. PASS verifies persistence and a live VM after --observe;
inspect the screenshots to verify game initialization and key handling.
"""
import argparse
import asyncio
import json
import sqlite3
import tempfile
from pathlib import Path

from playwright.async_api import async_playwright


async def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    async with async_playwright() as playwright:
        browser_args = [] if args.angle_backend == 'default' else [
            '--use-gl=angle', '--use-angle=' + args.angle_backend, '--enable-unsafe-swiftshader']
        browser = await playwright.chromium.launch(headless=not args.headed, args=browser_args,
                                                   channel=args.browser_channel)
        # This test exercises installation/storage, not PWA updates. A worker
        # activating on first load can otherwise reload midway through setup.
        page = await browser.new_page(service_workers='allow' if args.service_worker else 'block')
        permission_task = None
        if args.query_choice is not None:
            async def answer_system_queries():
                count = 0
                while True:
                    dialog = page.locator('#ekaSystemQuestion')
                    if await dialog.count():
                        count += 1
                        print('System query:', await dialog.inner_text(), flush=True)
                        await page.screenshot(path=str(args.output / f'query-{count}.png'))
                        await dialog.locator('[data-choice="' + str(args.query_choice) + '"]').click()
                    await asyncio.sleep(0.2)
            permission_task = asyncio.create_task(answer_system_queries())
        profiler = None
        if args.profile:
            profiler = await page.context.new_cdp_session(page)
            await profiler.send('Profiler.enable')
            await profiler.send('Profiler.start')
        page.set_default_timeout(30000)
        with (args.output / 'console.log').open('w', buffering=1) as log:
            page_errors = []
            page.on('console', lambda message: log.write(message.text + '\n'))
            def record_error(error):
                page_errors.append(error.stack)
                log.write('PAGEERROR ' + error.stack + '\n')
            page.on('pageerror', record_error)
            try:
                await page.goto(args.url)
                await page.click('#fab')
                jar_button = page.locator('[onclick="closeSheets();pickJar()"]')
                assert await jar_button.is_visible(), 'JAR install entry is missing'
                await page.screenshot(path=str(args.output / 'install-sheet.png'))
                await page.click('[onclick="closeSheets();openDeviceSheet()"]')
                await page.set_input_files('#romInput', str(args.rom.resolve()))
                await page.set_input_files('#rpkgInput', str(args.rpkg.resolve()))
                print('ROM/RPKG selected', flush=True)
                await page.click('#deviceInstallBtn')
                await page.wait_for_function("document.getElementById('coreStatusDot').classList.contains('green')"
                                             " && localStorage.getItem('eka2l1_has_device') === '1'", timeout=240000)
                print('ROM ready', flush=True)
                if args.seed_existing_app:
                    # Model an existing host DB without needing another licensed JAR.
                    # This isolated browser context is disposable; reload before
                    # any core code uses the replaced database connection.
                    with tempfile.TemporaryDirectory() as fixture_dir:
                        fixture = Path(fixture_dir) / 'applist.db'
                        with sqlite3.connect(fixture) as db:
                            db.execute('CREATE TABLE apps(id INTEGER PRIMARY KEY AUTOINCREMENT,'
                                       'name TEXT NOT NULL,author TEXT NOT NULL,iconPath TEXT,'
                                       'title TEXT,originalTitle TEXT,version TEXT NOT NULL)')
                            db.execute("INSERT INTO apps VALUES(1,'Previous app','Fixture','','Previous app','Previous app','1.0')")
                        await page.evaluate('async bytes => { EKA2L1.module.FS.writeFile('
                                            '"/eka2l1/j2me/applist.db",new Uint8Array(bytes)); await EKA2L1.save(); }',
                                            list(fixture.read_bytes()))
                    await page.reload()
                    await page.wait_for_function("document.getElementById('coreStatusDot').classList.contains('green')", timeout=180000)
                previous_uids = {app['uid'] for app in await page.evaluate('EKA2L1.appList()') if app.get('j2me')}
                await page.click('#fab')
                async with page.expect_file_chooser() as chooser:
                    await jar_button.click()
                await (await chooser.value).set_files(str(args.jar.resolve()))
                await page.wait_for_function("EKA2L1.midletInstallStatus() < 0 || (EKA2L1.midletInstallStatus() === 2"
                                             " && document.getElementById('coreStatusDot').classList.contains('green'))", timeout=args.install_timeout * 1000)
                if args.expect_install_error:
                    assert await page.evaluate('EKA2L1.midletInstallStatus()') < 0, 'Invalid JAR reported success'
                    apps = await page.evaluate('EKA2L1.appList()')
                    assert {app['uid'] for app in apps if app.get('j2me')} == previous_uids, apps
                    assert not page_errors, page_errors
                    print('PASS: guest rejection reached the UI without adding a game card.', flush=True)
                    return
                assert await page.evaluate('EKA2L1.midletInstallStatus()') == 2, 'Guest installer rejected the JAR; see console.log'
                print('JAR installed and saved', flush=True)
                apps = await page.evaluate('EKA2L1.appList()')
                midlet = next(app for app in apps if app.get('j2me') and app['uid'] not in previous_uids)
                assert sum(app['name'] == midlet['name'] for app in apps) == 1, apps
                if args.seed_existing_app:
                    assert midlet['uid'] == 0x7E000002, midlet
                await page.screenshot(path=str(args.output / 'installed.png'))
                await page.reload()
                await page.wait_for_function("document.getElementById('coreStatusDot').classList.contains('green')", timeout=180000)
                if args.reinstall:
                    await page.set_input_files('#jarInput', str(args.jar.resolve()))
                    await page.wait_for_function("EKA2L1.midletInstallStatus() < 0 || (EKA2L1.midletInstallStatus() === 2"
                                                 " && document.getElementById('coreStatusDot').classList.contains('green'))",
                                                 timeout=args.install_timeout * 1000)
                    assert await page.evaluate('EKA2L1.midletInstallStatus()') == 2, 'Guest installer rejected the reinstall; see console.log'
                    print('Same JAR reinstalled and saved', flush=True)
                    await page.reload()
                    await page.wait_for_function("document.getElementById('coreStatusDot').classList.contains('green')", timeout=180000)
                restored = await page.evaluate('EKA2L1.appList()')
                (args.output / 'apps.json').write_text(json.dumps(restored, indent=2, ensure_ascii=False))
                assert next(app for app in restored if app.get('j2me') and app['uid'] == midlet['uid']) == midlet, restored
                assert sum(app['name'] == midlet['name'] for app in restored) == 1, restored
                if args.install_only:
                    assert not page_errors, page_errors
                    print('PASS: guest installation, persisted reload and a single MIDlet card.', flush=True)
                    return
                # Scope to the visible library; settings also contain app names.
                card = page.locator('.game-item').filter(has=page.locator(
                    '.game-item-sub', has_text='0x' + format(midlet['uid'], 'X')))
                await card.click(timeout=30000)
                await page.wait_for_url('**/run.html?**')
                print('Persisted MIDlet launched', flush=True)
                await asyncio.sleep(args.observe)
                if profiler:
                    profile = await asyncio.wait_for(profiler.send('Profiler.stop'), 15)
                    (args.output / 'cpu-profile.json').write_text(json.dumps(profile))
                runtime = await asyncio.wait_for(page.evaluate('EKA2L1.probeJavaRuntime()'), 15)
                (args.output / 'runtime.txt').write_text(runtime)
                assert any(line.startswith('j9midps60[') and 'exit=3/0' in line
                           for line in runtime.splitlines()), runtime
                assert await page.locator('#bootOverlay').evaluate("e => e.classList.contains('hidden')")
                await page.screenshot(path=str(args.output / 'game.png'), timeout=15000)
                await page.click('[onclick="toggleKeypad()"]')
                for index, key in enumerate(args.keys.split(','), 1):
                    await page.locator('[data-key="' + key + '"]').first.click(delay=300, timeout=5000)
                    await asyncio.sleep(args.key_interval)
                    await page.screenshot(path=str(args.output / f'key-{index}-{key}.png'), timeout=15000)
                after_keys = await asyncio.wait_for(page.evaluate('EKA2L1.probeJavaRuntime()'), 15)
                assert any(line.startswith('j9midps60[') and 'exit=3/0' in line
                           for line in after_keys.splitlines()), after_keys
                assert not page_errors, page_errors
                print('PASS: JAR entry, guest installation, persisted reload and live cold-started J9.')
                print('Inspect game.png and key-*.png for menu and gameplay progress.')
            finally:
                try:
                    await asyncio.wait_for(page.evaluate("EKA2L1.module.ccall('wasm_debug_dump', null, [], [])"), 15)
                    runtime = await asyncio.wait_for(page.evaluate('EKA2L1.probeJavaRuntime()'), 15)
                    (args.output / 'runtime-final.txt').write_text(runtime)
                    state = await asyncio.wait_for(page.evaluate('({install:EKA2L1.midletInstallStatus(),apps:EKA2L1.appList()})'), 15)
                    (args.output / 'state.json').write_text(json.dumps(state, indent=2, ensure_ascii=False))
                    await page.screenshot(path=str(args.output / 'final.png'), timeout=15000)
                except Exception as error:
                    print('Final screenshot failed:', error)
                if permission_task:
                    permission_task.cancel()
                    try:
                        await permission_task
                    except asyncio.CancelledError:
                        pass
                await browser.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--rpkg', type=Path, required=True)
    parser.add_argument('--jar', type=Path, required=True)
    parser.add_argument('--url', default='http://127.0.0.1:18091/index.html')
    parser.add_argument('--output', type=Path, default=Path('/tmp/eka2l1-j2me-frontend'))
    parser.add_argument('--observe', type=int, default=60)
    parser.add_argument('--install-timeout', type=int, default=180)
    parser.add_argument('--seed-existing-app', action='store_true', help='Seed an unrelated host DB entry before installing the JAR')
    parser.add_argument('--reinstall', action='store_true', help='Reinstall the same JAR after restoring persisted storage')
    parser.add_argument('--install-only', action='store_true', help='Verify installation and persistence without launching the MIDlet')
    parser.add_argument('--angle-backend', choices=('default', 'swiftshader', 'metal'), default='swiftshader')
    parser.add_argument('--browser-channel', choices=('chrome', 'msedge'), help='Use an installed browser instead of bundled Chromium')
    parser.add_argument('--headed', action='store_true', help='Exercise the normal browser window and GPU path')
    parser.add_argument('--service-worker', action='store_true', help='Include PWA activation and update handling')
    parser.add_argument('--expect-install-error', action='store_true', help='Assert guest rejection and no new MIDlet card')
    parser.add_argument('--key-interval', type=float, default=8, help='Seconds to wait after each game key')
    parser.add_argument('--keys', default='OK,LSK', help='Comma-separated keypad sequence after launch')
    parser.add_argument('--query-choice', type=int, choices=(0, 1), help='Click this system-dialog button in the isolated test browser')
    parser.add_argument('--profile', action='store_true', help='Capture a renderer CPU profile through cold launch')
    asyncio.run(run(parser.parse_args()))

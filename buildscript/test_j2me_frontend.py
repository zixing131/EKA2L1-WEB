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
        browser = await playwright.chromium.launch(headless=True, args=[
            '--use-gl=angle', '--use-angle=' + args.angle_backend, '--enable-unsafe-swiftshader'])
        page = await browser.new_page()
        page.set_default_timeout(30000)
        with (args.output / 'console.log').open('w', buffering=1) as log:
            page.on('console', lambda message: log.write(message.text + '\n'))
            page.on('pageerror', lambda error: log.write('PAGEERROR ' + str(error) + '\n'))
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
                await page.wait_for_function("EKA2L1.midletInstallStatus() === 2"
                                             " && document.getElementById('coreStatusDot').classList.contains('green')", timeout=args.install_timeout * 1000)
                print('JAR installed and saved', flush=True)
                apps = await page.evaluate('EKA2L1.appList()')
                midlet = next(app for app in apps if app.get('j2me') and app['uid'] not in previous_uids)
                if args.seed_existing_app:
                    assert midlet['uid'] == 0x7E000002, midlet
                await page.screenshot(path=str(args.output / 'installed.png'))
                await page.reload()
                await page.wait_for_function("document.getElementById('coreStatusDot').classList.contains('green')", timeout=180000)
                if args.reinstall:
                    await page.set_input_files('#jarInput', str(args.jar.resolve()))
                    await page.wait_for_function("EKA2L1.midletInstallStatus() === 2"
                                                 " && document.getElementById('coreStatusDot').classList.contains('green')",
                                                 timeout=args.install_timeout * 1000)
                    print('Same JAR reinstalled and saved', flush=True)
                    await page.reload()
                    await page.wait_for_function("document.getElementById('coreStatusDot').classList.contains('green')", timeout=180000)
                restored = await page.evaluate('EKA2L1.appList()')
                (args.output / 'apps.json').write_text(json.dumps(restored, indent=2, ensure_ascii=False))
                assert next(app for app in restored if app.get('j2me') and app['uid'] == midlet['uid']) == midlet, restored
                # Scope to the visible library; settings also contain app names.
                card = page.locator('.game-item').filter(has=page.locator(
                    '.game-item-sub', has_text='0x' + format(midlet['uid'], 'X')))
                await card.click(timeout=30000)
                await page.wait_for_url('**/run.html?**')
                print('Persisted MIDlet launched', flush=True)
                await asyncio.sleep(args.observe)
                runtime = await asyncio.wait_for(page.evaluate('EKA2L1.probeJavaRuntime()'), 15)
                (args.output / 'runtime.txt').write_text(runtime)
                assert any(line.startswith('j9midps60[') and 'exit=3/0' in line
                           for line in runtime.splitlines()), runtime
                assert await page.locator('#bootOverlay').evaluate("e => e.classList.contains('hidden')")
                await page.screenshot(path=str(args.output / 'game.png'), timeout=15000)
                await page.click('[onclick="toggleKeypad()"]')
                for key in ('OK', 'LSK'):
                    await page.locator('[data-key="' + key + '"]').first.click(delay=300, timeout=5000)
                    await asyncio.sleep(8)
                    await page.screenshot(path=str(args.output / ('key-' + key + '.png')), timeout=15000)
                print('PASS: JAR entry, guest installation, persisted reload and live cold-started J9.')
                print('Inspect game.png and key-*.png for menu and gameplay progress.')
            finally:
                try:
                    state = await asyncio.wait_for(page.evaluate('({install:EKA2L1.midletInstallStatus(),apps:EKA2L1.appList()})'), 15)
                    (args.output / 'state.json').write_text(json.dumps(state, indent=2, ensure_ascii=False))
                    await page.screenshot(path=str(args.output / 'final.png'), timeout=15000)
                except Exception as error:
                    print('Final screenshot failed:', error)
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
    parser.add_argument('--angle-backend', choices=('swiftshader', 'metal'), default='swiftshader')
    asyncio.run(run(parser.parse_args()))

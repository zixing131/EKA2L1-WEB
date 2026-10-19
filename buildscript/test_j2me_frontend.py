#!/usr/bin/env python3
"""Install a JAR through the library, reload persisted storage and cold-launch J9.

Requires Playwright, Chromium and serve_regression.py. Each run uses a fresh
browser context. PASS verifies persistence and a live VM after --observe;
inspect the screenshots to verify game initialization and key handling.
"""
import argparse
import asyncio
import json
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
                await page.click('#fab')
                async with page.expect_file_chooser() as chooser:
                    await jar_button.click()
                await (await chooser.value).set_files(str(args.jar.resolve()))
                await page.wait_for_function("EKA2L1.midletInstallStatus() === 2"
                                             " && document.getElementById('coreStatusDot').classList.contains('green')", timeout=180000)
                print('JAR installed and saved', flush=True)
                apps = await page.evaluate('EKA2L1.appList()')
                midlet = next(app for app in apps if app.get('j2me'))
                await page.screenshot(path=str(args.output / 'installed.png'))
                await page.reload()
                await page.wait_for_function("document.getElementById('coreStatusDot').classList.contains('green')", timeout=180000)
                restored = await page.evaluate('EKA2L1.appList()')
                (args.output / 'apps.json').write_text(json.dumps(restored, indent=2, ensure_ascii=False))
                assert any(app.get('j2me') and app['uid'] == midlet['uid'] for app in restored), restored
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
    parser.add_argument('--angle-backend', choices=('swiftshader', 'metal'), default='swiftshader')
    asyncio.run(run(parser.parse_args()))

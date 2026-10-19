#!/usr/bin/env python3
"""Exercise the ROM MIDP installer and IBM J9 in a fresh, nonpersistent browser.

Requires Playwright and a running serve_regression.py. ROM/RPKG/JAR are local
inputs, not redistributed test fixtures. Success verifies the launch handoff;
inspect screenshot.png and runtime.txt separately for game initialization.
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
        page.set_default_timeout(120000)
        with (args.output / 'console.log').open('w') as log:
            page.on('console', lambda message: log.write(message.text + '\n'))
            page.on('pageerror', lambda error: log.write('PAGEERROR ' + str(error) + '\n'))
            try:
                await page.goto(args.url + '?j9raw=1&autolaunch=0')
                await page.wait_for_function("typeof window.EKA2L1 === 'object'", timeout=15000)
                await page.set_input_files('#rom', str(args.rom.resolve()))
                await page.set_input_files('#rpkg', str(args.rpkg.resolve()))
                await page.set_input_files('#package', str(args.jar.resolve()))
                await page.click('#start')
                await page.wait_for_function("!document.getElementById('diagnose').disabled", timeout=args.timeout * 1000)
                state = await page.evaluate('EKA2L1.midletInstallStatus()')
                assert state == 2, f'Guest installation failed: {state}'
                apps = await page.evaluate('EKA2L1.appList()')
                (args.output / 'apps.json').write_text(json.dumps(apps, indent=2, ensure_ascii=False))
                midlet = next(app for app in apps if app.get('j2me'))
                api = 'wasm_launch_midlet'
                if args.launch_via_app_uid:
                    # The library deliberately shows only one card. Resolve
                    # the real AppArc UID from this fresh fixture's guest suite.
                    guest_uid = await page.evaluate(r'''() => {
                        const fs = EKA2L1.module.FS;
                        const child = (dir, name) => dir + '/' + fs.readdir(dir).find(
                            entry => entry.toLowerCase() === name.toLowerCase());
                        let dir = '/eka2l1/drives/c';
                        for (const name of ['private', '102033e6', 'midlets']) dir = child(dir, name);
                        const suites = fs.readdir(dir).filter(name => /^\[[0-9a-f]{8}\]$/i.test(name));
                        if (suites.length !== 1) throw new Error('Expected one installed suite: ' + suites);
                        const ids = fs.readFile(child(dir + '/' + suites[0], 'uids'));
                        return new DataView(ids.buffer, ids.byteOffset, ids.byteLength).getUint32(4, true);
                    }''')
                    midlet = {'uid': guest_uid}
                    api = 'wasm_launch_app'
                for _ in range(30):
                    result = await page.evaluate("args => EKA2L1.module.ccall(args.api, 'number', ['number'], [args.uid])",
                                                 {'api': api, 'uid': midlet['uid']})
                    if result == 0:
                        break
                    assert result in (-100, -101), f'MIDlet launch failed: {result}'
                    await page.wait_for_timeout(1000)
                assert result == 0, 'AMS launch handoff timed out'
                await asyncio.sleep(args.observe)
                runtime = await asyncio.wait_for(page.evaluate('EKA2L1.probeJavaRuntime()'), 15)
                (args.output / 'runtime.txt').write_text(runtime)
                assert any(line.startswith('j9midps60[') and 'exit=3/0' in line for line in runtime.splitlines()), runtime
                print('PASS: guest installation committed; AMS launched a live IBM J9 process.')
                print('Inspect runtime.txt and screenshot.png for game initialization.')
            finally:
                try:
                    await asyncio.wait_for(page.evaluate("window.EKA2L1?.module?.ccall('wasm_debug_dump', null, [], [])"), 15)
                    await asyncio.wait_for(page.screenshot(path=str(args.output / 'screenshot.png'), full_page=True), 15)
                except Exception as error:
                    print(f'Diagnostic capture failed: {error}')
                finally:
                    log.flush()
                    await browser.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--rpkg', type=Path, required=True)
    parser.add_argument('--jar', type=Path, required=True)
    parser.add_argument('--url', default='http://127.0.0.1:18091/smoke.html')
    parser.add_argument('--output', type=Path, default=Path('/tmp/eka2l1-j2me'))
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--observe', type=int, default=60)
    parser.add_argument('--angle-backend', choices=('swiftshader', 'metal'), default='swiftshader',
                        help='Use metal on macOS to compare hardware rendering with SwiftShader')
    parser.add_argument('--launch-via-app-uid', action='store_true', help='Exercise the actual AppArc UID entry point')
    asyncio.run(run(parser.parse_args()))

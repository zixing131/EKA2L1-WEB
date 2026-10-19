#!/usr/bin/env python3
"""Exercise the Web system dialog in a real browser without booting a ROM."""
import asyncio
from pathlib import Path
from playwright.async_api import async_playwright


async def run():
    source = (Path(__file__).resolve().parents[1] / 'src/emu/web/src/platform_stubs.cpp').read_text()
    body = source.split('EM_JS(void, web_show_question,', 1)[1].split('), {', 1)[1].split('\n});', 1)[0]
    async with async_playwright() as playwright:
        browser = await playwright.chromium.launch()
        page = await browser.new_page()
        await page.set_content('<button id="guest">Guest</button>')
        await page.evaluate('''body => {
            window.Module = {answers: [], _wasm_answer_question: (id, choice) => Module.answers.push([id, choice])};
            window.UTF8ToString = value => value;
            window.showQuestion = new Function('id', 'message', 'first', 'second', body);
            window.guestKeys = 0;
            document.addEventListener('keydown', () => guestKeys++);
            window.frames = 0;
            function tick() { frames++; requestAnimationFrame(tick); }
            requestAnimationFrame(tick);
            document.getElementById('guest').focus();
            showQuestion(1, '<img src=x onerror="throw 1">\\n允许联网？', '允许', '拒绝');
            showQuestion(2, 'Second', 'Yes', 'No');
        }''', body)
        dialog = page.locator('#ekaSystemQuestion')
        assert await dialog.count() == 1
        assert await dialog.locator('img').count() == 0
        assert '允许联网？' in await dialog.inner_text()
        await page.wait_for_function('frames > 2')
        assert await page.locator('[data-choice="1"]').evaluate('e => e === document.activeElement')
        await dialog.locator('[data-choice="0"]').click()
        assert await dialog.count() == 1
        assert 'Second' in await dialog.inner_text()
        await page.keyboard.press('Enter')
        assert await dialog.count() == 0
        await page.evaluate("showQuestion(3, 'Cancel me', 'Yes', 'No')")
        await page.keyboard.press('Escape')
        assert await dialog.count() == 0
        assert await page.evaluate('Module.answers') == [[1, 0], [2, 1], [3, -1]]
        assert await page.evaluate('guestKeys') == 0
        assert await page.locator('#guest').evaluate('e => e === document.activeElement')
        await browser.close()
        print('PASS: queued dialogs, explicit choices, Escape, text escaping, focus and live animation frames')


if __name__ == '__main__':
    asyncio.run(run())

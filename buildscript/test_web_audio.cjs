/* Exercise the actual Emscripten fallback glue with a controlled audio clock. */
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/emu/web/src/web_audio.cpp', 'utf8');
function harness() {
  const nodes = [];
  class AudioContext {
    currentTime = 0;
    state = 'running';
    destination = {};
    createBuffer(channels, frames, rate) {
      return {duration: frames / rate, getChannelData: () => new Float32Array(frames)};
    }
    createBufferSource() {
      const node = {connect() {}, disconnect() {}, stop() { this.stopped = true; },
        start(time, offset) { this.startTime = time; this.offset = offset; }};
      nodes.push(node);
      return node;
    }
  }
  const ctx = vm.createContext({AudioContext, console, HEAP16: new Int16Array(4096),
    document: {addEventListener() {}}, Set});
  ctx.window = ctx;
  function bind(name, params) {
    const from = source.indexOf(', {', source.indexOf(', ' + name + ',')) + 3;
    const body = source.slice(from, source.indexOf('\n});', from));
    ctx[name] = vm.runInContext('(function(' + params + ') {' + body + '\n})', ctx);
  }
  bind('ek_webaudio_init', '');
  bind('ek_webaudio_queue', 'id,ptr,frames,channels,rate,vol');
  bind('ek_webaudio_position', 'id');
  bind('ek_webaudio_pause', 'id,paused');
  bind('ek_webaudio_drop', 'id');
  ctx.ek_webaudio_init();
  return {ctx, nodes};
}
test('fallback playhead follows consumed PCM and pause retains queued samples', () => {
  const {ctx, nodes} = harness();
  ctx.ek_webaudio_queue(1, 0, 1000, 1, 1000, 1);
  assert.equal(ctx.ek_webaudio_position(1), 0);
  ctx.__ekAudio.ctx.currentTime = 0.53;
  assert.equal(ctx.ek_webaudio_position(1), 0.5);
  ctx.ek_webaudio_pause(1, 1);
  assert.equal(nodes[0].stopped, true);
  ctx.__ekAudio.ctx.currentTime = 10;
  assert.equal(ctx.ek_webaudio_position(1), 0.5);
  ctx.ek_webaudio_pause(1, 0);
  assert.equal(nodes[1].offset, 0.5);
  ctx.__ekAudio.ctx.currentTime = 10.53;
  assert.equal(ctx.ek_webaudio_position(1), 1);
  ctx.ek_webaudio_drop(1);
  assert.equal(ctx.__ekAudio.streams[1], undefined);
});
test('dropping output cancels all scheduled sources on seek or destruction', () => {
  const {ctx, nodes} = harness();
  ctx.ek_webaudio_queue(1, 0, 1000, 1, 1000, 1);
  ctx.ek_webaudio_queue(1, 0, 1000, 1, 1000, 1);
  ctx.ek_webaudio_drop(1);
  assert.ok(nodes.every(node => node.stopped));
});
test('worklet pause preserves samples and stop discards queued PCM before restart', () => {
  const begin = source.indexOf("    var PROC_SRC = ") + '    var PROC_SRC = '.length;
  const end = source.indexOf(';\n\n    A.loadWorklet', begin);
  const processorSource = vm.runInNewContext(source.slice(begin, end));
  let Processor;
  vm.runInNewContext(processorSource, {AudioWorkletProcessor: class {}, sampleRate: 1000,
    registerProcessor: (_, type) => { Processor = type; }, Atomics, Int32Array, Int16Array, Float32Array});
  const sab = new SharedArrayBuffer(4096), ctrl = new Int32Array(sab, 0, 4);
  const ring = new Int16Array(sab, 16, 1000);
  const processor = new Processor({processorOptions: {sab, ctrl: 0, ring: 16, ringLen: 1000, ch: 1, rate: 1000}});
  ctrl[0] = 100; ctrl[2] = 65536; ring.fill(1000);
  const render = () => { const pcm = new Float32Array(8); processor.process([], [[pcm]]); return pcm; };
  assert.ok(render().some(value => value > 0));
  const pausedAt = ctrl[1];
  ctrl[3] = 2;
  assert.ok(render().every(value => value === 0));
  assert.equal(ctrl[1], pausedAt);
  ctrl[3] = 6;
  render();
  assert.equal(ctrl[1], 100);
  assert.equal(ctrl[3], 2);
  ctrl[0] = 200; ring.fill(-1000, 100, 200); ctrl[3] = 0;
  assert.ok(render().every(value => value < 0));
});

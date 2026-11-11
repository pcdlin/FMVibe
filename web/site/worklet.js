// The demo's audio side: the synth (fmvibe-dsp.wasm, built from web/dsp.cpp) in an AudioWorklet. It takes the
// editor's edits and notes from main.js and sends back what the editor animates, a few dozen times a second.

const TELEMETRY_SECONDS = 0.016; // about once per frame of the editor

class FMVibeProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const {wasm, values, oversampling, tempo} = options.processorOptions;
    // Compiling here, off the main thread, may be synchronous. The module expects nothing it needs from
    // outside; whatever it imports (a note that memory grew) gets a function that does nothing.
    const module = new WebAssembly.Module(wasm);
    const imports = {};
    for (const {module: from, name, kind} of WebAssembly.Module.imports(module))
      if (kind === 'function')
        (imports[from] ??= {})[name] = () => 0;
    this.dsp = new WebAssembly.Instance(module, imports).exports;
    this.dsp._initialize(); // static constructors
    this.paramCount = this.dsp.dsp_param_count();
    this.valuesView().set(values);
    this.dsp.dsp_init(sampleRate, oversampling);
    this.dsp.dsp_tempo(tempo);

    this.telemetrySize = this.dsp.dsp_telemetry_size();
    this.spare = [new ArrayBuffer(this.telemetrySize), new ArrayBuffer(this.telemetrySize)]; // main.js hands them back
    this.blocksPerTelemetry = 1;
    this.blocks = 0;
    this.memory = null;
    this.frames = 0;
    this.port.onmessage = (e) => this.receive(e.data);
    this.port.postMessage({type: 'ready', telemetrySize: this.telemetrySize, paramCount: this.paramCount});
  }

  valuesView() {
    return new Float64Array(this.dsp.memory.buffer, this.dsp.dsp_values(), this.paramCount);
  }

  receive(message) {
    switch (message.type) {
      case 'params': {
        const changes = message.changes; // index, value, index, value...
        for (let i = 0; i < changes.length; i += 2)
          this.dsp.dsp_set(changes[i], changes[i + 1]);
        break;
      }
      case 'all':
        this.valuesView().set(message.values);
        break;
      case 'note':
        this.dsp.dsp_note(message.key, message.on ? 1 : 0, message.velocity ?? 100);
        break;
      case 'tempo':
        this.dsp.dsp_tempo(message.bpm);
        break;
      case 'oversampling':
        this.dsp.dsp_set_oversampling(message.factor);
        break;
      case 'scope':
        this.dsp.dsp_scope(message.op, message.cycles);
        break;
      case 'buffer':
        this.spare.push(message.buffer);
        break;
    }
  }

  // Views on the module's memory: the output's as long as a block, and the telemetry's. Made again only when the
  // memory grew or the block's length changed, so a block makes no garbage on the audio thread.
  views(frames) {
    if (this.memory === this.dsp.memory.buffer && this.frames === frames)
      return;
    this.memory = this.dsp.memory.buffer;
    this.frames = frames;
    this.left = new Float32Array(this.memory, this.dsp.dsp_left(), Math.min(frames, 4096));
    this.right = new Float32Array(this.memory, this.dsp.dsp_right(), Math.min(frames, 4096));
    this.telemetryBytes = null; // made when the telemetry's place is known
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const frames = out[0].length;
    this.dsp.dsp_render(frames);
    this.views(frames);
    out[0].set(this.left);
    if (out.length > 1)
      out[1].set(this.right);

    this.blocksPerTelemetry = Math.max(1, Math.round(TELEMETRY_SECONDS * sampleRate / frames));
    if (++this.blocks >= this.blocksPerTelemetry && this.spare.length > 0) {
      this.blocks = 0;
      const buffer = this.spare.pop();
      const at = this.dsp.dsp_telemetry();
      this.views(frames);
      if (!this.telemetryBytes || this.telemetryBytes.byteOffset !== at)
        this.telemetryBytes = new Uint8Array(this.memory, at, this.telemetrySize);
      new Uint8Array(buffer).set(this.telemetryBytes);
      this.port.postMessage({type: 'telemetry', buffer}, [buffer]);
    }
    return true;
  }
}

registerProcessor('fmvibe', FMVibeProcessor);

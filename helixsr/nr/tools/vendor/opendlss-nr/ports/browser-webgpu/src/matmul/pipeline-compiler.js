// Compiling the several hundred specialized GEMM pipelines without stalling the queue.
//
// Specializing shapes and strides into pipeline-creation overrides means one pipeline per distinct matrix,
// which is around two hundred for this network. `createComputePipeline` defers the driver's work onto the
// device timeline, so the first frame then blocks that timeline for seconds. Compiling off the queue instead,
// with bounded concurrency rather than two hundred driver jobs at once, moves that cost to load time.

const compilers = new WeakMap();
const objectIds = new WeakMap();
let nextId = 1;

function id(value) {
  if (value == null || (typeof value !== 'object' && typeof value !== 'function')) return String(value);
  if (!objectIds.has(value)) objectIds.set(value, nextId++);
  return objectIds.get(value);
}

export function compileComputePipeline(device, descriptor) {
  let compiler = compilers.get(device);
  if (!compiler) {
    compilers.set(device, compiler = { active: 0, jobs: [], cache: new Map(),
      concurrency: Math.max(2, Math.min(8, Math.floor((globalThis.navigator?.hardwareConcurrency ?? 4) / 2))) });
  }
  const key = descriptor.compute
    ? `${id(descriptor.compute.module)}:${id(descriptor.layout)}:${descriptor.compute.entryPoint}:` +
      JSON.stringify(Object.entries(descriptor.compute.constants ?? {}).sort())
    : null;
  if (key && compiler.cache.has(key)) return compiler.cache.get(key);
  const task = new Promise((resolve, reject) => {
    compiler.jobs.push({ descriptor, resolve, reject });
    pump(device, compiler);
  });
  if (key) {
    compiler.cache.set(key, task);
    task.catch(() => { if (compiler.cache.get(key) === task) compiler.cache.delete(key); });
  }
  return task;
}

function pump(device, compiler) {
  while (compiler.active < compiler.concurrency && compiler.jobs.length) {
    const job = compiler.jobs.shift();
    compiler.active += 1;
    Promise.resolve().then(() => device.createComputePipelineAsync(job.descriptor))
      .then(job.resolve, job.reject)
      .finally(() => { compiler.active -= 1; pump(device, compiler); });
  }
}

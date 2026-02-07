// Number of sweep counts
// TODO (Exercise 2-1): Choose an appropriate value!
let P = 2;

// Number of elements in your trace
let K = 5 * 1000 / P; 

// Array of length K with your trace's values
let T;

// Value of performance.now() when you started recording your trace
let start;

function record() {
  // Create empty array for saving trace values
  T = new Array(K);

  // Fill array with -1 so we can be sure memory is allocated
  T.fill(-1, 0, T.length);

  // Save start timestamp
  start = performance.now();

  // TODO (Exercise 2-1): Record data for 5 seconds and save values to T.
  
  // Allocate a large buffer for sweeping
  let BUF_SIZE = 8 * 1024 * 1024; // ~8MB
  let buf = new Uint8Array(BUF_SIZE);

  // Index into trace array
  let idx = 0;

  // Record for ~5 seconds
  while (performance.now() - start < 5000 && idx < K) {
    let windowStart = performance.now();
    let count = 0;

    // Count how many sweeps fit into one P-ms window
    while (performance.now() - windowStart < P) {
      for (let i = 0; i < buf.length; i += 64) {
        buf[i] ^= 1;
      }
      count++;
    }

    T[idx] = count;
    idx++;
  }
  // Once done recording, send result to main thread
  postMessage(JSON.stringify(T));
}

// DO NOT MODIFY BELOW THIS LINE -- PROVIDED BY COURSE STAFF
self.onmessage = (e) => {
  if (e.data.type === "start") {
    setTimeout(record, 0);
  }
};

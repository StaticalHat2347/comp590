// part3/worker.js

// Window size in ms (keep same as Part 2 for fair comparison)
let P = 2;

// Number of elements in your trace (5 seconds total)
let K = (5 * 1000) / P;

// Array of length K with your trace's values
let T;

// Value of performance.now() when you started recording your trace
let start;

function record() {
  // Create empty array for saving trace values
  T = new Array(K);

  // Fill array with -1 so we can be sure memory is allocated (outside hot loop)
  T.fill(-1, 0, T.length);

  // Save start timestamp
  start = performance.now();

  // Index into trace array
  let idx = 0;

  // Record for ~5 seconds
  while (performance.now() - start < 5000 && idx < K) {
    let windowStart = performance.now();
    let count = 0;

    // HOT LOOP: only an add operation (incrementing a counter)
    while (performance.now() - windowStart < P) {
      count = count + 1; // (equivalently: count++)
    }

    T[idx] = count;
    idx++;
  }

  // Send trace back to main thread
  postMessage(JSON.stringify(T.slice(0, idx)));

}

// DO NOT MODIFY BELOW THIS LINE -- PROVIDED BY COURSE STAFF
self.onmessage = (e) => {
  if (e.data.type === "start") {
    setTimeout(record, 0);
  }
};

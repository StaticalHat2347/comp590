const runs = 10;

// Optional Check for Browser
// console.log("Hello World");

function measureOneLine() {
  const LINE_SIZE = 8; // originally 16; local machien is 64/sizeof(double) Note that js treats all numbers as double
  let result = [];

  // Fill with -1 to ensure allocation
  const M = new Array(runs * LINE_SIZE).fill(-1);

  for (let i = 0; i < runs; i++) {
    const start = performance.now();
    let val = M[i * LINE_SIZE];
    const end = performance.now();

    result.push(end - start);
  }

  return result;
}

function measureNLines(N) { // N is the number of cache lines to read
  const LINE_SIZE = 8 // 64/sizeof(double) - performing on x64 processor on local machine 
  let result = [];

  // M has to increase in size for the multiple lines of cache stated in N
  const M = new Array(runs * LINE_SIZE * N).fill(-1);  
  for(let i = 0; i < runs; i+=1) {
    const start = performance.now();
    
    for(let j = 0; j < N; j += 1) {
      // M[i * LINE_SIZE * N] is the starting point for each iteration, and (j * LINE_SIZE) is the address of the items stored in one "line"
      let val = M[(i * LINE_SIZE * N) + (j * LINE_SIZE)]; 
    }
    const end = performance.now();
    result.push(end - start);
  }

  return result;
}


document.getElementById(
  "exercise1-values"
).innerText = `1 Cache Line: [${measureOneLine().join(", ")}]`;

document.getElementById(
  "exercise2-values"
  // The input will be the number of cache lines
).innerText = `N Cache Lines: [${measureNLines(1).join(", ")}]`;

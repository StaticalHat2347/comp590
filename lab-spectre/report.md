## 1-1

**Given the attack plan above, how many addresses need to be flushed in the first step?**

256 addresses need to be flushed. The shared memory region has 256 pages, one for each possible byte value from 0 to 255. In Part 1, the kernel reads one secret byte and uses that byte as an index into the shared-memory page array, so the attacker must flush one address from each of the 256 pages before invoking the kernel. After the kernel touches exactly one of those pages, the attacker reloads all 256 addresses and identifies the cached page by its lower access time.


## 1-2

**Now assume the attacker and victim no longer share a memory region. Would your attack still work? If not, what changes would you need to make to make it work?**

No, this Flush+Reload attack would not work if the attacker and victim no longer shared memory. Flush+Reload depends on both parties accessing the same physical cache lines, so that the attacker can flush a shared line, let the victim access it, and then detect the victim's access by measuring a faster reload time. Without shared memory, the victim's access would affect only its own private cache lines, and the attacker would have no direct line to reload and measure. To make the attack work without shared memory, we would need to switch to a different cache side channel such as Prime+Probe or Evict+Reload, where the attacker infers the victim's accesses indirectly by monitoring cache sets instead of shared cache lines.

## 2-1

**Copy your code in `run_attacker` from `attacker-part1.c` to `attacker-part2.c`. Does your Flush+Reload attack from Part 1 still work? Why or why not?**



## 2-3

**In our example, the attacker tries to leak the values in the array `secret_part2`. In a real-world attack, attackers can use Spectre to leak data located in an arbitrary address in the victim's space. Explain how an attacker can achieve such leakage.**



## 2-4

**Experiment with how often you train the branch predictor. What is the minimum number of times you need to train the branch (i.e. `if offset < part2_limit`) to make the attack work?**



## 3-2

**Describe the strategy you employed to extend the speculation window of the target branch in the victim.**

The idea was to deliberately slow down how quickly the bounds check resolves while keeping the branch predictor heavily skewed toward taking the branch. To do this, I repeatedly trained the branch using in-bounds offsets so the CPU would speculatively execute the if body. Then, I attempted to evict the cache line containing part3_limit before triggering the out-of-bounds access, making the limit fetch slower. This delay allowed the speculative execution path to run longer, even though the victim now operates with a reduced default window, an added false dependency on the load, and a TLB miss during the shared-memory access. I also simplified the measurement phase and ran the attack multiple times, selecting the most frequently observed cache hit as the leaked byte.


## 3-3

**Assume you are an attacker looking to exploit a new machine that has the same kernel module installed as the one we attacked in this part. What information would you need to know about this new machine to port your attack? Could it be possible to determine this infomration experimentally? Briefly describe in 5 sentences or less.**

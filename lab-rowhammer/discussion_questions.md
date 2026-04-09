
# Part 1: Virtual to Physical Address Translation


## 1-1 Exercise: `virt_to_phys`

To translate a virtual address to a physical address, I first compute the **virtual page number (VPN)** by dividing the virtual address by the 4KB page size (or equivalently shifting right by 12 bits). Since Linux pagemap stores one entry per 4KB virtual page, this VPN identifies which pagemap entry corresponds to the address.

After reading the pagemap entry, I check whether the page is present in memory by verifying that bit 63 is set. If the page is present, bits 0–54 contain the **physical page number (PPN)**.

To reconstruct the full physical address, I preserve the lower 12-bit page offset from the original virtual address and combine it with the physical page number shifted left by 12 bits:

[
\text{Physical Address} = (\text{PPN} \ll 12) ;|; \text{Offset}
]

This works because address translation changes only the page number; the page offset remains unchanged.


## 1-2 Discussion Question

### In a 64-bit system using 4KB pages, which bits represent page offset vs page number?

A 4KB page is:

[
4KB = 2^{12} \text{ bytes}
]

Therefore:

* **Bits 0–11** represent the **page offset**
* **Bits 12–63** represent the **page number**


### In a 64-bit system using 2MB pages, which bits represent page offset vs page number?

A 2MB hugepage is:

[
2MB = 2^{21} \text{ bytes}
]

Therefore:

* **Bits 0–20** represent the **page offset**
* **Bits 21–63** represent the **page number**


### In a 2GB buffer, how many 2MB hugepages are there?

[
2GB = 2048MB
]

[
2048MB / 2MB = 1024
]

So the 2GB buffer contains:

**1024 hugepages**


## 1-3 Exercise: `setup_PPN_VPN_map`

To support physical-to-virtual translation, I constructed a reverse page table mapping **Physical Page Numbers (PPN)** to **Virtual Page Numbers (VPN)**.

Since the allocated memory region uses **2MB hugepages**, I iterate through the 2GB allocated memory region in increments of 2MB. For each hugepage:

1. Compute its virtual address
2. Translate that virtual address to physical using `virt_to_phys()`
3. Extract the virtual hugepage number and physical hugepage number
4. Insert the mapping into `PPN_VPN_map`

This creates a reverse lookup table for physical-to-virtual translation.


## 1-4 Exercise: `phys_to_virt`

To translate a physical address back to a virtual address:

1. Extract the **physical hugepage number** by dividing by 2MB
2. Extract the **offset within the hugepage** using modulo 2MB
3. Look up the corresponding virtual hugepage number in `PPN_VPN_map`
4. Reconstruct the virtual address:

[
\text{Virtual Address} = (\text{VPN} \times 2MB) + \text{Offset}
]

This works because virtual and physical addresses preserve the same page offset during translation.


# Part 2: DRAM Geometry – Finding Bank Conflicts


## Why Adjacent DRAM Rows Are Not a Constant Physical Stride Apart

Adjacent DRAM rows are not always separated by a constant physical stride because physical memory addresses are **not mapped linearly** to DRAM rows.

Instead, DRAM coordinates are determined by mapping physical address bits into:

* DIMM ID
* Channel ID
* Rank ID
* Bank ID
* Row ID
* Column ID

The **Bank ID** is computed using proprietary XOR functions over selected physical address bits. Because of this XOR-based scrambling, incrementing a physical address may change the bank rather than simply moving to the next row.

Thus, two physically adjacent DRAM rows must have:

* Same DIMM / Channel / Rank / Bank
* Row IDs differing by 1

But these rows may appear non-contiguous in physical address space.


## 2-1 Discussion Question

### Given Victim Address `0x96ec3000`, What Is Its Row ID?

Row ID uses **bits 17–31**.

Compute:

[
\text{Row ID} = (0x96ec3000 >> 17)
]

[
= 0x4B76
]

So:

**Row ID = 0x4B76**


### What Is Its Column ID?

Column ID uses **bits 0–12**.

Compute:

[
\text{Column ID} = 0x96ec3000 ;&; 0x1FFF
]

[
= 0x1000
]

So:

**Column ID = 0x1000**


### List All Possible Attacker Addresses One Row Below with Same Column and Bank

To move to the next DRAM row:

* Increment Row ID by 1
* Preserve Column ID
* Bank ID must remain same

However, Bank ID is unknown and depends on XORs of bits 7–20.

Because there are 4 unknown bank bits:

[
2^4 = 16
]

There are **16 possible attacker addresses** that may preserve the same bank.

These correspond to adding:

[
2^{17} = 0x20000
]

to move down one row, then varying bits 13–16 (the bank-selection-related bits that may affect XOR outputs).

Thus the 16 candidates are:

```text
0x96ee3000
0x96ee5000
0x96ee7000
0x96ee9000
0x96eeb000
0x96eed000
0x96eef000
0x96ef1000
0x96ef3000
0x96ef5000
0x96ef7000
0x96ef9000
0x96efb000
0x96efd000
0x96eff000
0x96f01000
```

(These preserve column and enumerate all possible bank-XOR combinations.)


## 2-2 Exercise: `measure_bank_latency`

To detect whether two addresses map to the same DRAM bank, I implemented a timing side-channel measurement.

Procedure:

1. Flush both addresses from cache using `_mm_clflush()`
2. Insert memory fence with `_mm_mfence()`
3. Read timestamp counter
4. Access both addresses back-to-back
5. Read timestamp counter again
6. Return elapsed cycles

If both addresses map to the same bank but different rows:

* DRAM row buffer conflict occurs
* Access latency increases significantly

If they map to different banks:

* Requests proceed in parallel
* Latency remains lower


## 2-3 Discussion Question

### Observed Timing Distribution

My timing histogram showed two distinct latency clusters:

* **Fast accesses:** approximately **268–300 cycles**
* **Slow accesses:** approximately **444–452 cycles**

This indicates successful separation between:

* Non-conflicting accesses
* Same-bank row-buffer conflicts


### Chosen Threshold for Detecting Bank Conflicts

A threshold between the two clusters is appropriate.

I selected:

[
\boxed{370 \text{ cycles}}
]

Decision rule:

* **Latency < 370 cycles:** likely different banks
* **Latency ≥ 370 cycles:** likely same bank / bank conflict

This threshold cleanly separates the two observed timing populations.



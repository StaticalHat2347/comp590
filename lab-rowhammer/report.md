## 1-2

**In a 64-bit system using 4KB pages, which bits are used to represent the page offset, and which are used to represent the page number?**

**How about for a 64-bit system using 2MB pages? Which bits are used for page number and which are for page offset?**

**In a 2GB buffer, how many 2MB hugepages are there?**

### 64-bit system using 4 KB pages
- Page Offset: bits [11:0]
- Page Number: bits [63:12]

### 64-bit system using 2 MB pages
- Page Offset: bits [20:0]
- Page Number: bits [63:21]

### Number of 2 MB hugepages in 2 GB

2 GB / 2 MB = 2048 MB / 2 MB = 1024

**Answer:** 1024 hugepages

## 2-1

**Given a victim address 0x752C3000, what is the value of its Row id? The value of its Column id?**

**For the same address, assume an arbitrary XOR function for computing the Bank id, list all possible attacker addresses whose Row id is one more than 0x752C3000's Row id and all the other ids match, including the Bank id and Column id. Hint: there should be 16 such addresses total.**

For victim address `0x752C3000`:

- **Row ID:** `0x3A96`
- **Column ID:** `0x1000`

The row-adjacent attacker addresses with matching DRAM structure are:

0x752E0000
0x752E1000
0x752E2000
0x752E3000
0x752E4000
0x752E5000
0x752E6000
0x752E7000
0x752E8000
0x752E9000
0x752EA000
0x752EB000
0x752EC000
0x752ED000
0x752EE000
0x752EF000

## 2-3

**Analyze the statistics produced by your code when running part2, and report a threshold to distinguish the bank conflict.**

Reasonable threshold is the midpoint:

(276 + 288) / 2 = 282

**Answer:** 282 cycles

### Write-up:

Based on the latency histogram from Part 2, accesses cluster around **272–276 cycles** for non-conflicting accesses and **288–292 cycles** for bank conflicts. A threshold of **282 cycles** effectively distinguishes bank conflicts from non-conflicts.

## 3-2

**Based on the XOR function you reverse-engineered, determine which of the 16 candidate addresses you derived in Discussion Question 2-1 maps to the same bank.**

Using the reverse-engineered bank mapping function from Part 3, we determined that **Function F0** is the correct XOR mapping.

To identify which of the 16 candidate attacker addresses maps to the same bank as the victim address `0x96ec3000`, we compute the bank ID using Function F0.

### Victim Bank ID

0x96ec3000 -> Bank ID = 6

### Candidate Evaluation

We compute the bank ID for each of the 16 candidate addresses derived in Part 2 using the same function.

Among these candidates, the address `0x96ee7000` maps to **Bank ID = 6**, which matches the victim’s bank ID.

Therefore, this address is in the **same bank** as the victim while being in the **adjacent row**, making it a valid attacker address for a double-sided Rowhammer attack. This confirms that Function F0 correctly models the DRAM bank mapping and can be used to identify attacker rows in the same bank as a victim row.

## 4-2

**Try different data pattern and include the bitflip observation statistics in the table below. Then answer the following questions:**

**Do your results match your expectations? What is the best pattern to trigger flips effectively?**

| Data Pattern (Victim/Aggressor) | Number of Flips (100 trials) |
| --- | --- |
| `0x00 / 0xff` | `10` |
| `0xff / 0x00` | `12` |
| `0x00 / 0x00` | `0` |
| `0xff / 0xff` | `0` |

The outcome conforms to the expectations. The complementary pattern between the aggressor and victim row resulted in more bit flips compared to when the same pattern is used in each row. For example, the pattern of 0xff/0x00 performed better than the rest with 12 flips in 100 trials. The next best is 0x00/0xff, having 10 flips, whereas the two patterns 0x00/0x00 and 0xff/0xff recorded zero flips.

## 5-1

**Given the ECC type descriptions listed above, fill in the following table (assuming a data length of 4). For correction/detection, only answer "Yes" if it can always correct/detect (and "No" if there is ever a case where the scheme can fail to correct/detect). We've filled in the first line for you.**

| Code | 1-Repetition (No ECC) | 2-Repetition | 3-Repetition | Single Parity Bit | Hamming(7,4) |
|---|---:|---:|---:|---:|---:|
| Code Rate (Data Bits / Total Bits) | 1.0 | 4/8 = 0.5 | 4/12 = 0.333 | 4/5 = 0.8 | 4/7 = 0.571 |
| Max Number of Errors Can Detect |  | 1 | 2 | 1 | 2 |
| Max Number of Errors Can Correct | 0 | 0 | 1 | 0 | 1 |

## 5-3

**When a single bit flip is detected, describe how Hamming(22,16) can correct this error.**

## 5-5

**Can the Hamming(22,16) code we implemented always protect us from rowhammer attacks? If not, describe how a clever attacker could work around this scheme.**

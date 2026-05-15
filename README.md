# sm-ext-transmitthrottle
**TransmitThrottle is a SourceMod extension designed to reduce the performance cost of the SDKHooks SetTransmit function in plugins.**



**It works by throttling SetTransmit calls on SDKHook through memory patching and injection, with a customizable update interval:**

- Instead of triggering SetTransmit on every entity for every plugin every tick, it executes the hook at a fixed interval (0.4 seconds by default). Between calls, it uses cached results so the behavior remains consistent while significantly reducing overhead.

- This eliminates excessive per tick calls caused by plugins using SetTransmit. No more up to 64 × 2048 calls per tick across every plugins. Instead, each entity is processed at a fixed interval, not per tick.

# Note:
- To change the interval (0.4 by default), set the convar value of "transmit_throttle_interval" to any desired value.
- Create an empty file named TransmitThrottle.autoload next to the extension file, otherwise SourceMod will not load it automatically.

# Maintaining on SourceMod updates:
- Currently, it is made for the latest stable version of SourceMod 1.12 build 7230.

- This extension is fragile because it relies on hardcoded addresses and offsets from the Source engine and SDKHooks. With every SourceMod update, there is a risk that these values will break.

- If something stops working after an update, check lines 228 and 328 where the hardcoded addresses are defined. The original ASM snippets are kept next to them for reference.

- To fix issues, update the offsets using a disassembler like IDA for the function SDKHooks::Hook_SetTransmit(CCheckTransmitInfo *, bool).

- Locate the function in the disassembly and take the first 16 bytes on Linux.

- Then find the "result" logic below the Call instruction, usually around a cmp eax, 3 check.

- From there, take 11 bytes for Linux 64-bit or 9 bytes for Linux 32-bit.  

  
**This is the price you pay for not changing the SDKHook source itself and compiling it properly, lazy ass mfs...**

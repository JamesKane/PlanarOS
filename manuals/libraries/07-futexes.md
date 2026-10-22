# 7. Exec: futexes

*Checked against V5.*

A signal wakes a task in its own process (chapter 2), and a message reaches a server (chapter 4). Neither fits a word that something else moves: a GPU writing a sync object, a display server counting vblanks, or a task in another process passing a sequence number. `WaitAddr` sleeps until such a word changes, and `WakeAddr` wakes whoever sleeps on it. Planar uses them for sequence numbers only.

## Concepts

- **The word** is 64 bits, 8-byte aligned, in memory the caller maps: its own, an Area shared with it, a connection's arena, or a page a server mapped to it read-only. Its value means whatever its writer says. Usually it's a count that only goes up.
- **Waits are keyed by physical address.** In the identity layout every process maps a word at the same address, so a waiter in one process and a waker in another meet without either naming the other. The kernel looks the word up in the caller's own domain.
- **A waiter says what it last saw.** `WaitAddr(address, value, deadline)` sleeps only while the word still holds `value`. If it already holds something else, the call returns at once. The kernel lists the waiter before it reads the word, so a wake that comes between the caller's look and its sleep isn't lost.
- **A waker changes the word, then wakes.** `WakeAddr(address, count)` wakes up to `count` waiters on that word, in any process, and returns how many it woke.
- **A deadline** is absolute, on `CurrentTime`'s clock, and 0 means none. `ERR_TIMEOUT` says it passed with the word unchanged.
- **A wait can end early.** It ends when any mapping of the word's Area goes, including at the death of a process that shared it. So when a process that writes the word dies, every wait on the word ends, though nothing woke it. A return of 0 says nothing about the word: look at it again.
- **The word is read through the caller's mapping**, never through the kernel's own, because it may be in an `AREAF_NC` Area that a device writes without the CPU's caches.

## Using it

### Waiting for a sequence number

From `conformance/futex.c`, which passes a sequence number between two processes in their connection's arena:
```c
/* Wait until '*seq' is 'want', sleeping while it holds anything else. */
static bool await(volatile uint64_t *seq, uint64_t want)
{
    uint64_t v;

    while ((v = __atomic_load_n(seq, __ATOMIC_ACQUIRE)) != want)
        if (WaitAddr(seq, v, CurrentTime() + 2000 * MS) == ERR_TIMEOUT)
            return false;
    return true;
}

static void move(volatile uint64_t *seq, uint64_t to)
{
    __atomic_store_n(seq, to, __ATOMIC_RELEASE);
    WakeAddr(seq, 1);
}
```
The loop is the whole pattern. Read the word; if it isn't there yet, sleep on what was read; read it again on every return.

### A word a device moves

A GPU's sync object is written by the GPU, and woken by its server when the firmware says it moved ([Devices ch. 9](../devices/09-gpu.md)). From `conformance/gputest.c`:
```c
submit();
while (sync->gs_Seqno != seqno && CurrentTime() < t0 + 1000 * MS)
    WaitAddr(&sync->gs_Seqno, seqno - 1, t0 + 1000 * MS);
```
The client could spin on the word instead and make no call at all. Sleeping costs the server's wake: a few microseconds on both boards (below).

### A word a server moves

`display` counts vblanks and latched commits in a page it maps to each client read-only ([Devices ch. 10](../devices/10-display.md)). From `conformance/displaytest.c`, waiting a quarter of a second at most for commit `seq` to be on the screen:
```c
uint64_t until = CurrentTime() + 250 * MS, v;

while ((v = sync_page->ds_Disp) < seq)
    if (WaitAddr(&sync_page->ds_Disp, v, until) == ERR_TIMEOUT)
        return sync_page->ds_Disp >= seq;
return true;
```

## Rules

- **One writer per word.** Futexes are for sequence numbers, never for locks between processes ([docs §12.5](../../docs/12-let-it-crash.md)). A lock holder that died would leave every other process waiting on it.
- **Change the word before you wake.** A waker that wakes first can be beaten by a waiter that reads the old value and sleeps on it again.
- **Always look again after a return.** A 0 doesn't mean the word moved.
- **Pass what you read, not what you want.** `WaitAddr(seq, v, …)` sleeps while the word is `v`. Passing the value you're waiting for sleeps until the word leaves it, which is the opposite.
- **A word in a device's memory** must be in an `AREAF_NC` Area, or the CPU may read a stale copy from its cache.
- **`WakeAddr` always traps.** A writer that moves a word often and is rarely waited on keeps a flag saying someone sleeps, and skips the call when nobody does. Connection rings do this with `cr_Waiting` ([docs §4.3](../../docs/04-ipc.md)).

## Costs

- **QEMU (TCG):** a sequence number passed back and forth between two processes takes 35–50 µs a round trip (`conformance/futex.c` logs it).
- **The CIX:** a GPU job waited for, woken by `gpu-panthor`, takes 22–23 µs against 11 µs spun on.
- **The RV2:** the same job, woken by `gpu-rogue`, takes 55–57 µs against 54 µs spun on. The server wakes the word within 2–3 µs of the firmware's update.

## Differences from Exec

- **Exec had nothing like it.** Every wait was `Wait()` on signal bits, and only a task's own process, or the kernel, could set them. In Planar, signals still reach only the caller's own process (chapter 2).
- **Unlike Linux's futex**, the word is 64 bits, the deadline is absolute, and there's no flag for a private or a shared futex: every word is keyed by its physical address.

## Functions used

[WaitAddr](../autodocs/exec.md#execwaitaddr), [WakeAddr](../autodocs/exec.md#execwakeaddr), [CurrentTime](../autodocs/exec.md#execcurrenttime)

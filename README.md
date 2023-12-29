# ocoomt
Over Commit and Out of Memory Testing

## The inspiration...

From time to time, I see people writing code that sticks malloc() into a wrapper that checks it for error. Usually the presumed error is that there was not enough memory to fulfill the requested allocation. However, because of the way most systems implement virtual memory and allow Over Commit, and with Linux in particular, most of these types of wrappers are meaningless because they will basically never report an out of memory situation except under various specific circumstances.

### A specific example...

The inspiration for this little 'study' (for lack of a better term) is a small chat utility that was written by antirez:
https://github.com/antirez/smallchat

```
void *chatMalloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        perror("Out of memory");
        exit(1);
    }
    return ptr;
}
```
The main question I had was at what point would malloc() actually return NULL as a result of an Out of Memory condition. According to the Linux manual pages, there is documentation which specifies what *should* happen.

*proc(5)* under the section labeled as and pertaining to '/proc/vm/sys/overcommit_memory' states:


>              This file contains the kernel virtual memory accounting mode.  Values are:
>
>                     0: heuristic overcommit (this is the default)
>                     1: always overcommit, never check
>                     2: always check, never overcommit
>
>              In mode 0, calls of mmap(2) with MAP_NORESERVE are not checked, and the default check is very weak, leading to the risk of getting a process "OOM-killed".
>
>              In mode 1, the kernel pretends there is always enough memory, until memory actually runs out.  One use case for this mode is scientific computing  >applications  that  employ
>              large sparse arrays.  In Linux kernel versions before 2.6.0, any nonzero value implies mode 1.
>
>              In mode 2 (available since Linux 2.6), the total virtual address space that can be allocated (CommitLimit in /proc/meminfo) is calculated as
>
>                  CommitLimit = (total_RAM - total_huge_TLB) *
>                                overcommit_ratio / 100 + total_swap
>
>              where:
>
>                   *  total_RAM is the total amount of RAM on the system;
>
>                   *  total_huge_TLB is the amount of memory set aside for huge pages;
>
>                   *  overcommit_ratio is the value in /proc/sys/vm/overcommit_ratio; and
>
>                   *  total_swap is the amount of swap space.
>
>              For  example, on a system with 16 GB of physical RAM, 16 GB of swap, no space dedicated to huge pages, and an overcommit_ratio of 50, this formula yields a eCommitLimit of 24
>            GB.
>
>              Since Linux 3.14, if the value in /proc/sys/vm/overcommit_kbytes is nonzero, then CommitLimit is instead calculated as:
>
>                  CommitLimit = overcommit_kbytes + total_swap
>
>              See also the description of /proc/sys/vm/admin_reserve_kbytes and /proc/sys/vm/user_reserve_kbytes.

However, this of course begs the question about how the 'heuristic' algorithm operates. There are a few different guides available from a Google search that suggest this mode refuses "unreasonable" memory allocation requests, but I was unable to find any that specifically detail what would constitute an "unreasonable" request.

According to https://www.baeldung.com/linux/overcommit-modes, using *stress-ng* on a machine to request various amounts of memory hogs resulted in the following:

>stress-ng -vm 1 --vm-bytes 32g -t 120
>stress-ng: debug: [5668] stress-ng 0.13.12
>stress-ng: debug: [5668] system: Linux ubuntu 5.15.0-52-generic 58-Ubuntu SMP Thu Oct 13 08:03:55 UTC 2022 x86_64
>stress-ng: debug: [5668] RAM total: 15.5G, RAM free: 9.6G, swap free: 14.9G
>
># ...
>
>stress-ng: error: [5670] stress-ng-vm: gave up trying to mmap, no available memory
>
># ...

Note the total amount of ram listed as 15.5 GB. They then modified the command...

>stress-ng -vm 1 --vm-bytes 16g -t 120

According to the website...

>Although the demand was accepted and everything looked good for a few moments, we noticed the sudden disappearance of windows in our GUI. It happened to the task’s >terminal or the web browser’s window as well. So, we experienced the OOM killer activity, triggered by the kernel facing the exhausting memory.

So in other words, the kernel accepted an Over Commitment of 512 MB to try to allocate 16 GB when the total amount of system memory was 15.5 GB. This is the 'heuristic' algorithm kicking in and determining that a request is 'unreasonable', but exactly how does it determine that this request is unreasonable?  Well, the answer is as simple as it is unclear:

This answer on Stack Overflow examines the specific piece of kernel code that constitutes the heuristic algorithm: https://stackoverflow.com/a/38688983

```
138 /*
139  * Check that a process has enough memory to allocate a new virtual
140  * mapping. 0 means there is enough memory for the allocation to
141  * succeed and -ENOMEM implies there is not.
142  *
143  * We currently support three overcommit policies, which are set via the
144  * vm.overcommit_memory sysctl.  See Documentation/vm/overcommit-accounting
145  *
146  * Strict overcommit modes added 2002 Feb 26 by Alan Cox.
147  * Additional code 2002 Jul 20 by Robert Love.
148  *
149  * cap_sys_admin is 1 if the process has admin privileges, 0 otherwise.
150  *
151  * Note this is a helper function intended to be used by LSMs which
152  * wish to use this logic.
153  */
154 int __vm_enough_memory(struct mm_struct *mm, long pages, int cap_sys_admin)
...
170         if (sysctl_overcommit_memory == OVERCOMMIT_GUESS) {
171                 free = global_page_state(NR_FREE_PAGES);
172                 free += global_page_state(NR_FILE_PAGES);
173 
174                 /*
175                  * shmem pages shouldn't be counted as free in this
176                  * case, they can't be purged, only swapped out, and
177                  * that won't affect the overall amount of available
178                  * memory in the system.
179                  */
180                 free -= global_page_state(NR_SHMEM);
181 
182                 free += get_nr_swap_pages();
183 
184                 /*
185                  * Any slabs which are created with the
186                  * SLAB_RECLAIM_ACCOUNT flag claim to have contents
187                  * which are reclaimable, under pressure.  The dentry
188                  * cache and most inode caches should fall into this
189                  */
190                 free += global_page_state(NR_SLAB_RECLAIMABLE);
191
192                 /*
193                  * Leave reserved pages. The pages are not for anonymous pages.
194                  */
195                 if (free <= totalreserve_pages)
196                         goto error;
197                 else
198                         free -= totalreserve_pages;
199 
200                 /*
201                  * Reserve some for root
202                  */
203                 if (!cap_sys_admin)
204                         free -= sysctl_admin_reserve_kbytes >> (PAGE_SHIFT - 10);
205 
206                 if (free > pages)
207                         return 0;
208 
209                 goto error;
210         }
```

Now, admittedly, I am not well versed in memory management. However, from what I can glean from this code, is that it is basically checking if the requested allocation in 'pages' is greater than the amount of 'free' pages. If it is not, it will return 0, or that it sucessfully served the requested allocation. Otherwise, it will return an error. The amount of 'pages' available to the system seems to be determined by the amount of physical memory available, and/or the amount of swap available if enabled. In other words, if you try to allocate more physical memory than the total amount the system has, and/or the total amount of swap space in addition to that, the 'heuristic' algorithm will result in a failed allocation. This is not exactly a sophisticated calculation that the 'heuristic' descriptor would imply.

On the other hand, this answer also details the code for the "always check, never overcommit" mode, referring to it as some "magic":

```
401 /*
402  * Committed memory limit enforced when OVERCOMMIT_NEVER policy is used
403  */
404 unsigned long vm_commit_limit(void)
405 {
406         unsigned long allowed;
407 
408         if (sysctl_overcommit_kbytes)
409                 allowed = sysctl_overcommit_kbytes >> (PAGE_SHIFT - 10);
410         else
411                 allowed = ((totalram_pages - hugetlb_total_pages())
412                            * sysctl_overcommit_ratio / 100);
413         allowed += total_swap_pages;
414 
415         return allowed;
416 }
417
```

Again, I am not exactly well versed in memory management principles. So instead of trying to glean what should happen from the kernel code posted, I decided to simply write my own program to test these various conditions. Not only did I hope to deduce what types of allocations the kernel would refuse, I also wanted to see what allocations would trigger the Out Of Memory killer to kill our process. This proved to be pretty illuminating.

## How much memory could a memory-hog hog if a memory-hog could hog memory...

Now, going back to the original inspiration for this, I wondered just how much "over commitment" a process could elicit from the kernel before finally being OOM-killed. In the original program in question, I never did test it to see what size allocations it would be making in practical use. Instead, I decided to write a program that would simply continually try to allocate incrementally larger pieces of memory until it was OOM-killed. To do this, I would first allocate all of the memory reported available by '/proc/meminfo', and then allocate a kilobyte at a time until the program was OOM-killed under Out of Memory conditions.

I first attepmted it when I was running a normal session on XFCE. Meaning that I had my IDE, web browser, etc. open. This is probably a more realistic scenario for most "memory pressure" conditions.

```Overcommit Mode: heuristic

MemTotal: 7820 mB
MemAvailable: 4860 mB
Buffers: 19 mB
Cached: 471 mB
SwapCached: 0 bytes
SwapTotal: 0 bytes
SwapFree: 0 bytes
CommitLimit: 3910 mB
Committed_AS: 11016 mB
Attepting to Allocate: 4860 mB
Successfully Allocated 4860 mB

Now filling array of buffers until Out Of Memory condition

MemAvailable: 4860 mB...
Buffers: 19 mB
Cached: 471 mB
CommitLimit: 3910 mB
CommittedAS: 15876 mB
Attempting to allocate ~4860 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 0 mB (4863 mB Virtual Mem)...
MemAvailable: 53 mB...
Buffers: 966656 bytes
Cached: 357 mB
CommitLimit: 3910 mB
CommittedAS: 20738 mB
Attempting to allocate ~53 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4861 mB (9724 mB Virtual Mem)...
MemAvailable: 24 mB...
Buffers: 557056 bytes
Cached: 308 mB
CommitLimit: 3910 mB
CommittedAS: 20792 mB
Attempting to allocate ~24 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4914 mB (9777 mB Virtual Mem)...
MemAvailable: 10 mB...
Buffers: 528384 bytes
Cached: 287 mB
CommitLimit: 3910 mB
CommittedAS: 20816 mB
Attempting to allocate ~10 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4938 mB (9801 mB Virtual Mem)...
MemAvailable: 4 mB...
Buffers: 516096 bytes
Cached: 279 mB
CommitLimit: 3910 mB
CommittedAS: 20827 mB
Attempting to allocate ~5 mB...


malloc() executed succeeded
Killed
```

As you can see, the program attepmted to allocate and use more and more memory until eventually it atempted to use more memory than was avalable and the Out of Memory manager killed it. However, do note that malloc() still never reported an error. I decided to also test this in just a terminal without any window manager running, and with minimal RAM used, and the results were even more interesting.

```
Overcommit Mode: heuristic

MemTotal: 7820 mB
MemAvailable: 6712 mB
Buffers: 4 mB
Cached: 94 mB
SwapCached: 0 bytes
SwapTotal: 0 bytes
SwapFree: 0 bytes
CommitLimit: 3910 mB
Committed_AS: 3574 mB
Attepting to Allocate: 6712 mB
Successfully Allocated 6712 mB

Now filling array of buffers until Out Of Memory condition

MemAvailable: 6712 mB...
Buffers: 4 mB
Cached: 94 mB
CommitLimit: 3910 mB
CommittedAS: 10286 mB
Attempting to allocate ~6712 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 0 mB (6715 mB Virtual Mem)...
MemAvailable: 22 mB...
Buffers: 3 mB
Cached: 72 mB
CommitLimit: 3910 mB
CommittedAS: 16998 mB
Attempting to allocate ~22 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6713 mB (13427 mB Virtual Mem)...
MemAvailable: 10 mB...
Buffers: 819200 bytes
Cached: 55 mB
CommitLimit: 3910 mB
CommittedAS: 17020 mB
Attempting to allocate ~10 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6736 mB (13450 mB Virtual Mem)...
MemAvailable: 4 mB...
Buffers: 552960 bytes
Cached: 44 mB
CommitLimit: 3910 mB
CommittedAS: 17031 mB
Attempting to allocate ~4 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6746 mB (13460 mB Virtual Mem)...
MemAvailable: 1 mB...
Buffers: 487424 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17035 mB
Attempting to allocate ~1 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6750 mB (13465 mB Virtual Mem)...
MemAvailable: 180224 bytes...
Buffers: 487424 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~180224 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6751 mB (13466 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 487424 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~1024 bytes...


malloc() executed succeeded
memset() executed succeeded

...

Memory Used By This Process: 6751 mB (13466 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 487424 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~1024 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6751 mB (13466 mB Virtual Mem)...
MemAvailable: 180224 bytes...
Buffers: 487424 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~2048 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6751 mB (13466 mB Virtual Mem)...
MemAvailable: 86016 bytes...
Buffers: 483328 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~86016 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6751 mB (13466 mB Virtual Mem)...
MemAvailable: 86016 bytes...
Buffers: 483328 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~86016 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6751 mB (13466 mB Virtual Mem)...
MemAvailable: 86016 bytes...
Buffers: 483328 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~86016 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6751 mB (13466 mB Virtual Mem)...
MemAvailable: 86016 bytes...
Buffers: 483328 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~86016 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6751 mB (13466 mB Virtual Mem)...
MemAvailable: 385024 bytes...
Buffers: 483328 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~86016 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6752 mB (13466 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 483328 bytes
Cached: 41 mB
CommitLimit: 3910 mB
CommittedAS: 17037 mB
Attempting to allocate ~3072 bytes...


malloc() executed succeeded
memset() executed succeeded

...

malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6758 mB (13473 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 507904 bytes
Cached: 39 mB
CommitLimit: 3910 mB
CommittedAS: 17043 mB
Attempting to allocate ~112640 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 6758 mB (13473 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 507904 bytes
Cached: 40 mB
CommitLimit: 3910 mB
CommittedAS: 17043 mB
Attempting to allocate ~113664 by
```

Notice how even when *MemAvailable* is zero, that allocations still succeed. The program starts allocating and filling a kilobyte at a time after this point, and it seems that it can get up to around 100 kB before it is actually OOM-killed, at least in the context of there not being a lot of memory-pressure present from other processes. The Out of Memory killer seems to act much more swiftly when there are other processes using a large amount of the memory in tandem with our testing process.

There's also a lot of points in the process where the *MemAvailable* changes from 0, and then goes back up, and I believe this is the system finding areas of chached memory to free in order to fulfill the allocation requests. Also notice again that the calls to malloc never fail, even when an allocation that results in the process being OOM-killed is made.

On the one hand, this suggests that for the most part applications which perform small (relative to about 100 kB or less) allocation requests can probably count on those allocations succeeding because the system does a good job finding a way free up memory for them. On the other hand, it might lead to people relying too much on the idea that malloc() will inform them of a lack of memory, and if they're making larger allocations, could find their process mysteriously being killed before any debug or error message could be triggered on the condition of malloc failing. I could imagine this causing headaches if/when memory leaks are encountered during a long-running process like a daemon, as it's unlikely that the allocation failure will show up in any logs. The programmer then may think, "Well it can't be that there isn't memory left, because malloc() never failed," and will end up having to go through a lot more trouble-shooting than necessary.

### How do I get malloc() to actually fail?

At this point it started to seem like "unreasonable" requests to malloc still wouldn't cause it to fail, and would just result in the process being OOM-killed, with varying degrees of successful allocation sizes being made before that occuring. For the purposes of the original program, it's hard to imagine that it would be making allocations that were larger than 100 kB, so it seems like it would be reasonable to expect the kernel to find memory to clear in order to successfully fulfill the allocation. However, if at some point it wasn't able to do that, it still doesn't seem like you could count on malloc() failing in order to allow the process to exit gracefully instead of being OOM-killed.

I started to wonder again how I could actually get malloc to fail under 'heuristic' and 'never' overcommit modes, and I found that the circumstances needed were a little different than what the manual pages described. Recalling the kernel code, I decided to see what would happen if I attempted to allocate the total amount of physical memory the system had...

```
Overcommit Mode: heuristic

MemTotal: 7820 mB
MemAvailable: 4582 mB
Buffers: 99 mB
Cached: 1255 mB
SwapCached: 0 bytes
SwapTotal: 0 bytes
SwapFree: 0 bytes
CommitLimit: 3910 mB
Committed_AS: 11360 mB
Attepting to Allocate: 7821 mB
./ocoomt.c:main:304: Cannot allocate memory
Attepting to Allocate: 7820 mB
Successfully Allocated 7820 mB

Now filling array of buffers until Out Of Memory condition

MemAvailable: 4582 mB...
Buffers: 99 mB
Cached: 1255 mB
CommitLimit: 3910 mB
CommittedAS: 19181 mB
Attempting to allocate ~4582 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 1 mB (7823 mB Virtual Mem)...
MemAvailable: 112 mB...
Buffers: 6 mB
Cached: 484 mB
CommitLimit: 3910 mB
CommittedAS: 23730 mB
Attempting to allocate ~111 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4583 mB (12405 mB Virtual Mem)...
MemAvailable: 40 mB...
Buffers: 1 mB
Cached: 408 mB
CommitLimit: 3910 mB
CommittedAS: 23842 mB
Attempting to allocate ~40 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4695 mB (12517 mB Virtual Mem)...
MemAvailable: 16 mB...
Buffers: 393216 bytes
Cached: 376 mB
CommitLimit: 3910 mB
CommittedAS: 23883 mB
Attempting to allocate ~15 mB...


Killed
```

As you can see, I wrote a loop into the program to continuously try allocations that were greater than or equal to *MemTotal* and *finally* got malloc() to fail when attempting to allocate the same amount as *MemTotal*. However, instead of then letting the program exit gracefully as I could have, I wanted to see what it would take for it to be OOM-killed beyond that point.

On the other hand, it should be noted at this point that I do not run with swap on my system, and that's not a common setup. So I decided to turn on some swap space and try it again...

```Overcommit Mode: heuristic

MemTotal: 7820 mB
MemAvailable: 4583 mB
Buffers: 37 mB
Cached: 881 mB
SwapCached: 0 bytes
SwapTotal: 1023 mB
SwapFree: 1023 mB
CommitLimit: 4934 mB
Committed_AS: 11586 mB
Attepting to Allocate: 7821 mB
Successfully Allocated 7821 mB

Now filling array of buffers until Out Of Memory condition

MemAvailable: 4583 mB...
Buffers: 37 mB
Cached: 881 mB
SwapCached: 0 bytes
SwapFree: 1023 mB...
CommitLimit: 4934 mB
CommittedAS: 19407 mB
Attempting to allocate ~4583 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 0 mB (7824 mB Virtual Mem)...
MemAvailable: 145 mB...
Buffers: 6 mB
Cached: 520 mB
SwapCached: 6 mB
SwapFree: 909 mB...
CommitLimit: 4934 mB
CommittedAS: 23962 mB
Attempting to allocate ~144 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4584 mB (12407 mB Virtual Mem)...
MemAvailable: 115 mB...
Buffers: 5 mB
Cached: 441 mB
SwapCached: 5 mB
SwapFree: 809 mB...
CommitLimit: 4934 mB
CommittedAS: 24107 mB
Attempting to allocate ~115 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4728 mB (12551 mB Virtual Mem)...
MemAvailable: 110 mB...
Buffers: 5 mB
Cached: 421 mB
SwapCached: 5 mB
SwapFree: 694 mB...
CommitLimit: 4934 mB
CommittedAS: 24222 mB
Attempting to allocate ~110 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4843 mB (12666 mB Virtual Mem)...
MemAvailable: 111 mB...
Buffers: 4 mB
Cached: 418 mB
SwapCached: 4 mB
SwapFree: 592 mB...
CommitLimit: 4934 mB
CommittedAS: 24328 mB
Attempting to allocate ~111 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4954 mB (12777 mB Virtual Mem)...
MemAvailable: 114 mB...
Buffers: 4 mB
Cached: 417 mB
SwapCached: 4 mB
SwapFree: 480 mB...
CommitLimit: 4934 mB
CommittedAS: 24441 mB
Attempting to allocate ~114 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5066 mB (12889 mB Virtual Mem)...
MemAvailable: 118 mB...
Buffers: 4 mB
Cached: 415 mB
SwapCached: 4 mB
SwapFree: 353 mB...
CommitLimit: 4934 mB
CommittedAS: 24555 mB
Attempting to allocate ~118 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5180 mB (13003 mB Virtual Mem)...
MemAvailable: 114 mB...
Buffers: 4 mB
Cached: 396 mB
SwapCached: 4 mB
SwapFree: 244 mB...
CommitLimit: 4934 mB
CommittedAS: 24673 mB
Attempting to allocate ~114 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5298 mB (13121 mB Virtual Mem)...
MemAvailable: 119 mB...
Buffers: 4 mB
Cached: 396 mB
SwapCached: 4 mB
SwapFree: 121 mB...
CommitLimit: 4934 mB
CommittedAS: 24786 mB
Attempting to allocate ~119 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5412 mB (13235 mB Virtual Mem)...
MemAvailable: 124 mB...
Buffers: 4 mB
Cached: 400 mB
CommitLimit: 4934 mB
CommittedAS: 24905 mB
Attempting to allocate ~124 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5531 mB (13354 mB Virtual Mem)...
MemAvailable: 55 mB...
Buffers: 1 mB
Cached: 317 mB
SwapCached: 1 mB
SwapFree: 16384 bytes...
CommitLimit: 4934 mB
CommittedAS: 25030 mB
Attempting to allocate ~55 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5655 mB (13479 mB Virtual Mem)...
MemAvailable: 25 mB...
Buffers: 1 mB
Cached: 271 mB
CommitLimit: 4934 mB
CommittedAS: 25085 mB
Attempting to allocate ~26 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5711 mB (13534 mB Virtual Mem)...
MemAvailable: 5 mB...
Buffers: 536576 bytes
Cached: 257 mB
CommitLimit: 4934 mB
CommittedAS: 25112 mB
Attempting to allocate ~6 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5737 mB (13561 mB Virtual Mem)...
MemAvailable: 1 mB...
Buffers: 520192 bytes
Cached: 255 mB
CommitLimit: 4934 mB
CommittedAS: 25118 mB
Attempting to allocate ~634880 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5743 mB (13567 mB Virtual Mem)...
MemAvailable: 1 mB...
Buffers: 499712 bytes
Cached: 254 mB
CommitLimit: 4934 mB
CommittedAS: 25119 mB
Attempting to allocate ~2 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5744 mB (13568 mB Virtual Mem)...
Killed
```
This time around, the malloc did not fail when requesting *MemTotal* because the swap file was present. You can see the amount of swap available diminishing as the program runs.  So then I decided to try to alloate *MemTotal* + *SwapTotal*...

```
Overcommit Mode: heuristic

MemTotal: 7820 mB
MemAvailable: 4580 mB
Buffers: 38 mB
Cached: 889 mB
SwapCached: 0 bytes
SwapTotal: 1023 mB
SwapFree: 1023 mB
CommitLimit: 4934 mB
Committed_AS: 11646 mB
Attepting to Allocate: 8845 mB
./ocoomt.c:main:304: Cannot allocate memory
Attepting to Allocate: 8844 mB
Successfully Allocated 8844 mB

Now filling array of buffers until Out Of Memory condition

MemAvailable: 4580 mB...
Buffers: 38 mB
Cached: 889 mB
SwapCached: 0 bytes
SwapFree: 1023 mB...
CommitLimit: 4934 mB
CommittedAS: 20490 mB
Attempting to allocate ~4580 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 0 mB (8847 mB Virtual Mem)...
MemAvailable: 360 mB...
Buffers: 34 mB
Cached: 646 mB
SwapCached: 34 mB
SwapFree: 692 mB...
CommitLimit: 4934 mB
CommittedAS: 25058 mB
Attempting to allocate ~360 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4581 mB (13427 mB Virtual Mem)...
MemAvailable: 275 mB...
Buffers: 18 mB
Cached: 578 mB
SwapCached: 18 mB
SwapFree: 398 mB...
CommitLimit: 4934 mB
CommittedAS: 25423 mB
Attempting to allocate ~274 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 4942 mB (13788 mB Virtual Mem)...
MemAvailable: 177 mB...
Buffers: 6 mB
Cached: 495 mB
SwapCached: 6 mB
SwapFree: 205 mB...
CommitLimit: 4934 mB
CommittedAS: 25698 mB
Attempting to allocate ~177 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5216 mB (14062 mB Virtual Mem)...
MemAvailable: 115 mB...
Buffers: 3 mB
Cached: 421 mB
SwapCached: 3 mB
SwapFree: 112 mB...
CommitLimit: 4934 mB
CommittedAS: 25876 mB
Attempting to allocate ~115 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5394 mB (14240 mB Virtual Mem)...
MemAvailable: 72 mB...
Buffers: 1 mB
Cached: 329 mB
SwapCached: 1 mB
SwapFree: 82 mB...
CommitLimit: 4934 mB
CommittedAS: 25991 mB
Attempting to allocate ~72 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5509 mB (14355 mB Virtual Mem)...
MemAvailable: 54 mB...
Buffers: 729088 bytes
Cached: 294 mB
SwapCached: 0 mB
SwapFree: 54 mB...
CommitLimit: 4934 mB
CommittedAS: 26064 mB
Attempting to allocate ~54 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5581 mB (14428 mB Virtual Mem)...
MemAvailable: 42 mB...
Buffers: 651264 bytes
Cached: 270 mB
SwapCached: 0 mB
SwapFree: 18 mB...
CommitLimit: 4934 mB
CommittedAS: 26118 mB
Attempting to allocate ~42 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5635 mB (14482 mB Virtual Mem)...
MemAvailable: 36 mB...
Buffers: 565248 bytes
Cached: 246 mB
SwapCached: 0 mB
SwapFree: 65536 bytes...
CommitLimit: 4934 mB
CommittedAS: 26161 mB
Attempting to allocate ~36 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5678 mB (14525 mB Virtual Mem)...
MemAvailable: 6 mB...
Buffers: 454656 bytes
Cached: 233 mB
CommitLimit: 4934 mB
CommittedAS: 26197 mB
Attempting to allocate ~5 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5714 mB (14561 mB Virtual Mem)...
MemAvailable: 1 mB...
Buffers: 434176 bytes
Cached: 233 mB
CommitLimit: 4934 mB
CommittedAS: 26203 mB
Attempting to allocate ~1 mB...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5720 mB (14567 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 421888 bytes
Cached: 233 mB
CommitLimit: 4934 mB
CommittedAS: 26204 mB
Attempting to allocate ~1024 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5721 mB (14568 mB Virtual Mem)...
MemAvailable: 225280 bytes...
Buffers: 446464 bytes
Cached: 233 mB
CommitLimit: 4934 mB
CommittedAS: 26204 mB
Attempting to allocate ~69632 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5721 mB (14568 mB Virtual Mem)...
MemAvailable: 225280 bytes...
Buffers: 446464 bytes
Cached: 233 mB
CommitLimit: 4934 mB
CommittedAS: 26204 mB
Attempting to allocate ~225280 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5721 mB (14568 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 446464 bytes
Cached: 233 mB
CommitLimit: 4934 mB
CommittedAS: 26204 mB
Attempting to allocate ~2048 bytes...

...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5722 mB (14568 mB Virtual Mem)...
MemAvailable: 45056 bytes...
Buffers: 446464 bytes
Cached: 233 mB
CommitLimit: 4934 mB
CommittedAS: 26204 mB
Attempting to allocate ~45056 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5722 mB (14568 mB Virtual Mem)...
MemAvailable: 45056 bytes...
Buffers: 446464 bytes
Cached: 233 mB
CommitLimit: 4934 mB
CommittedAS: 26204 mB
Attempting to allocate ~45056 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5722 mB (14568 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 446464 bytes
Cached: 233 mB
CommitLimit: 4934 mB
CommittedAS: 26204 mB
Attempting to allocate ~45056 bytes...


malloc() executed succeeded
memset() executed succeeded

...

Memory Used By This Process: 5724 mB (14570 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 446464 bytes
Cached: 232 mB
CommitLimit: 4934 mB
CommittedAS: 26206 mB
Attempting to allocate ~60416 bytes...


malloc() executed succeeded
memset() executed succeeded

Memory Used By This Process: 5724 mB (14570 mB Virtual Mem)...
MemAvailable: 0 bytes...
Buffers: 446464 bytes
Cached: 232 mB
CommitLimit: 4934 mB
CommittedAS: 26206 mB
Attempting to allocate ~61440 bytes...


malloc() executed succeeded
memset() executed succeeded
```
Again, malloc failed, but only when attempting to allocate both the total amount of physical memory, and the total amount of swap. Interestingly though, the system still did a pretty good job trying to fulfill allocation requests before finally OOM-killing the process. One thing to note is that after constructing the intial buffer of pointers, they're not actually using any physical ram, but instead are only using virtual memory. If the buffer of pointers is instead actually filled with memset(), it uses physical memory instead, and the program gets OOM-killed much faster...

```
Overcommit Mode: heuristic

MemTotal: 7820 mB
MemAvailable: 4595 mB
Buffers: 41 mB
Cached: 845 mB
SwapCached: 0 bytes
SwapTotal: 1023 mB
SwapFree: 1023 mB
CommitLimit: 4934 mB
Committed_AS: 11651 mB
Attepting to Allocate: 8845 mB
./ocoomt.c:main:304: Cannot allocate memory
Attepting to Allocate: 8844 mB
Successfully Allocated 8844 mB
Attempting to memset() allocated memory
Killed
```

The interesting thing is that there doesn't really seem to be anything 'heuristic' about how the kernel refuses an over-commitment, despite the descriptive title. Regardless of whether I ran this test in a simple terminal with hardly any memory used, or under memory pressure, the only circumstance where the kernel refuses to over-commit is when requesting an allocation that is greater than the total amount of physical memory and total amount of swap. The distinction of the "total" amounts of each should be noted, because even if a request is greater than the available physical memory and swap, the kernel will still allow that over-commitment, and then the program will simply be OOM-killed.

## Never say 'never'

Okay, so at this point, I started to think that the simple solution would be to simply use mode 2, to "never" over-commit. At least in that configuration, you can count on malloc() failing, and wouldn't be left with any mysterious OOM-killings to troubleshoot. However, since I already had the application written, I decided to see how it behaved under this mode. Again, recall that proc(5) states that the kernel will respect the *CommitLimit* and refuse allocations that are greater than that. Well, that's what I decided I wanted to test, and I got some interesting results yet again.

>              CommitLimit %lu (since Linux 2.6.10)
>                     This  is the total amount of memory currently available to be allocated on the system, expressed in kilobytes.  This limit is adhered to only if strict overcommit ac‐
>                     counting is enabled (mode 2 in /proc/sys/vm/overcommit_memory).  The limit is calculated according to the formula described under /proc/sys/vm/overcommit_memory.  For
>                     further details, see the kernel source file Documentation/vm/overcommit-accounting.rst.
>
>              Committed_AS %lu
>                     The  amount of memory presently allocated on the system.  The committed memory is a sum of all of the memory which has been allocated by processes, even if it has not
>                     been "used" by them as of yet.  A process which allocates 1 GB of memory (using malloc(3) or similar), but touches only 300 MB of that memory will show  up  as  using
>                     only 300 MB of memory even if it has the address space allocated for the entire 1 GB.
>
>                     This  1  GB is memory which has been "committed" to by the VM and can be used at any time by the allocating application.  With strict overcommit enabled on the system
>                     (mode 2 in /proc/sys/vm/overcommit_memory), allocations which would exceed the CommitLimit will not be permitted.  This is useful if one needs to guarantee that  pro‐
>                     cesses will not fail due to lack of memory once that memory has been successfully allocated.


One reading this might assume that they could simply write their malloc() wrapper to check the *CommitLimit*, but what I found was that allocation requests that were much lower than that were causing malloc() to fail. I created a loop that would continually try to allocate more and more until it found an amount that would succeed, and eventually I noticed that the only amounts which would succeed were basically the difference between the *CommitLimit* and the *Committed_AS* amount.

# A better solution

While there of course will probably never be a one-size-fits-all approach, I think that using /proc/meminfo is simple enough. A malloc() wrapper could simply check this and determine if an allocation request is likely to exhaust the available memory and trigger the process to be OOM-killed, and handle that appropriately. I think I would use something like this...

```
#include <errno.h>
...
void* no_oom_malloc(size_t size) {
    
    FILE *meminfo_file = fopen("/proc/meminfo", "r");
    if (meminfo_file == NULL) {
        perror("/proc/meminfo");
        return NULL;
    }

    char meminfo_line[256];
    size_t mem_available = -1;
    while (fgets(meminfo_line, sizeof(meminfo_line), meminfo_file)) {
        size_t attribute;
        if (sscanf(meminfo_line, "MemAvailable: %lu kB", &attribute) == 1) {
            mem_available = attribute * 1024;
        }
    }
    
    fclose(meminfo_file);
    
    enum overcommit_value {
        heuristic,
        always,
        never
    };
    
    FILE *overcommit_memory = fopen("/proc/sys/vm/overcommit_memory", "r");
    if (overcommit_memory == NULL) {
        return NULL;
    }

    char overcommit_string[3];
    if (fgets(overcommit_string, 2, overcommit_memory) == NULL) {
        fclose(overcommit_memory);
        return NULL;
    }
    
    fclose(overcommit_memory);
    
    int overcommit_mode;
    if(sscanf(overcommit_string,"%i",&overcommit_mode) != 1) {
        return NULL;
    }
        
    if (size > mem_available && overcommit_mode != always) {
        errno = ENOMEM;
        return NULL;
    } else {
        return malloc(size);
    }
}
```

This has the benefit of being a very simple drop-in replacement for malloc(), and just very simply checks if the allocation request would be greater than the available memory. If so, it sets errno to ENOMEM, and otherwise it simply calls malloc() and returns whatever it does. It also checks what the overcommit mode is set to, so that if it is 'always' it proceeds to attempt an allocation that is greater than the amount of memory available as one would want. One could also write a version that also checked if the overcommit mode was 'never' and check the *CommitLimit - Committed_AS* amount to the allocation request if there was some useful reason to do so. Otherwise, this seems to be the most useful, and universal approach.

Now, I know what you're thinking...

"What if I am using a lot of calls to malloc() to build a data structure like a linked-list? Shouldn't I use sysinfo() instead to reduce the overhead of calls in the wrapper function?"

While that would be ideal, the problem is that sysinfo() doesn't actually show the available memory.

```

               total        used        free      shared  buff/cache   available
Mem:            7820        2418        1063         251        4338        4853
Swap:              0           0           0
total ram: 7820
free ram: 1063
shared ram: 251
buffer ram: 440

```

If you look at the result of "free -m" versus listing the values of the sysinfo struct propagated by sysinfo(), you'll see that it is missing the cache information. I couldn't find any way that one could use that function to match the "memory available" reported by /proc/meminfo or 'free'. On the other hand, it could at least be modified so that the over commit mode isn't read from the file every time it's called to minimize overhead.

For a little example program...

```
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum overcommit_value {
    heuristic,
    always,
    never
};

void *no_oom_malloc(size_t size, int overcommit_mode)
{

    FILE *meminfo_file = fopen("/proc/meminfo", "r");
    if (meminfo_file == NULL) {
        perror("/proc/meminfo");
        return NULL;
    }

    char meminfo_line[256];
    size_t mem_available = -1;
    while (fgets(meminfo_line, sizeof(meminfo_line), meminfo_file)) {
        size_t attribute;
        if (sscanf(meminfo_line, "MemAvailable: %lu kB", &attribute) == 1) {
            mem_available = attribute * 1024;
        }
    }

    fclose(meminfo_file);

    if (size > mem_available && overcommit_mode != always) {
        errno = ENOMEM;
        return NULL;
    } else {
        return malloc(size);
    }
}

int main(int argc, char *argv[])
{

    FILE *meminfo_file = fopen("/proc/meminfo", "r");
    if (meminfo_file == NULL) {
        perror("/proc/meminf");
        exit(1);
    }

    char meminfo_line[256];
    size_t mem_total = -1;
    while (fgets(meminfo_line, sizeof(meminfo_line), meminfo_file)) {
        size_t attribute;
        if (sscanf(meminfo_line, "MemTotal: %lu kB", &attribute) == 1) {
            mem_total = attribute * 1024;
        }
    }

    fclose(meminfo_file);

    FILE *overcommit_memory = fopen("/proc/sys/vm/overcommit_memory", "r");
    if (overcommit_memory == NULL) {
        perror("overcommit_memory");
        exit(1);
    }

    char overcommit_string[3];
    if (fgets(overcommit_string, 2, overcommit_memory) == NULL) {
        fclose(overcommit_memory);
        exit(1);
    }

    fclose(overcommit_memory);

    int overcommit_mode;
    if (sscanf(overcommit_string, "%i", &overcommit_mode) != 1) {
        exit(1);
    }

    printf("Trying to allocate 1024 bytes...\n");
    char *buffer1 = no_oom_malloc(sizeof(char) * 1024, overcommit_mode);
    if (buffer1 == NULL) {
        perror("malloc");
    } else {
        printf("Successfully allocated a kilobyte\n");
        if (memset(buffer1, 0, sizeof(char))) {
            printf("Successfully executed memset() on the buffer\n");
        }
        free(buffer1);
    }

    size_t too_much_memory = -1;

    printf("\nTrying to allocate %zu mB...\n", too_much_memory/1024/1024);
    char *buffer2 = no_oom_malloc(sizeof(char) * too_much_memory, overcommit_mode);
    if (buffer2 == NULL) {
        perror("malloc");
    } else {
        printf("Successfully allocated %zu\n", too_much_memory/1024/1024);
        if (memset(buffer2, 0, sizeof(char))) {
            printf("Successfully executed memset() on the buffer\n");
        }
        free(buffer2);
    }

    printf("\nFinding the greatest amount we can allocate...\n");

    for (long long i = mem_total; i >= 1; i -= (1024*1024)) {
        printf("\nTrying to allocate %llu mB\n", i/1024/1024);
        buffer1 = no_oom_malloc(sizeof(char) * i, overcommit_mode);
        if (buffer1 == NULL) {
            perror("malloc");
        } else {
            printf("Successfully allocated %llu mB\n", i/1024/1024);
            if (memset(buffer1, 0, sizeof(char))) {
                printf("Successfully executed memset() on the buffer\n");
            }
            free(buffer1);
            break;
        }
    }

    return 0;
}
```

Resulting in...

```
               total        used        free      shared  buff/cache   available
Mem:            7820        2182        5033         203         604        5187
Swap:              0           0           0
Trying to allocate 1024 bytes...
Successfully allocated a kilobyte
Successfully executed memset() on the buffer

Trying to allocate 17592186044415 megabytes...
malloc: Cannot allocate memory

Finding the greatest amount we can allocate...

Trying to allocate 7820
malloc: Cannot allocate memory

Trying to allocate 7819
malloc: Cannot allocate memory

...

Trying to allocate 5188
malloc: Cannot allocate memory

Trying to allocate 5187
Successfully allocated 5187
```

# Conclusion

In the end, I think that this shows simply checking malloc() for error to avoid out-of-memory situations is a naive approach, but one that programmers aren't often bit by because the system does such a good job attempting to fulfill allocation requests in out-of-memory situations. However, as stated, if/when the system does eventually fail to do so, this situation could really lead to some head-scratchers, because if the process isn't actively monitoring the memory situation, but instead only relies on testing the success of malloc(), it will be OOM-killed before it can even make a record of it being killed.

In addition to that, I think the documentation surrounding this issue needs to be updated for better clarity. Not only is the descriptive title of 'heuristic' somewhat misleading, as evidenced by the erroneous takes on various web articles about how it is supposed to behave, but the way the 'proc' manual describes *CommitLimit* is easily misunderstood--if not downright inaccurate. While it would at least allow the programmer to rely on testing malloc() for error to avoid memory exhaustion situations by knowing they could select mode 2, it might also leave them scratching their head if allocations that are less than the *CommitLimit* are refused, because the true limit is actually *CommitLimit - Committed_AS*

Finally, while it is of course still a good idea to always check the success of malloc(), counting on it being platform-agnostic in terms of reporting when memory has ran out is obviously fraught with error. With how Linux handles this, I am unaware of what Windows, or Mac, or any other kernel does, and so counting on standard C library functions to check for and avoid memory exhaustion is simply not a good idea. On the other hand, knowing all of this now, it is at least relatively easy to devise a malloc() wrapper that actually *does* allow a program to fail gracefully under out-of-memory conditions.



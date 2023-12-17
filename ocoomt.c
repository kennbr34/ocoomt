/* This program tests over-commit behavior and Out of Memory management on Linux distributions.
 * malloc() will not report that it failed to allocate memory except under very specific conditions
 * because of the various Over Commit modes that the Linux kernel uses. This is described in the
 * '/proc/sys/vm/overcommit_memory' section of manual page proc(5).
 * 
 * However, I found that the conditions in which the kernel will not Over Commit are not as described
 * in the manual page, but are instead as follows:
 * 
 * heuristic overcommit: The kernel is supposed to use a 'heuristic' algorithm to refuse an allocation
 * that is too large. In my testing, this only occurs when the allocation amount requested is larger
 * than or equal to the total amount of physical memory, and total amount of swap if enabled.
 * 
 * always overcommit, never check: Self-explanatory. Kernel will fulfill any allocation request even
 * if it is too large.
 * 
 * never overcommit, always check: proc(5) describes the kernel limiting allocation requests to the
 * amount defined by 'CommitLimit' and how it is calculated. However, in my testing, the kernel
 * instead refuses requests that are approximately 'CommitLimit - CommittedAS' in size. CommitLimit
 * and CommittedAS are both further defined in proc(5).
 * 
 * The program firsts allocates a buffer of pointers, the maximum size the system will allow by using
 * a loop to attempt allocations until malloc() succeeds, which will depend on the Over Commit mode as
 * described above. It will then go into a loop that allocates memory to each pointer in that buffer
 * incrementally until an Out of Memory condition is created, and the kernel kills it in 'heuristic' 
 * or 'always' mode, or until malloc() finally fails in 'never' mode. It will display the amount of
 * physical and virtual memory the process is using, as well as the available memory, buffers, cached,
 * swap, and caches in swap as it does so, revealing what memory conditions are created under memory
 * pressure, and what levels of Over Commit will be tolerated before the program is OOM-killed.
 * 
 * To ensure that this process is killed by the OOM manager, and not other processes, it can be started
 * with the 'choom' utility to adjust its oom score.*/

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRINT_SYS_ERROR(err_code) \
    { \
        fprintf(stderr, "%s:%s:%d: %s\n", __FILE__, __func__, __LINE__, strerror(err_code)); \
    }

#define BYTE 1
#define KILOBYTE 1024
#define MEGABYTE 1048576

enum overcommit_value {
    heuristic,
    always,
    never
};

enum format_value {
    byte,
    kilobyte,
    megabyte
};

struct opts {
    bool memset_pointers;
    bool overcommit_heuristic;
    bool total_is_physical;
    bool total_is_swap_and_physical;
    bool use_free_swap;
    bool use_total_swap;
};

int print_help(char *arg)
{
    fprintf(stderr, "\
\nUsage: \
\n\n%s [-h] [-o] [-t physical | swap_and_physical] [-s total | free]\
\n\nOptions: \
\n-h,--help - Print this help page\
\n-m,--memset-pointers - memset() the buffer of pointers so that Out of Memory condition is met faster\
\n-o,--overcommit_heuristic - Allocate enough to make malloc() fail in heuristic mode\
\n-t,--total-type - Set whether to consider 'total' memory as physical memory or swap and physical memory \
\n\tphysical - Consider only physical memory as total\
\n\tswap_and_physical - Consider only swap and physical memory as total\
\n-s,--swap-type - Set whether to add swap as the free amount of swap or total amount of swap \
\n\ttotal - Use total swap\
\n\tfree - Use free swap\
\n\
",
            arg);
    return 1;
}

int parse_opts(int argc, char *argv[], struct opts *opts_st)
{
    int opt = 0;
    int errflg = 0;

    while (1) {

        int option_index = 0;
        static struct option long_options[] = {
            {"help", no_argument, 0, 'h'},
            {"memset-pointers", no_argument, 0, 'm'},
            {"overcommit-heuristic", no_argument, 0, 'v'},
            {"total-type", required_argument, 0, 't'},
            {"swap-type", required_argument, 0, 't'},
            {0, 0, 0, 0}};

        opt = getopt_long(argc, argv, "hmot:s:",
                          long_options, &option_index);
        if (opt == -1)
            break;

        switch (opt) {
        case 'h':
            print_help("ocoomt");
            exit(EXIT_SUCCESS);
            break;
        case 'm':
            opts_st->memset_pointers = true;
            break;
        case 'o':
            opts_st->overcommit_heuristic = true;
            break;
        case 't':
            if (optarg[0] == '-') {
                fprintf(stderr, "Option -%c requires an argument\n", opt);
                errflg++;
                break;
            }
            
            if(strcmp(optarg,"physical") == 0) {
                opts_st->total_is_physical = true;
            } else if(strcmp(optarg,"swap_and_physical") == 0) {
                opts_st->total_is_swap_and_physical = true;
            } else {
                fprintf(stderr, "Invalid option: %s\n", optarg);
                errflg = 1;
            }
            
            break;
        case 's':
            if (optarg[0] == '-') {
                fprintf(stderr, "Option -%c requires an argument\n", opt);
                errflg++;
                break;
            }
            
            if(strcmp(optarg,"total") == 0) {
                opts_st->use_total_swap = true;
            } else if(strcmp(optarg,"free") == 0) {
                opts_st->use_free_swap = true;
            } else {
                fprintf(stderr, "Invalid option: %s\n", optarg);
                errflg = 1;
            }
            
            break;
        case ':':
            fprintf(stderr, "Option -%c requires an argument\n", opt);
            errflg++;
            break;
        case '?':
            errflg++;
            break;
        }
    }

    if (errflg) {
        fprintf(stderr, "Use -h to print help\n");
        return 1;
    }

    return 0;
}

size_t b_to_mb(size_t bytes)
{
    return bytes / MEGABYTE;
}

size_t get_meminfo_attribute(const char *file, const char *attribute_string, int format_value)
{
    FILE *attribute_file = fopen(file, "r");
    if (attribute_file == NULL) {
        perror(file);
        fclose(attribute_file);
        return 0;
    }

    char attribute_line[256];
    while (fgets(attribute_line, sizeof(attribute_line), attribute_file)) {
        size_t attribute;
        if (sscanf(attribute_line, attribute_string, &attribute) == 1) {
            fclose(attribute_file);
            if (format_value == byte) {
                return attribute * 1024;
            } else if (format_value == kilobyte) {
                return attribute;
            } else if (format_value == megabyte) {
                return attribute / 1024;
            } else {
                return attribute;
            }
        }
    }

    fclose(attribute_file);

    return 0;
}

int main(int argc, char *argv[])
{
    struct opts opts_st = {0};

    if (parse_opts(argc, argv, &opts_st) != 0) {
        print_help("ocoomt");
        exit(EXIT_FAILURE);
    }

    FILE *overcommit_memory = fopen("/proc/sys/vm/overcommit_memory", "r");
    if (overcommit_memory == NULL) {
        PRINT_SYS_ERROR(errno);
        exit(EXIT_FAILURE);
    }

    char overcommit_string[3];
    if (fgets(overcommit_string, 2, overcommit_memory) == NULL) {
        PRINT_SYS_ERROR(errno);
    }
    
    int overcommit_mode;
    if(sscanf(overcommit_string,"%i",&overcommit_mode) != 1) {
        PRINT_SYS_ERROR(errno);
        exit(EXIT_FAILURE);
    }

    size_t mem_total = get_meminfo_attribute("/proc/meminfo", "MemTotal: %lu kB", byte);
    size_t mem_available = get_meminfo_attribute("/proc/meminfo", "MemAvailable: %lu kB", byte);
    size_t buffers = get_meminfo_attribute("/proc/meminfo", "Buffers: %lu kB", byte);
    size_t cached = get_meminfo_attribute("/proc/meminfo", "Cached: %lu kB", byte);
    size_t swap_cached = get_meminfo_attribute("/proc/meminfo", "SwapCached: %lu kB", byte);
    size_t swap_free = get_meminfo_attribute("/proc/meminfo", "SwapFree: %lu kB", byte);
    size_t swap_total = get_meminfo_attribute("/proc/meminfo", "SwapTotal: %lu kB", byte);
    size_t commit_limit = get_meminfo_attribute("/proc/meminfo", "CommitLimit: %lu kB", byte);
    size_t committed_as = get_meminfo_attribute("/proc/meminfo", "Committed_AS: %lu Kb", byte);
    
    size_t swap_type;
    if(opts_st.use_free_swap) {
        swap_type = swap_free;
    } else if(opts_st.use_total_swap) {
        swap_type = swap_total;
    }
    
    size_t allocation_amount = 0;

    if (overcommit_mode == heuristic || overcommit_mode == always) {
        if (overcommit_mode == heuristic) {
            if (opts_st.overcommit_heuristic) {
                if(opts_st.total_is_physical) {
                    allocation_amount = mem_total;
                } else if (opts_st.total_is_swap_and_physical) {
                    allocation_amount = mem_total + swap_type;
                } else {
                    allocation_amount = mem_total + swap_type;
                }
                allocation_amount += MEGABYTE;
            } else {
                allocation_amount = mem_available + swap_type;
            }
        } else if (overcommit_mode == always) {
            if(opts_st.total_is_physical) {
                allocation_amount = mem_available;
            } else if (opts_st.total_is_swap_and_physical) {
                allocation_amount = mem_available + swap_type;
            } else {
                allocation_amount = mem_available + swap_type;
            }
        }

        printf(overcommit_mode == heuristic ? "Overcommit Mode: heuristic\n" : "Overcommit Mode: always\n");
    } else if (overcommit_mode == never) {
        allocation_amount = commit_limit - committed_as;

        printf("Overcommit Mode: never\n");
    }

    size_t **buffer_array;

    printf(mem_total > MEGABYTE ? "\nMemTotal: %zu mB\n" : "\nMemTotal: %zu bytes\n", mem_total > MEGABYTE ? b_to_mb(mem_total) : mem_total);
    printf(mem_available > MEGABYTE ? "MemAvailable: %zu mB\n" : "MemAvailable: %zu mB\n", mem_available > MEGABYTE ? b_to_mb(mem_available) : mem_available);
    printf(buffers > MEGABYTE ? "Buffers: %zu mB\n" : "Buffers: %zu bytes\n", buffers > MEGABYTE ? b_to_mb(buffers) : buffers);
    printf(cached > MEGABYTE ? "Cached: %zu mB\n" : "Cached: %zu bytes\n", cached > MEGABYTE ? b_to_mb(cached) : cached);
    printf(swap_cached > MEGABYTE ? "SwapCached: %zu mB\n" : "SwapCached: %zu bytes\n", swap_cached > MEGABYTE ? b_to_mb(swap_cached) : swap_cached);
    printf(swap_total > MEGABYTE ? "SwapTotal: %zu mB\n" : "SwapTotal: %zu bytes\n", swap_total > MEGABYTE ? b_to_mb(swap_total): swap_total);
    printf(swap_free > MEGABYTE ? "SwapFree: %zu mB\n" : "SwapFree: %zu bytes\n", swap_free > MEGABYTE ? b_to_mb(swap_free) : swap_free);
    printf(commit_limit > MEGABYTE ? "CommitLimit: %zu mB\n" : "CommitLimmit: %zu bytes\n", commit_limit > MEGABYTE ? b_to_mb(commit_limit) : commit_limit);
    printf(committed_as > MEGABYTE ? "Committed_AS: %zu mB\n" : "Committed_AS: %zu bytes\n", committed_as > MEGABYTE ? b_to_mb(committed_as) : committed_as);
    
    for (size_t i = allocation_amount;; i -= MEGABYTE) {
        printf("Attepting to Allocate: %zu mB\n", b_to_mb(i));
        buffer_array = malloc(sizeof(uint8_t) * i);
        if (buffer_array == NULL) {
            PRINT_SYS_ERROR(errno);
            if (i <= MEGABYTE) {
                exit(EXIT_FAILURE);
            }
            continue;
        } else {
            printf("Successfully Allocated %zu mB\n", b_to_mb(i));
            if(opts_st.memset_pointers) {
                printf("Attempting to memset() allocated memory\n");
                if (memset(buffer_array, 0, sizeof(uint8_t) * i) != NULL) {
                    printf("\nmemset() executed succeeded\n\n");
                }
            }
            break;
        }
    }

    printf("\nNow filling array of buffers until Out Of Memory condition\n\n");

    for (size_t i = 1;;) {

        if (overcommit_mode == never) {
            mem_available = get_meminfo_attribute("/proc/meminfo", "CommitLimit: %lu kB", byte) - get_meminfo_attribute("/proc/meminfo", "Committed_AS: %lu kB", byte);
        } else {
            mem_available = get_meminfo_attribute("/proc/meminfo", "MemAvailable: %lu kB", byte);
            swap_free = get_meminfo_attribute("/proc/meminfo", "SwapFree: %lu kB", byte);
            commit_limit = get_meminfo_attribute("/proc/meminfo", "CommitLimit: %lu kB", byte);
        }

        if ((mem_available || swap_free) && overcommit_mode != never) {
            allocation_amount = mem_available ? mem_available : mem_available + swap_free;
        } else if (!mem_available || overcommit_mode == never) {
            allocation_amount = i * 1024;
            i++;
        }

        mem_available = get_meminfo_attribute("/proc/meminfo", "MemAvailable: %lu kB", byte);
        buffers = get_meminfo_attribute("/proc/meminfo", "Buffers: %lu kB", byte);
        cached = get_meminfo_attribute("/proc/meminfo", "Cached: %lu kB", byte);
        swap_cached = get_meminfo_attribute("/proc/meminfo", "SwapCached: %lu kB", byte);
        swap_total = get_meminfo_attribute("/proc/meminfo", "SwapTotal: %lu kB", byte);
        swap_free = get_meminfo_attribute("/proc/meminfo", "SwapFree: %lu kB", byte);
        commit_limit = get_meminfo_attribute("/proc/meminfo", "CommitLimit: %lu kB", byte);
        committed_as = get_meminfo_attribute("/proc/meminfo", "Committed_AS: %lu Kb", byte);
        size_t proc_mem_used = get_meminfo_attribute("/proc/self/status", "VmRSS: %lu kB", byte);
        size_t proc_virt_mem_used = get_meminfo_attribute("/proc/self/status", "VmSize: %lu kB", byte);

        printf(mem_available > MEGABYTE ? "MemAvailable: %zu mB...\n" : "MemAvailable: %zu bytes...\n", mem_available > MEGABYTE ? b_to_mb(mem_available) : mem_available);
        printf(buffers > MEGABYTE ? "Buffers: %zu mB\n": "Buffers: %zu bytes\n", buffers > MEGABYTE ? b_to_mb(buffers) : buffers);
        printf(cached > MEGABYTE ? "Cached: %zu mB\n" : "Cached: %zu bytes\n", cached > MEGABYTE ? b_to_mb(cached) : cached);
        if (swap_free) {
            printf(swap_cached > MEGABYTE ? "SwapCached: %zu mB\n" : "SwapCached: %zu bytes\n", swap_cached > MEGABYTE ?  b_to_mb(buffers) : swap_cached);
            printf(swap_free > MEGABYTE ? "SwapFree: %zu mB...\n" : "SwapFree: %zu bytes...\n", swap_free > MEGABYTE ? b_to_mb(swap_free) : swap_free);
        }
        printf(commit_limit > MEGABYTE ? "CommitLimit: %zu mB\n" : "CommitLimit: %zu bytes\n", commit_limit > MEGABYTE ? b_to_mb(commit_limit) : commit_limit);
        printf(committed_as > MEGABYTE ? "CommittedAS: %zu mB\n" : "CommittedAS: %zu bytes\n", committed_as > MEGABYTE ? b_to_mb(committed_as) : committed_as);
        printf(allocation_amount > MEGABYTE ? "Attempting to allocate ~%zu mB...\n" : "Attempting to allocate ~%zu bytes...\n", allocation_amount > MEGABYTE ? b_to_mb(allocation_amount) : allocation_amount);

        *buffer_array = malloc(sizeof(uint8_t) * allocation_amount);
        if (*buffer_array == NULL) {
            PRINT_SYS_ERROR(errno);
            exit(EXIT_FAILURE);
        } else {
            printf("\n\nmalloc() executed succeeded");
        }

        if (memset(*buffer_array, 0, sizeof(uint8_t) * allocation_amount) != NULL) {
            printf("\nmemset() executed succeeded");
        }

        printf("\n\nMemory Used By This Process: %zu mB (%zu mB Virtual Mem)...\n", b_to_mb(proc_mem_used), b_to_mb(proc_virt_mem_used));

        *(buffer_array++);
    }

    return EXIT_SUCCESS;
}

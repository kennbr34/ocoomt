#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define MEGABYTE 1048576

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
        return NULL;
    }
    
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

int main (int argc, char *argv[]) {
        
    FILE *meminfo_file = fopen("/proc/meminfo", "r");
    if (meminfo_file == NULL) {
        perror("/proc/meminf");
        exit(1);
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
    
    printf("Trying to allocate 1024 bytes...\n");
    char *buffer1 = no_oom_malloc(sizeof(char) * 1024);
    if(buffer1 == NULL ) {
        perror("malloc");
    } else {
        printf("Successfully allocated a kilobyte\n");
        if(memset(buffer1,0,sizeof(char))) {
            printf("Successfully executed memset() on the buffer\n");
        }
        free(buffer1);
    }
    
    size_t too_much_memory = -1;
    
    printf("\nTrying to allocate %zu...\n", too_much_memory);
    char *buffer2 = no_oom_malloc(sizeof(char) * too_much_memory);
    if(buffer2 == NULL) {
        perror("malloc");
    } else {
        printf("Successfully allocated %zu\n", too_much_memory);
        if(memset(buffer2,0,sizeof(char))) {
            printf("Successfully executed memset() on the buffer\n");
        }
        free(buffer2);
    }
    
    printf("\nFinding the greatest amount we can allocate...\n");
        
    for( size_t i = mem_available * 10; ; i -= MEGABYTE ) {
        printf("\nTrying to allocate %zu...\n", i);
        buffer1 = no_oom_malloc(sizeof(char) * i);
        if(buffer1 == NULL) {
            perror("malloc");
        } else {
            printf("Successfully allocated %zu\n", i);
            if(memset(buffer1,0,sizeof(char))) {
                printf("Successfully executed memset() on the buffer\n");
            }
            free(buffer1);
            break;
        }
    }
    
    return 0;
}
    
    

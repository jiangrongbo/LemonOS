#ifndef STRING_H
#define STRING_H

#include <stddef.h>

#ifdef __cplusplus
#define NULL 0L
#else
#define NULL ((void*)0)
#endif

#ifdef __cplusplus
extern "C"{
#endif

void* memset(void* src, int c, size_t count);
void *memcpy(void* dest, void* src, size_t count);

int strlen(char* str);
char* strcat(char* dest, const char* src);
size_t strspn(const char *s1, const char *s2);
size_t strcspn(const char *s1, const char *s2);
char *strtok(char * str, const char * delim);
void strcpy(char* dest, const char* src);

#ifdef __cplusplus
}
#endif

#endif
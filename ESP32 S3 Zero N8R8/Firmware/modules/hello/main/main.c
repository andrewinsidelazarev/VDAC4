#include <stdio.h>

// Пробный модуль VDAC4: печатает и возвращает число аргументов + 40.
int main(int argc, char *argv[])
{
    printf("hello from ELF, argc=%d\n", argc);
    return argc + 40;
}

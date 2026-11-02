#include "zifi/elf_image.hpp"
#include <stdio.h>
#include <vector>
int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        FILE *file = fopen(argv[i], "rb");
        if (!file || fseek(file, 0, SEEK_END))
            return 2;
        long size = ftell(file);
        if (size < 0 || fseek(file, 0, SEEK_SET))
            return 2;
        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (fread(data.data(), 1, data.size(), file) != data.size())
            return 2;
        fclose(file);
        const char *problem = zifi::elfImageProblem(data.data(), data.size());
        if (problem) {
            fprintf(stderr, "%s: %s\n", argv[i], problem);
            return 1;
        }
        printf("PASS elfImageProblem: %s\n", argv[i]);
    }
    return 0;
}

#include <stdio.h>

int main(void)
{
    fprintf(stderr, "intentional CGI failure\n");
    return 1;
}

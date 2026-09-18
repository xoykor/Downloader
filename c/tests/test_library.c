#include "downloader/library.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char dir[] = "/tmp/downloader-library-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char media[512]; snprintf(media, sizeof(media), "%s/video.mp4", dir);
    FILE *f = fopen(media, "wb"); assert(f != NULL); fputs("x", f); fclose(f);
    DldAppError error; dld_app_error_init(&error);
    assert(dld_library_record(dir, "abc", "mp4", media, &error));
    char *found = NULL;
    assert(dld_library_find(dir, "abc", "mp4", &found, &error));
    assert(strcmp(found, media) == 0);
    free(found);
    char index[512]; snprintf(index, sizeof(index), "%s/.downloader-library.json", dir);
    unlink(media); unlink(index); rmdir(dir); dld_app_error_clear(&error);
    puts("test_library: ok");
    return 0;
}

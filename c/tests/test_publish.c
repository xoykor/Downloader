#include "downloader/publish.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb"); assert(f != NULL);
    assert(fwrite(text, 1U, strlen(text), f) == strlen(text));
    fclose(f);
}

int main(void)
{
    char dir[] = "/tmp/downloader-publish-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char tmp[512], dst[512];
    snprintf(tmp, sizeof(tmp), "%s/temp", dir);
    snprintf(dst, sizeof(dst), "%s/video.mp4", dir);
    write_file(tmp, "new"); write_file(dst, "old");
    DldPublishedOutput out; dld_published_output_init(&out);
    DldAppError error; dld_app_error_init(&error);
    assert(dld_publish_output(tmp, dst, DLD_COLLISION_RENAME, &out, &error));
    assert(strcmp(out.path, dst) != 0);
    assert(access(dst, F_OK) == 0 && access(out.path, F_OK) == 0);
    unlink(dst); unlink(out.path); rmdir(dir);
    dld_published_output_clear(&out); dld_app_error_clear(&error);
    puts("test_publish: ok");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>

static void each_property(const prop_info *pi, void *cookie) {
    char name[PROP_NAME_MAX] = {0};
    char value[PROP_VALUE_MAX] = {0};
    __system_property_read(pi, name, value);
    FILE *out = (FILE*)cookie;
    fputs(name, out);
    fputc(0, out);
    fputs(value, out);
    fputc(0, out);
}

int main(int argc, char **argv) {
    const char *out_path = (argc > 1) ? argv[1] : "/data/local/tmp/hollowroot/property-seed.bin";
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror("fopen output");
        return 1;
    }
    int rc = __system_property_foreach(each_property, out);
    fclose(out);
    printf("[+] Dumped host properties to %s (result=%d)\n", out_path, rc);
    return rc;
}

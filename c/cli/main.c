#include "exports.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
    (void)fprintf(stream, "Usage: sdaf-c decode <input.sdaf> [--format json|cbor|csv] [--output <path>|-]\n");
}

int main(int argc, char **argv)
{
    int index = 1, result = 0; const char *input, *format = "json", *output_path = NULL; FILE *output = stdout; sdaf_document document; sdaf_status status;
    if (argc <= 1) { usage(stderr); return 2; }
    if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) { usage(stdout); return 0; }
    if (strcmp(argv[index], "decode") == 0) ++index;
    if (index >= argc || argv[index][0] == '-') { usage(stderr); return 2; }
    input = argv[index++];
    while (index < argc) {
        if ((strcmp(argv[index], "--format") == 0 || strcmp(argv[index], "-f") == 0) && index + 1 < argc) { format = argv[index + 1]; index += 2; }
        else if ((strcmp(argv[index], "--output") == 0 || strcmp(argv[index], "-o") == 0) && index + 1 < argc) { output_path = argv[index + 1]; index += 2; }
        else { (void)fprintf(stderr, "sdaf-c: invalid option '%s'\n", argv[index]); return 2; }
    }
    if (strcmp(format, "json") != 0 && strcmp(format, "cbor") != 0 && strcmp(format, "csv") != 0) { (void)fprintf(stderr, "sdaf-c: format must be json, cbor, or csv\n"); return 2; }
    status = sdaf_decode_file(input, NULL, &document);
    if (status != SDAF_OK) { (void)fprintf(stderr, "sdaf-c: %s at byte %llu: %s\n", sdaf_status_string(status), (unsigned long long)document.error_offset, document.error[0] != '\0' ? document.error : sdaf_status_string(status)); sdaf_document_free(&document); return 1; }
    if (output_path != NULL && strcmp(output_path, "-") != 0) { output = fopen(output_path, strcmp(format, "cbor") == 0 ? "wb" : "w"); if (output == NULL) { perror("sdaf-c"); sdaf_document_free(&document); return 1; } }
    if (strcmp(format, "json") == 0) result = sdaf_cli_write_json(output, &document);
    else if (strcmp(format, "cbor") == 0) result = sdaf_cli_write_cbor(output, &document);
    else result = sdaf_cli_write_csv(output, &document);
    if (output != stdout && fclose(output) != 0) result = -1;
    sdaf_document_free(&document);
    if (result != 0) { (void)fprintf(stderr, "sdaf-c: failed to write output\n"); return 1; }
    return 0;
}

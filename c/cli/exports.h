#ifndef SDAF_CLI_EXPORTS_H
#define SDAF_CLI_EXPORTS_H

#include "sdaf/sdaf.h"

int sdaf_cli_write_json(FILE *output, const sdaf_document *document);
int sdaf_cli_write_cbor(FILE *output, const sdaf_document *document);
int sdaf_cli_write_csv(FILE *output, const sdaf_document *document);

#endif

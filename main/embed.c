/**
 * Client cert, taken from client.crt
 * Client key, taken from client.key
 * CA cert, taken from ca.crt
 *
 * To embed it in the app binary, the files are named
 * in the component.mk COMPONENT_EMBED_TXTFILES variable.
 * All embedded buffers are NULL terminated.
 */
#include "embed.h"

static uint8_t d_client_crt_start[] asm("_binary_client_crt_start");
static uint8_t d_client_crt_end[]   asm("_binary_client_crt_end");
static uint8_t d_client_key_start[] asm("_binary_client_key_start");
static uint8_t d_client_key_end[]   asm("_binary_client_key_end");
static uint8_t d_ca_crt_start[]     asm("_binary_ca_crt_start");

uint8_t *client_crt_start = d_client_crt_start;
uint8_t *client_crt_end   = d_client_crt_end;
uint8_t *client_key_start = d_client_key_start;
uint8_t *client_key_end   = d_client_key_end;
uint8_t *ca_crt_start = d_ca_crt_start;

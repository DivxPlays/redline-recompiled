/* texpack.c: replacement textures by content (texture packs) and the dump they are made from */
#pragma once
#include <d3d11.h>
#include "pgraph.h"

void texpack_init(ID3D11Device *dev, int enabled, int dump);
void texpack_enable(int on);
int texpack_poll_keys(void); /* build/texkeys.now: 1 when it changed the allowed keys */
void texpack_set_dump(int on);
int texpack_get(uint64_t key, int device, ID3D11ShaderResourceView **srv, DWORD wait_ms); /* 0 none, 1 loading, 2 *srv */
int texpack_has_device_icons(uint64_t key); /* the key has replacements for particular input devices */
void texpack_dump(uint64_t key, const TexImage *img);
void texpack_stats(uint32_t *loads, uint32_t *evictions, int *queued); /* totals so far, and waiting to load */

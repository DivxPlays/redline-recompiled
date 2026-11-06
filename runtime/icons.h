/* icons.c: the button prompts drawn for the device in use */
#pragma once
#include <d3d11.h>
#include "pgraph.h"

int icons_is_prompt(uint64_t key);
void icons_original(uint64_t key, const TexImage *img); /* the game's glyphs, kept when first decoded */
ID3D11ShaderResourceView *icons_get(ID3D11Device *dev, uint64_t key, int state); /* NULL: the game's own */

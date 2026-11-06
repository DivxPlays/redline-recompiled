/* video.c: upscaled cutscenes shown in place of the Bink frames the game copies out */
#pragma once
#include <d3d11.h>
#include <stdint.h>

void video_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int enabled);
ID3D11ShaderResourceView *video_for(uint32_t addr); /* the upscaled frame for a guest buffer, or NULL */
